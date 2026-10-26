#include <gtest/gtest.h>

#include "TestDeviceHelper.h"
#include "AssetCore/GUID.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Mathematics/HalfFloat.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

namespace
{
constexpr uint32_t kSize = 16;
constexpr uint32_t kSamples = 8;
constexpr float kBright = 25.0f; // the helmet's white metal face against the sky, in sky units
constexpr float kDim = 1.0f;

// The curve the resolve's weight inverts: x / (1 + x) on a grey value.
float Tonemap(float x)
{
    return x / (1.0f + x);
}

TextureDesc ColorDesc(uint32_t samples, uint32_t extraUsage)
{
    TextureDesc d{};
    d.width = kSize;
    d.height = kSize;
    d.depth = 1;
    d.mipLevels = 1;
    d.arrayLayers = 1;
    d.sampleCount = samples;
    d.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    d.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) | static_cast<uint32_t>(TextureUsage::ShaderResource) |
              extraUsage;
    return d;
}

// A full-screen draw that writes kBright into sample 0 of each pixel in the left half (gl_SampleMask = 1)
// over a target cleared to kDim: a left-half pixel is one bright sample in eight, a right-half pixel is flat.
PipelineHandle CreateBrightSamplePipeline(IDevice& device, std::string& why)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("ge_msaa_resolve_probe_" + GUID::Generate().ToString());
    ShaderProgramCompileRequest request{};
    request.debugName = "MsaaResolveProbe";
    request.baseDirectory = root;
    request.cacheRoot = root / ".Cache" / "Shaders";
    ShaderStageCompileSpec vertex{};
    vertex.stage = "vs";
    vertex.sourcePath = root / "msaa_resolve_probe.vert";
    vertex.inlineSource = R"(#version 450
void main()
{
    const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
)";
    ShaderStageCompileSpec fragment{};
    fragment.stage = "fs";
    fragment.sourcePath = root / "msaa_resolve_probe.frag";
    fragment.inlineSource = "#version 450\nlayout(location = 0) out vec4 oColor;\nvoid main()\n{\n"
                            "    if (gl_FragCoord.x >= " + std::to_string(kSize / 2) + ".0)\n        discard;\n"
                            "    gl_SampleMask[0] = 1;\n"
                            "    oColor = vec4(vec3(" + std::to_string(kBright) + "), 1.0);\n}\n";
    request.stages = {vertex, fragment};
    ShaderProgramCompileResult result{};
    std::string error;
    const bool compiled = ShaderCompileService::CompileProgramToCache(request, ShaderSourceKind::SpirV, result, &error);
    std::error_code ec;
    fs::remove_all(root, ec);
    if (!compiled || !result.stageBytes.count("vs") || !result.stageBytes.count("fs"))
    {
        why = "probe compile failed: " + error;
        return {};
    }
    PipelineDesc desc{};
    desc.type = PipelineType::Graphics;
    desc.vertexShader = result.stageBytes.at("vs");
    desc.pixelShader = result.stageBytes.at("fs");
    desc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT)};
    desc.rasterizationSamples = kSamples;
    desc.rasterizationState.cullMode = CullModeFlagBits::None;
    desc.depthStencilState.depthTestEnable = false;
    desc.depthStencilState.depthWriteEnable = false;
    desc.debugName = "MsaaResolveProbe";
    return device.CreatePipeline(desc);
}

float RedAt(const ViewReadbackResult& image, uint32_t x, uint32_t y)
{
    constexpr size_t kTexelBytes = 4 * sizeof(uint16_t);
    uint16_t bits = 0;
    std::memcpy(&bits, image.pixels.data() + (static_cast<size_t>(y) * image.width + x) * kTexelBytes, sizeof(bits));
    return Mathematics::HalfToFloat(bits);
}

