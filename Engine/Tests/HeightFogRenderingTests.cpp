// Execute the production height fog pass over hand-written depth values and read back what it
// writes. The pass decides from the raw reverse-Z depth whether a pixel is sky, which it leaves
// alone (or gives sky fog), or geometry, which it fogs by distance. These tests pin where that
// line falls: the cleared depth of 0 is sky, and everything above it is geometry, however close
// to the far plane. Geometry in the last metres before the far plane has a raw depth below 1e-6,
// which a threshold there took for sky and left unfogged.
//
// Each case renders one pixel down the centre of the view, so its view distance is its view z.
// With the fog colour green, a red scene and no sun, the output is (T, 1 - T, 0, T) for the
// fog's transmittance T, so the green channel reads the fog's opacity directly.
//
// Skips ONLY without a Vulkan device. A missing production shader FAILS: the same target builds it.
#include <gtest/gtest.h>

#include "Engine/Rendering/HeightFogParamsUBO.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;
using GameEngine::Engine::Renderer::HeightFogColorSource;
using GameEngine::Engine::Renderer::HeightFogFlagDistanceEnabled;
using GameEngine::Engine::Renderer::HeightFogFlagSkyEnabled;
using GameEngine::Engine::Renderer::HeightFogParamsUBO;

namespace
{
using Pixel = std::array<float, 4>;

constexpr float kNear = 0.5f;
constexpr float kFar = 1800.0f;
// The threshold the pass used to take for sky.
constexpr float kOldSkyThreshold = 1e-6f;

void Require(bool condition, const std::string& why)
{
    if (!condition) throw std::runtime_error(why);
}

GameEngine::Mathematics::Matrix4x4 Projection()
{
    return GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0f, 1.0f, kNear, kFar);
}

// The raw depth the engine's projection writes for geometry at view z.
float DepthAt(float viewZ)
{
    const float* m = Projection().Data();
    return (m[10] * viewZ + m[14]) / viewZ;
}

class HeightFogRendering : public ::testing::Test
{
protected:
    void SetUp() override
    {
#ifdef _WIN32
        _putenv_s("GE_HEADLESS_TEST", "1");
#else
        setenv("GE_HEADLESS_TEST", "1", 1);
#endif
        Utils::SetShaderPathResolver(+[](const std::filesystem::path& rel)
        {
            return std::filesystem::path(RENDERING_SHADER_OUTPUT_DIR) / rel;
        });
        DeviceDesc dd{};
        dd.preferredAPI = GraphicsAPI::Vulkan;
        dd.enableDynamicRendering = true;
        m_Device = DeviceFactory::CreateDevice(dd);
        if (!m_Device || !m_Device->Initialize(dd))
            GTEST_SKIP() << "No usable Vulkan device";
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("HeightFogTests"));
        ASSERT_TRUE(m_Sampler.IsValid());

        BufferDesc bd{};
        bd.size = sizeof(ViewParamsUBO);
        bd.usage = static_cast<uint32_t>(BufferUsage::Uniform);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        m_View = m_Device->CreateBuffer(bd);
        bd.size = sizeof(HeightFogParamsUBO);
        m_Fog = m_Device->CreateBuffer(bd);
        ASSERT_TRUE(m_View.IsValid() && m_Fog.IsValid());

        // The camera at the origin looking down +z, with the identity view.
        ViewParamsUBO view{};
        const auto invProj = GameEngine::Mathematics::Inverse(Projection());
        std::memcpy(view.ge_invProj, invProj.Data(), sizeof(view.ge_invProj));
        for (int i = 0; i < 4; ++i) view.ge_view[i * 5] = 1.0f;
        view.ge_nearFar[0] = kNear;
        view.ge_nearFar[1] = kFar;
        m_Device->UpdateBuffer(m_View, 0, sizeof(view), &view);

