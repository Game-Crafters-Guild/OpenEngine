// The reverse-Z convention at the pixel: Metal on macOS (GE_GFX_API=vulkan selects MoltenVK),
// Vulkan elsewhere. Each draw uses the default depth compare and depth clear, and the engine's
// reverse-Z projection, so the rasterizer does the real perspective divide.

#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include "Mathematics/Angle.h"
#include "Mathematics/MatrixOps.h"
#include "Types/Color.h"

#include "TestGlslCompile.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace GameEngine::Rendering;
using namespace GameEngine::Mathematics;

#if RENDERING_HAS_SHADERC
namespace
{

constexpr uint32_t kTargetSize = 16;
constexpr float kFovY = ToRadians(60.0f);
constexpr float kAspect = 1.0f;

// One quad: x from MinX to MaxX in NDC, full height, at the clip-space depth of a view distance.
struct QuadConstants
{
    float ClipZ;
    float ClipW;
    float MinX;
    float MaxX;
    GameEngine::ColorLinear Color;
};
static_assert(sizeof(QuadConstants) == 32, "matches the shaders' push-constant block");

constexpr GameEngine::ColorLinear kRed(1.0f, 0.0f, 0.0f);
constexpr GameEngine::ColorLinear kGreen(0.0f, 1.0f, 0.0f);

// The quad covers the left three quarters of the target; the right column is never drawn.
constexpr float kQuadMinX = -1.0f;
constexpr float kQuadMaxX = 0.5f;
constexpr uint32_t kCoveredColumn = kTargetSize / 4;
constexpr uint32_t kUncoveredColumn = kTargetSize - 1;

const char* kQuadVertexShader = R"(
#version 450
layout(push_constant) uniform Quad
{
    float clipZ;
    float clipW;
    float minX;
    float maxX;
    vec4 color;
} pc;
void main()
{
    const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                                    vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    const vec2 c = corners[gl_VertexIndex];
    const vec2 ndc = vec2(mix(pc.minX, pc.maxX, c.x), mix(-1.0, 1.0, c.y));
    gl_Position = vec4(ndc * pc.clipW, pc.clipZ, pc.clipW);
}
)";

const char* kQuadFragmentShader = R"(
#version 450
layout(push_constant) uniform Quad
{
    float clipZ;
    float clipW;
    float minX;
    float maxX;
    vec4 color;
} pc;
layout(location = 0) out vec4 outColor;
void main() { outColor = pc.color; }
)";

// A quad at viewDistance meters in front of a camera with the given clip planes.
QuadConstants QuadAtDistance(float viewDistance, float zNear, float zFar, const GameEngine::ColorLinear& color)
{
    const Matrix4x4 projection = MakePerspectiveLH_ZO_ReverseZ(kFovY, kAspect, zNear, zFar);
    const Vector4 clip = projection.Transform(Vector4(0.0f, 0.0f, viewDistance, 1.0f));
    return QuadConstants{clip.z, clip.w, kQuadMinX, kQuadMaxX, color};
}

// A color target, a D32 depth target and a pipeline that leaves the depth state at its defaults.
class DepthProbe
{
public:
    bool Initialize()
    {
        DeviceDesc desc{};
        desc.preferredAPI = GraphicsAPI::Auto;
        desc.enableSwapchain = false;
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
        {
            m_Device.reset();
            return false;
        }
        return CreateTargets() && CreatePipeline();
    }

    ~DepthProbe()
    {
        if (!m_Device)
        {
            return;
        }
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(m_Readback);
        m_Device->DestroyTexture(m_Depth);
        m_Device->DestroyTexture(m_Color);
        m_Device->Shutdown();
    }