// One view of an empty scene with an 8x color and its resolve, and the passes the tests declare around
// the world pass. A depth clear declares the world pass of an empty scene; the color is not cleared, so
// the pass loads the samples the test wrote. The targets carry no depth, so nothing is cleared.
class MsaaColorResolveTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device";
        if (m_Device->GetCapabilities().maxMSAASamples < kSamples)
            GTEST_SKIP() << "the device supports fewer than 8 samples";
        if (!ShaderCompileService::IsCompilerAvailable())
            GTEST_SKIP() << "no shader compiler in this build";
        m_Services = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_Services->Initialize(m_Device.get()));
        const CameraId camera = m_Services->Views().AllocateCamera("MsaaResolveCam");
        CameraData cameraData{};
        for (int i = 0; i < 16; i += 5)
        {
            cameraData.view[i] = 1.0f;
            cameraData.proj[i] = 1.0f;
            cameraData.viewProj[i] = 1.0f;
        }
        m_Services->Views().SetCameraData(camera, cameraData);
        m_View = m_Services->Views().AllocateView("MsaaResolveView", camera);
        ViewClearConfig clear{};
        clear.clearDepth = true;
        m_Services->Views().SetViewTargets(m_View, 0, 0, 0, clear);

        std::string why;
        m_Probe = CreateBrightSamplePipeline(*m_Device, why);
        ASSERT_TRUE(m_Probe.IsValid()) << why;
        m_Persistent = std::make_unique<RenderGraph::RGResourcePool>(m_Device.get());
        m_Transient = std::make_unique<RenderGraph::RGTransientPool>(m_Device.get());
        m_Ring = std::make_unique<RenderGraph::RGUploadRing>(m_Device.get(), 2, 65536);
        m_Frame = std::make_unique<RenderGraph::RGFrame>(m_Device.get(), m_Persistent.get(), m_Transient.get(),
                                                        m_Ring.get());
        m_Frame->BeginFrame(0);
        m_Color = m_Frame->ImportPersistentTexture("MsaaResolve.Color", ColorDesc(kSamples, 0));
        m_Resolve = m_Frame->ImportPersistentTexture("MsaaResolve.Resolve",
                                                     ColorDesc(1, static_cast<uint32_t>(TextureUsage::TransferSrc)));
        m_Services->BeginWorldDrawFrame();
        m_Services->BuildWorldBatchKeys();
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Frame.reset();
        m_Ring.reset();
        m_Transient.reset();
        m_Persistent.reset();
        if (m_Probe.IsValid())
            m_Device->DestroyPipeline(m_Probe);
        if (m_OceanSampler.IsValid())
            m_Device->DestroySampler(m_OceanSampler);
        if (m_Services)
            m_Services->Shutdown();
        m_Device->Shutdown();
    }

    // Clears every sample of the color to `value`; with `brightSample`, then writes kBright into
    // sample 0 of each left-half pixel.
    void DeclareSamples(float value, bool brightSample)
    {
        const RenderGraph::RGTexture color = m_Color;
        const PipelineHandle probe = brightSample ? m_Probe : PipelineHandle{};
        m_Frame->AddPass(
            brightSample ? "MsaaResolve.BrightSample" : "MsaaResolve.Clear", PassPhase::kWorldRender,
            [color, value](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                for (int c = 0; c < 3; ++c)
                    ops.Clear.Color[c] = value;
                ops.Clear.Color[3] = 1.0f;
                p.AttachColor(0, color, ops);
            },
            [probe](RenderGraph::RGContext& ctx)
            {
                if (!probe.IsValid())
                    return;
                ctx.Cmd->SetPipeline(probe);
                ctx.Cmd->SetViewport(0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize));
                ctx.Cmd->SetScissor(0, 0, kSize, kSize);
                ctx.Cmd->Draw(3);
            });
    }

    bool DeclareWorld()
    {
        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = m_Color;
        targets.Resolve = m_Resolve;
        const RenderServices::WorldPassRG world = m_Services->AddWorldPassForView(*m_Frame, m_View, targets);
        return world.Pass.IsValid() && world.EffectiveColor.Id == m_Resolve.Id;
    }

    std::shared_ptr<RGReadbackTicket> DeclareReadback(const char* name)
    {
        return RequestTextureReadbackRG(m_Device.get(), *m_Frame, m_Resolve, name);
    }

    void Execute()
    {
        m_Frame->MarkOutput(m_Resolve);
        m_Frame->Execute();
        OnFrameSubmittedReadbacksRG(*m_Frame, m_Frame->SubmissionToken());
        m_Device->WaitForIdle();
    }

    // Declares the ocean's copy of the multisampled color (Shaders/ocean_underwater_resolve.shaderpkg,
    // the program the underwater and reflected-caustics passes run) into the resolve target.
    bool DeclareOceanResolve(std::string& why)
    {
        ShaderPackage package{};
        if (!LoadShaderPkg("Shaders/ocean_underwater_resolve.shaderpkg", m_Device->PreferredShaderSource(), package,
                           &why))
            return false;
        const auto vertex = package.stageBytes.find("vs");
        const auto fragment = package.stageBytes.find("fs");
        if (vertex == package.stageBytes.end() || fragment == package.stageBytes.end())
        {
            why = "the package has no vs/fs stage";
            return false;
        }
        DescriptorSetLayoutDesc layout{};
        layout.debugName = "MsaaResolve.Ocean.Set0";
        layout.bindings = {{0, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment}};
        ApplyMetaImageShapeToLayout(package.meta, layout);
        PipelineDesc pipeline = RenderGraph::MakeFullscreenPipelineDesc("MsaaResolve.Ocean");
        pipeline.vertexShader = vertex->second;
        pipeline.pixelShader = fragment->second;
        pipeline.descriptorSetLayouts.push_back(layout);
        const GraphicsPipelineId pipelineId = PipelineDescTranslator::InternGraphics(*m_Device, pipeline);
        m_OceanSampler = m_Device->CreateSampler(SamplerDesc::PointClamp("MsaaResolve.Ocean.PointClamp"));
        if (!pipelineId.IsValid() || !m_OceanSampler.IsValid())
        {
            why = "the pipeline or the sampler was not created";
            return false;
        }

        RenderGraph::RGFullscreenTextureInput input{};
        input.Texture = m_Color;
        input.Sampler = m_OceanSampler;
        input.Required = true;
        RenderGraph::RGFullscreenDesc desc{};
        desc.Name = "MsaaResolve.Ocean";
        desc.Phase = PassPhase::kWorldRender;
        desc.Pipeline = pipelineId;
        desc.Layout = layout;
        desc.Textures = {&input, 1};
        desc.Target = m_Resolve;
        desc.Ops.Load = RenderGraph::RGLoadOp::DontCare;
        if (RenderGraph::AddFullscreenPass(*m_Frame, desc).IsValid())
            return true;
        why = "the pass was not declared";
        return false;
    }

    uint32_t OverwrittenPassCount() const
    {
        uint32_t count = 0;
        for (RenderGraph::RGPassId p = 0; p < m_Frame->Graph().PassCount(); ++p)
            if (m_Frame->Graph().CullReason(p) == RenderGraph::RGCullReason::Overwritten)
                ++count;
        return count;
    }

    static bool Read(const std::shared_ptr<RGReadbackTicket>& ticket, ViewReadbackResult& image)
    {
        return ticket && ticket->TryGet(image) && image.format == TextureFormat::R16G16B16A16_FLOAT &&
               image.width == kSize;
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<RenderServices> m_Services;
    std::unique_ptr<RenderGraph::RGResourcePool> m_Persistent;
    std::unique_ptr<RenderGraph::RGTransientPool> m_Transient;
    std::unique_ptr<RenderGraph::RGUploadRing> m_Ring;
    std::unique_ptr<RenderGraph::RGFrame> m_Frame;
    ViewId m_View{};
    PipelineHandle m_Probe{};
    SamplerHandle m_OceanSampler{};
    RenderGraph::RGTexture m_Color{};
    RenderGraph::RGTexture m_Resolve{};
};