        m_Scene = Upload({1, 0, 0, 1});
        m_Backdrop = Upload({0, 0, 0, 1});
        BuildPipeline();
    }

    void TearDown() override
    {
        if (!m_Device) return;
        m_Device->WaitForIdle();
        for (auto image : m_Images) m_Device->DestroyTexture(image);
        if (m_Pipeline.IsValid()) m_Device->DestroyPipeline(m_Pipeline);
        if (m_Sampler.IsValid()) m_Device->DestroySampler(m_Sampler);
        if (m_View.IsValid()) m_Device->DestroyBuffer(m_View);
        if (m_Fog.IsValid()) m_Device->DestroyBuffer(m_Fog);
        m_Device->Shutdown();
    }

    // Distance fog only, green, reaching the far plane: a fourth of the way to opaque per
    // fourth of the far distance, with no sun and no sky fog unless a test turns it on.
    static HeightFogParamsUBO DistanceFog()
    {
        HeightFogParamsUBO fog{};
        fog.intensity = 1.0f;
        fog.density = 1.0f;
        fog.maxOpacity = 1.0f;
        fog.minDistance = 0.0f;
        fog.smoothLength = 1.0f;
        fog.maxDistance = kFar;
        fog.flags = HeightFogFlagDistanceEnabled;
        fog.emissiveR = 0.0f;
        fog.emissiveG = 1.0f;
        fog.emissiveB = 0.0f;
        fog.sunIntensity = 0.0f;
        fog.colorSource = static_cast<int32_t>(HeightFogColorSource::Fixed);
        return fog;
    }

    TextureHandle Allocate()
    {
        TextureDesc td{};
        td.width = td.height = td.depth = 1;
        td.mipLevels = td.arrayLayers = td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget |
                                         TextureUsage::TransferSrc | TextureUsage::TransferDst);
        td.debugName = "HeightFogTests.Image";
        auto image = m_Device->CreateTexture(td);
        Require(image.IsValid(), "image allocation failed");
        m_Images.push_back(image);
        return image;
    }

    TextureHandle Upload(Pixel pixel)
    {
        auto image = Allocate();
        auto upload = m_Device->CreateUploadBuffer(sizeof(Pixel), "HeightFogTests.Upload");
        Require(upload.IsValid(), "upload allocation failed");
        m_Device->UpdateBuffer(upload, 0, sizeof(Pixel), pixel.data());
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(image, ResourceState::Undefined, ResourceState::CopyDest));
        cl->CopyBufferToTextureSubresource(upload, image, 0, 0, 1, 1, 0, sizeof(Pixel));
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(image, ResourceState::CopyDest, ResourceState::ShaderResource));
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(upload);
        return image;
    }

    void BuildPipeline()
    {
        auto vs = Utils::LoadShaderFile("fullscreen_noinput.vert.spv");
        auto fs = Utils::LoadShaderFile("height_fog.frag.spv");
        Require(!vs.empty() && !fs.empty(), "missing production shader: height_fog");
        StageReflectionResult rvs{}, rfs{};
        ReflectionOptions opts{};
        Require(ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vs.data()),
                            vs.size() / 4, opts, rvs, nullptr), "vertex reflection failed");
        Require(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fs.data()),
                            fs.size() / 4, opts, rfs, nullptr), "fragment reflection failed");
        m_Meta = MergeStages({rvs, rfs});
        Require(m_Meta.Sets.size() == 1 && m_Meta.Sets[0].Set == 0, "unexpected height fog descriptor sets");
        m_Desc.type = PipelineType::Graphics;
        m_Desc.vertexShader = vs;
        m_Desc.pixelShader = fs;
        MaterialBuilder::FormatsHint formats{};
        formats.ColorFormats = {static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT)};
        std::string why;
        Require(MaterialBuilder::BuildPipelineDescFromMeta(m_Meta, m_Desc, formats,
            MaterialBuilder::MergeMode::Auto, MaterialBuilder::PushConstantPolicy{}, &why), why);
        m_Pipeline = m_Device->CreatePipeline(m_Desc);
        Require(m_Pipeline.IsValid(), "pipeline creation failed: height_fog");
    }

    // Runs the pass on one pixel of the red scene at the given raw depth.
    Pixel Render(float rawDepth, const HeightFogParamsUBO& fog)
    {
        m_Device->UpdateBuffer(m_Fog, 0, sizeof(fog), &fog);
        auto depth = Upload({rawDepth, 0, 0, 0});
        DescriptorSetDesc sd{};
        sd.layout = m_Desc.descriptorSetLayouts[0];
        sd.transient = false;
        auto ds = m_Device->CreateDescriptorSet(sd);
        Require(ds.IsValid(), "descriptor allocation failed");
        for (const auto& binding : m_Meta.Sets[0].Bindings)
        {
            switch (binding.Binding)
            {
            case 0: m_Device->UpdateCombinedImageSamplerBinding(ds, 0, m_Scene, m_Sampler); break;
            case 1: m_Device->UpdateCombinedImageSamplerBinding(ds, 1, depth, m_Sampler); break;
            case 2: m_Device->UpdateBufferBinding(ds, 2, m_View, 0, sizeof(ViewParamsUBO)); break;
            case 3: m_Device->UpdateBufferBinding(ds, 3, m_Fog, 0, sizeof(HeightFogParamsUBO)); break;
            case 4: m_Device->UpdateCombinedImageSamplerBinding(ds, 4, m_Backdrop, m_Sampler); break;
            default: throw std::runtime_error("unexpected height fog binding " + binding.Name);
            }
        }

        auto target = Allocate();
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc rp{};
        rp.colorTargets[0] = target;
        rp.colorTargetCount = 1;
        rp.clearColor[0] = true;
        rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        cl->BeginRenderPass(rp);
        cl->SetViewport(0.0f, 0.0f, 1.0f, 1.0f);
        cl->SetScissor(0, 0, 1, 1);
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->Draw(3, 1);
        cl->EndRenderPass();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::RenderTarget, ResourceState::CopySource));
        auto rb = m_Device->CreateReadbackBuffer(sizeof(Pixel), "HeightFogTests.Readback");
        Require(rb.IsValid(), "readback allocation failed");
        cl->CopyTextureToBuffer(target, rb, 1, 1, 0, 0, 0, 0);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        m_Device->DestroyDescriptorSet(ds);

        Pixel px{};
        auto* data = m_Device->MapBuffer(rb);
        Require(data != nullptr, "readback map failed");
        std::memcpy(px.data(), data, sizeof(px));
        m_Device->UnmapBuffer(rb);
        m_Device->DestroyBuffer(rb);
        for (float channel : px) Require(std::isfinite(channel), "nonfinite height fog output");
        return px;
    }

    std::unique_ptr<IDevice> m_Device;
    SamplerHandle m_Sampler{};
    BufferHandle m_View{}, m_Fog{};
    TextureHandle m_Scene{}, m_Backdrop{};
    PipelineHandle m_Pipeline{};
    PipelineDesc m_Desc{};
    ShaderMeta m_Meta{};
    std::vector<TextureHandle> m_Images;
};