    // Clears color to blue and depth to the pass default, draws the quads in order, reads back RGBA8.
    std::vector<uint8_t> Render(const std::vector<QuadConstants>& quads)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        RenderPassDesc pass{};
        pass.colorTargets[0] = m_Color;
        pass.colorTargetCount = 1;
        pass.clearColor[0] = true;
        pass.clearColorValue[0][0] = 0.0f;
        pass.clearColorValue[0][1] = 0.0f;
        pass.clearColorValue[0][2] = 1.0f;
        pass.clearColorValue[0][3] = 1.0f;
        pass.depthTarget = m_Depth;
        pass.clearDepth = true;
        cl->BeginRenderPass(pass);
        cl->SetPipeline(m_Pipeline);
        cl->SetViewport(0.0f, 0.0f, static_cast<float>(kTargetSize), static_cast<float>(kTargetSize));
        cl->SetScissor(0, 0, kTargetSize, kTargetSize);
        for (const QuadConstants& quad : quads)
        {
            cl->SetPushConstants(quad);
            cl->Draw(6);
        }
        cl->EndRenderPass();
        cl->CopyTextureToBuffer(m_Color, m_Readback, kTargetSize, kTargetSize);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();

        const auto* mapped = static_cast<const uint8_t*>(m_Device->MapBuffer(m_Readback));
        if (mapped == nullptr)
        {
            return {};
        }
        std::vector<uint8_t> pixels(mapped, mapped + static_cast<size_t>(kTargetSize) * kTargetSize * 4);
        m_Device->UnmapBuffer(m_Readback);
        return pixels;
    }

    GraphicsAPI Api() const { return m_Device->GetAPI(); }

private:
    bool CreateTargets()
    {
        TextureDesc color{};
        color.width = kTargetSize;
        color.height = kTargetSize;
        color.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        color.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
        m_Color = m_Device->CreateTexture(color);

        TextureDesc depth{};
        depth.width = kTargetSize;
        depth.height = kTargetSize;
        depth.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
        depth.usage = static_cast<uint32_t>(TextureUsage::DepthStencil);
        m_Depth = m_Device->CreateTexture(depth);

        BufferDesc readback{};
        readback.size = static_cast<size_t>(kTargetSize) * kTargetSize * 4;
        readback.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
        readback.memoryUsage = BufferMemoryUsage::Readback;
        m_Readback = m_Device->CreateBuffer(readback);
        return m_Color.IsValid() && m_Depth.IsValid() && m_Readback.IsValid();
    }

    bool CreatePipeline()
    {
        const std::vector<uint8_t> vertex =
            Tests::CompileGlsl(kQuadVertexShader, shaderc_vertex_shader, "reverse-z-quad.vert");
        const std::vector<uint8_t> fragment =
            Tests::CompileGlsl(kQuadFragmentShader, shaderc_fragment_shader, "reverse-z-quad.frag");
        if (vertex.empty() || fragment.empty())
        {
            return false;
        }
        PipelineDesc desc{};
        desc.type = PipelineType::Graphics;
        desc.vertexShader = vertex;
        desc.pixelShader = fragment;
        desc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::RGBA8_UNORM)};
        desc.depthAttachmentFormat = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
        desc.rasterizationState.cullMode = CullModeFlagBits::None;
        desc.depthStencilState.depthTestEnable = true;
        desc.depthStencilState.depthWriteEnable = true;
        desc.pushConstantSize = sizeof(QuadConstants);
        desc.pushConstantStagesMask = kShaderStageVertex | kShaderStageFragment;
        desc.debugName = "ReverseZProbeQuad";
        m_Pipeline = m_Device->CreatePipeline(desc);
        return m_Pipeline.IsValid();
    }

    std::unique_ptr<IDevice> m_Device;
    TextureHandle m_Color;
    TextureHandle m_Depth;
    BufferHandle m_Readback;
    PipelineHandle m_Pipeline;
};

std::array<uint8_t, 3> PixelAt(const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y)
{
    const size_t offset = (static_cast<size_t>(y) * kTargetSize + x) * 4;
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
}

constexpr std::array<uint8_t, 3> kRedPixel = {255, 0, 0};
constexpr std::array<uint8_t, 3> kGreenPixel = {0, 255, 0};
constexpr std::array<uint8_t, 3> kBluePixel = {0, 0, 255};