// The blend of the tonemapped samples: (Tonemap(25) + 7 * Tonemap(1)) / 8.
const float kIdealBlend = (Tonemap(kBright) + (kSamples - 1) * Tonemap(kDim)) / kSamples;

} // namespace

// The world pass resolve on the path every view takes (RenderServices::AddWorldPassForView). One sample
// at 25 and seven at 1 must tonemap to the blend of the tonemapped samples (0.558), not to the plain
// average's 0.80.
TEST_F(MsaaColorResolveTest, OneBrightSampleInEightResolvesToTheTonemappedBlend)
{
    DeclareSamples(kDim, true);
    ASSERT_TRUE(DeclareWorld());
    const auto ticket = DeclareReadback("MsaaResolve.Readback");
    Execute();

    ViewReadbackResult image{};
    ASSERT_TRUE(Read(ticket, image));
    const float plainAverage = (kBright + (kSamples - 1) * kDim) / kSamples;
    const float edge = RedAt(image, kSize / 4, kSize / 2);
    EXPECT_NEAR(Tonemap(edge), kIdealBlend, 0.01f)
        << "resolved " << edge << "; a plain average resolves " << plainAverage << ", which tonemaps to "
        << Tonemap(plainAverage);
    EXPECT_FLOAT_EQ(RedAt(image, 3 * kSize / 4, kSize / 2), kDim) << "a flat pixel resolves to its value";
}