TEST_F(HeightFogRendering, FogThickensRightUpToTheFarPlane)
{
    // The last of these are geometry the old threshold took for sky.
    const std::vector<float> distances{1500.0f, 1700.0f, 1790.0f, 1795.0f, 1799.0f, 1799.9f};
    ASSERT_LE(DepthAt(1795.0f), kOldSkyThreshold);
    ASSERT_GT(DepthAt(1799.9f), 0.0f);

    float previous = 0.0f;
    for (float distance : distances)
    {
        const float depth = DepthAt(distance);
        const Pixel px = Render(depth, DistanceFog());
        const float opacity = px[1];
        EXPECT_GE(opacity, previous - 1e-5f) << "at " << distance << " (raw depth " << depth << ")";
        EXPECT_NEAR(px[0] + px[1], 1.0f, 1e-4f) << "at " << distance;
        EXPECT_NEAR(px[3], px[0], 1e-4f) << "the alpha carries the transmittance, at " << distance;
        previous = opacity;
    }
    // Optical depth 4 at the far plane leaves 1 - e^-4 of the scene behind the fog.
    EXPECT_GT(previous, 0.97f);
}

TEST_F(HeightFogRendering, ClearedDepthIsSkyAndKeepsTheSceneWithoutSkyFog)
{
    const Pixel px = Render(0.0f, DistanceFog());
    EXPECT_EQ(px[0], 1.0f);
    EXPECT_EQ(px[1], 0.0f);
    EXPECT_EQ(px[2], 0.0f);
    EXPECT_EQ(px[3], 1.0f);
}

TEST_F(HeightFogRendering, ClearedDepthTakesSkyFogWhenItIsOn)
{
    // Looking along the horizon, where the sky fog is the fog's full sky amount: 1 - e^-4 at
    // full density.
    HeightFogParamsUBO fog = DistanceFog();
    fog.flags |= HeightFogFlagSkyEnabled;
    const Pixel px = Render(0.0f, fog);
    EXPECT_NEAR(px[1], 1.0f - std::exp(-4.0f), 1e-4f);
    EXPECT_NEAR(px[0], std::exp(-4.0f), 1e-4f);
}

} // namespace