// Every covered pixel shows expected, and the uncovered column keeps the clear color.
void ExpectCoveredPixels(const std::vector<uint8_t>& pixels, const std::array<uint8_t, 3>& expected,
                         const char* what)
{
    ASSERT_EQ(pixels.size(), static_cast<size_t>(kTargetSize) * kTargetSize * 4) << what << ": no readback";
    for (uint32_t y = 0; y < kTargetSize; ++y)
    {
        EXPECT_EQ(PixelAt(pixels, kCoveredColumn, y), expected) << what << ", row " << y;
        EXPECT_EQ(PixelAt(pixels, kUncoveredColumn, y), kBluePixel) << what << ": untouched pixel, row " << y;
    }
}

const char* ApiName(GraphicsAPI api)
{
    switch (api)
    {
    case GraphicsAPI::Vulkan:
        return "Vulkan";
    case GraphicsAPI::DirectX12:
        return "DirectX12";
    case GraphicsAPI::Metal:
        return "Metal";
    case GraphicsAPI::WebGPU:
        return "WebGPU";
    case GraphicsAPI::Auto:
        break;
    }
    return "unknown";
}

// The probe on the platform's device; the XML records which backend ran.
std::unique_ptr<DepthProbe> CreateProbe()
{
    auto probe = std::make_unique<DepthProbe>();
    if (!probe->Initialize())
    {
        return nullptr;
    }
    ::testing::Test::RecordProperty("graphicsApi", ApiName(probe->Api()));
    return probe;
}

} // namespace

TEST(ReverseZProbe, TheNearerSurfaceWinsInEitherDrawOrder)
{
    const auto probe = CreateProbe();
    if (!probe)
    {
        GTEST_SKIP() << "no graphics device";
    }
    constexpr float kNear = 0.1f;
    constexpr float kFar = 1000.0f;
    const QuadConstants nearer = QuadAtDistance(2.0f, kNear, kFar, kRed);
    const QuadConstants farther = QuadAtDistance(50.0f, kNear, kFar, kGreen);
    ASSERT_GT(nearer.ClipZ / nearer.ClipW, farther.ClipZ / farther.ClipW) << "reverse-Z: the nearer depth is larger";

    ExpectCoveredPixels(probe->Render({farther, nearer}), kRedPixel, "far then near");
    ExpectCoveredPixels(probe->Render({nearer, farther}), kRedPixel, "near then far");
}

TEST(ReverseZProbe, GeometryAtTheFarEndPassesAgainstTheClearedDepth)
{
    const auto probe = CreateProbe();
    if (!probe)
    {
        GTEST_SKIP() << "no graphics device";
    }
    // 999 m with a 1000 m far plane: a depth just above the clear value.
    const QuadConstants farEnd = QuadAtDistance(999.0f, 0.1f, 1000.0f, kGreen);
    ASSERT_GT(farEnd.ClipZ / farEnd.ClipW, RenderPassDesc{}.clearDepthValue);

    ExpectCoveredPixels(probe->Render({farEnd}), kGreenPixel, "far-end quad over the clear");
}

TEST(ReverseZProbe, AFaceTwoCentimetersInFrontOfAWallNineHundredMetersAwayStaysSolid)
{
    const auto probe = CreateProbe();
    if (!probe)
    {
        GTEST_SKIP() << "no graphics device";
    }
    // With near 0.1 and far 1000 the 2 cm gap is about 2700 float steps of reverse-Z depth.
    // Forward-Z puts both surfaces at depth ~0.99999, where the gap is about 1/24 of a step,
    // so they would get the same depth and the wall drawn second would cover the face.
    const QuadConstants face = QuadAtDistance(899.98f, 0.1f, 1000.0f, kRed);
    const QuadConstants wall = QuadAtDistance(900.0f, 0.1f, 1000.0f, kGreen);
    ASSERT_GT(face.ClipZ / face.ClipW, wall.ClipZ / wall.ClipW);

    ExpectCoveredPixels(probe->Render({face, wall}), kRedPixel, "face then wall");
    ExpectCoveredPixels(probe->Render({wall, face}), kRedPixel, "wall then face");
}
#endif