// Each pass that draws into the color declares a resolve. The graph culls one that the next resolve
// overwrites before any read. A read between two resolves keeps the first, and the last always runs.
TEST_F(MsaaColorResolveTest, AResolveReadBeforeTheNextOneRunsAndTheLastOneAlwaysRuns)
{
    constexpr float kLater = 3.0f;
    DeclareSamples(kDim, true);
    ASSERT_TRUE(DeclareWorld());
    const auto between = DeclareReadback("MsaaResolve.ReadBetween");
    DeclareSamples(kLater, false);
    ASSERT_TRUE(DeclareWorld());
    ASSERT_TRUE(DeclareWorld()); // nothing reads the resolve the world pass before this one declared
    const auto last = DeclareReadback("MsaaResolve.ReadLast");
    Execute();

    ViewReadbackResult image{};
    ASSERT_TRUE(Read(between, image));
    EXPECT_NEAR(Tonemap(RedAt(image, kSize / 4, kSize / 2)), kIdealBlend, 0.01f) << "the read resolve ran";
    ASSERT_TRUE(Read(last, image));
    EXPECT_FLOAT_EQ(RedAt(image, kSize / 4, kSize / 2), kLater) << "the last resolve ran";
    EXPECT_FLOAT_EQ(RedAt(image, 3 * kSize / 4, kSize / 2), kLater);
    EXPECT_EQ(OverwrittenPassCount(), 1u) << "the unread resolve is culled";
}

// The ocean's copy of the multisampled color blends like the scene color resolve, so refraction and
// the underwater view see the same edges.
TEST_F(MsaaColorResolveTest, TheOceanCopyResolvesToTheTonemappedBlend)
{
    DeclareSamples(kDim, true);
    std::string why;
    ASSERT_TRUE(DeclareOceanResolve(why)) << why;
    const auto ticket = DeclareReadback("MsaaResolve.OceanReadback");
    Execute();

    ViewReadbackResult image{};
    ASSERT_TRUE(Read(ticket, image));
    const float edge = RedAt(image, kSize / 4, kSize / 2);
    EXPECT_NEAR(Tonemap(edge), kIdealBlend, 0.01f) << "resolved " << edge;
    EXPECT_FLOAT_EQ(RedAt(image, 3 * kSize / 4, kSize / 2), kDim) << "a flat pixel resolves to its value";
}
