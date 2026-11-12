#include <gtest/gtest.h>

#include "TestDeviceHelper.h"
#include "AssetCore/GUID.h"
#include "Components/Rendering/Look.h"
#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Mathematics/HalfFloat.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/Pipeline/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Materials/Shader/ShaderCompileService.h"
#include "Rendering/Materials/Shader/ShaderLayoutShape.h"
#include "Rendering/Passes/MsaaColorResolvePass.h"
#include "Rendering/Passes/TonemapPass.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestCameraEye.h"
#include "Types/Color.h"

#include <algorithm>
#include <cmath>
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
constexpr uint32_t kMaxSamples = 8;
constexpr uint32_t kForcedSamples = 4; // every Apple GPU and Mesa lavapipe stop at 4x
constexpr float kBright = 25.0f; // the helmet's white metal face against the sky, in sky units
constexpr float kDim = 1.0f;

// The Neutral look on a grey (tonemap.frag's TonemapNeutral): the curve the resolve inverts for a view
// with no look of its own, at exposure 1.
float Tonemap(float x)
{
    const float v = x < 0.08f ? 6.25f * x * x : x - 0.04f;
    return v < 0.76f ? v : 1.0f - 0.0576f / (v - 0.52f);
}

// The Karis curve x / (1 + x) on a grey: the curve the ocean's copy of the color inverts.
float KarisTonemap(float x)
{
    return x / (1.0f + x);
}

// The Filmic look on a grey: the ACES 2 tonescale at the SDR tier, with tsFScale and tsSDiv x 2^(1/3) from tier 0
// of TonemapAces2Tables.h (TonemapHdrHeadroomTests checks the curve against tonemap.frag).
float FilmicTonemap(float x)
{
    const float f = 1.04710376f * std::pow(x / (x + 0.730092108f * std::cbrt(2.0f)), 1.15f);
    return std::min(f * f / (f + 0.04f), 1.0f);
}

// The Photographic look on a grey: tonemap.frag's AgX sigmoid. The inset and outset matrices keep a grey grey.
float PhotographicTonemap(float x)
{
    const float a = std::clamp((std::log2(std::max(x, 1e-10f)) + 12.47393f) / (4.026069f + 12.47393f), 0.0f, 1.0f);
    const float a2 = a * a;
    const float a4 = a2 * a2;
    const float sigmoid = 15.5f * a4 * a2 - 40.14f * a4 * a + 31.96f * a4 - 6.868f * a2 * a + 0.4298f * a2 +
                          0.1191f * a - 0.00232f;
    return std::pow(std::max(sigmoid, 0.0f), 2.2f);
}

// tonemap.frag's look values: Components::Look and the no-look arm.
enum class Look : int32_t
{
    Neutral = static_cast<int32_t>(Components::Look::Neutral),
    Filmic = static_cast<int32_t>(Components::Look::Filmic),
    Photographic = static_cast<int32_t>(Components::Look::Photographic),
    None = Passes::kTonemapNoLookArm,
};

// The display light a look shows for a grey of exposed luminance x.
float LookTonemap(Look look, float x)
{
    switch (look)
    {
    case Look::Filmic:
        return FilmicTonemap(x);
    case Look::Photographic:
        return PhotographicTonemap(x);
    case Look::None:
        return x;
    default:
        return Tonemap(x);
    }
}

// sRGB-encoded 8-bit display levels of display-linear light in [0, 1].
float EncodedLevels(float light)
{
    const float v = std::clamp(light, 0.0f, 1.0f);
    return 255.0f * (v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f);
}

// Rec.709 luma, the luminance the resolve weights by.
float Luma(const ColorLinear& color)
{
    return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
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

// The blend of the tonemapped samples of a pixel with one sample at kBright and the rest at kDim.
float IdealBlend(uint32_t samples)
{
    return (Tonemap(kBright) + static_cast<float>(samples - 1) * Tonemap(kDim)) / static_cast<float>(samples);
}

// The same blend under the Karis curve.
float IdealKarisBlend(uint32_t samples)
{
    return (KarisTonemap(kBright) + static_cast<float>(samples - 1) * KarisTonemap(kDim)) /
           static_cast<float>(samples);
}

// The plain average of the samples of a pixel with one sample at kBright and the rest at kDim.
float PlainAverage(uint32_t samples)
{
    return (kBright + static_cast<float>(samples - 1) * kDim) / static_cast<float>(samples);
}

// The tonemap of the plain average of the same samples, which the weighted resolve must not give.
float PlainAverageTonemapped(uint32_t samples)
{
    return Tonemap(PlainAverage(samples));
}

// A full-screen draw that writes `bright` into sample 0 of each pixel in the left half (gl_SampleMask = 1)
// over a cleared target: a left-half pixel is one bright sample in `samples`, a right-half pixel is flat.
PipelineHandle CreateBrightSamplePipeline(IDevice& device, uint32_t samples, const ColorLinear& bright,
                                          std::string& why)
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
                            "    if (gl_FragCoord.x >= " +
                            std::to_string(kSize / 2) + ".0)\n        discard;\n"
                                                        "    gl_SampleMask[0] = 1;\n"
                                                        "    oColor = vec4(" +
                            std::to_string(bright.r) + ", " + std::to_string(bright.g) +
                            ", " + std::to_string(bright.b) + ", 1.0);\n}\n";
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
    desc.rasterizationSamples = samples;
    desc.rasterizationState.cullMode = CullModeFlagBits::None;
    desc.depthStencilState.depthTestEnable = false;
    desc.depthStencilState.depthWriteEnable = false;
    desc.debugName = "MsaaResolveProbe";
    return device.CreatePipeline(desc);
}

float ChannelAt(const ViewReadbackResult& image, uint32_t x, uint32_t y, uint32_t channel)
{
    constexpr size_t kTexelBytes = 4 * sizeof(uint16_t);
    uint16_t bits = 0;
    std::memcpy(&bits,
                image.pixels.data() + (static_cast<size_t>(y) * image.width + x) * kTexelBytes +
                    channel * sizeof(uint16_t),
                sizeof(bits));
    return Mathematics::HalfToFloat(bits);
}

float RedAt(const ViewReadbackResult& image, uint32_t x, uint32_t y)
{
    return ChannelAt(image, x, y, 0);
}

// One view of an empty scene with a multisampled color and its resolve, and the passes the tests declare
// around the world pass. The parameter caps the sample count; the test runs at min(cap, device maximum).
// A depth clear declares the world pass of an empty scene; the color is not cleared, so the pass loads
// the samples the test wrote. The targets carry no depth, so nothing is cleared.
class MsaaColorResolveTest : public testing::TestWithParam<uint32_t>
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device";
        m_Samples = std::min(GetParam(), m_Device->GetCapabilities().maxMSAASamples);
        if (m_Samples < 2)
            GTEST_SKIP() << "the device has no multisampled color";
        if (!ShaderCompileService::IsCompilerAvailable())
            GTEST_SKIP() << "no shader compiler in this build";
        m_Services = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_Services->Initialize(m_Device.get()));
        const CameraId camera = m_Services->Views().AllocateCamera("MsaaResolveCam");
        CameraData cameraData{};
        cameraData.view = Mathematics::Matrix4x4::Identity();
        cameraData.proj = Mathematics::Matrix4x4::Identity();
        cameraData.viewProj = Mathematics::Matrix4x4::Identity();
        m_Services->Views().SetCameraData(camera, ::GameEngine::Tests::TestCameraWithEye(cameraData), {0.1f, 1000.0f});
        m_View = m_Services->Views().AllocateView("MsaaResolveView", camera);
        ViewClearConfig clear{};
        clear.clearDepth = true;
        m_Services->Views().SetViewTargets(m_View, 0, 0, 0, clear);

        std::string why;
        m_Probe = CreateBrightSamplePipeline(*m_Device, m_Samples, ColorLinear(kBright, kBright, kBright), why);
        ASSERT_TRUE(m_Probe.IsValid()) << why;
        m_Persistent = std::make_unique<RenderGraph::RGResourcePool>(m_Device.get());
        m_Transient = std::make_unique<RenderGraph::RGTransientPool>(m_Device.get());
        m_Ring = std::make_unique<RenderGraph::RGUploadRing>(m_Device.get(), 2, 65536);
        m_Frame = std::make_unique<RenderGraph::RGFrame>(m_Device.get(), m_Persistent.get(), m_Transient.get(),
                                                        m_Ring.get());
        m_Frame->BeginFrame(0);
        m_Color = m_Frame->ImportPersistentTexture("MsaaResolve.Color", ColorDesc(m_Samples, 0));
        m_Resolve = m_Frame->ImportPersistentTexture("MsaaResolve.Resolve",
                                                     ColorDesc(1, static_cast<uint32_t>(TextureUsage::TransferSrc)));
        m_BloomSampleDelta = m_Frame->ImportPersistentTexture(
            "MsaaResolve.BloomSampleDelta", ColorDesc(1, static_cast<uint32_t>(TextureUsage::TransferSrc)));
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
        if (m_ColoredProbe.IsValid())
            m_Device->DestroyPipeline(m_ColoredProbe);
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
        DeclareSamplesWith(value, brightSample ? m_Probe : PipelineHandle{});
    }

    // Clears every sample of the color to `value`, then draws `probe` when it is valid.
    void DeclareSamplesWith(float value, PipelineHandle probe)
    {
        const RenderGraph::RGTexture color = m_Color;
        m_Frame->AddPass(
            probe.IsValid() ? "MsaaResolve.BrightSample" : "MsaaResolve.Clear", PassPhase::kWorldRender,
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

    bool DeclareWorld(bool withBloomSampleDelta = false)
    {
        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = m_Color;
        targets.Resolve = m_Resolve;
        if (withBloomSampleDelta)
            targets.BloomSampleDelta = m_BloomSampleDelta;
        const RenderServices::WorldPassRG world = m_Services->AddWorldPassForView(*m_Frame, m_View, targets);
        return world.Pass.IsValid() && world.EffectiveColor.Id == m_Resolve.Id;
    }

    std::shared_ptr<RGReadbackTicket> DeclareReadback(const char* name)
    {
        return RequestTextureReadbackRG(m_Device.get(), *m_Frame, m_Resolve, name);
    }

    std::shared_ptr<RGReadbackTicket> DeclareDeltaReadback(const char* name)
    {
        return RequestTextureReadbackRG(m_Device.get(), *m_Frame, m_BloomSampleDelta, name);
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
    uint32_t m_Samples = 0;
    ViewId m_View{};
    PipelineHandle m_Probe{};
    PipelineHandle m_ColoredProbe{};
    SamplerHandle m_OceanSampler{};
    RenderGraph::RGTexture m_Color{};
    RenderGraph::RGTexture m_Resolve{};
    RenderGraph::RGTexture m_BloomSampleDelta{};
};

std::string SampleCapName(const testing::TestParamInfo<uint32_t>& info)
{
    return "UpTo" + std::to_string(info.param) + "x";
}

} // namespace

// The world pass resolve on the path every view takes (RenderServices::AddWorldPassForView), with no pipeline:
// the Neutral look at exposure 1. One sample at 25 and the rest at 1 tonemap to the blend of the tonemapped
// samples (0.885 at 8x, 0.901 at 4x), not to the tonemap of the plain average (0.983 at 8x, 0.990 at 4x).
TEST_P(MsaaColorResolveTest, OneBrightSampleResolvesToTheTonemappedBlend)
{
    DeclareSamples(kDim, true);
    ASSERT_TRUE(DeclareWorld());
    const auto ticket = DeclareReadback("MsaaResolve.Readback");
    Execute();

    ViewReadbackResult image{};
    ASSERT_TRUE(Read(ticket, image));
    const float edge = RedAt(image, kSize / 4, kSize / 2);
    EXPECT_NEAR(Tonemap(edge), IdealBlend(m_Samples), 0.01f)
        << m_Samples << "x resolved " << edge << "; a plain average tonemaps to " << PlainAverageTonemapped(m_Samples);
    EXPECT_FLOAT_EQ(RedAt(image, 3 * kSize / 4, kSize / 2), kDim) << "a flat pixel resolves to its value";
}

// With a bloom sample delta target, the resolve still writes the weighted mean, and the delta is the
// plain average minus it: bloom adds the two and gets the plain average (4.0 at 8x, 6.25 at 4x).
TEST_P(MsaaColorResolveTest, TheBloomSampleDeltaAddsTheWeightedMeanUpToThePlainAverage)
{
    DeclareSamples(kDim, true);
    ASSERT_TRUE(DeclareWorld(true));
    const auto resolved = DeclareReadback("MsaaResolve.WeightedReadback");
    const auto delta = DeclareDeltaReadback("MsaaResolve.DeltaReadback");
    Execute();

    ViewReadbackResult weightedImage{};
    ASSERT_TRUE(Read(resolved, weightedImage));
    ViewReadbackResult deltaImage{};
    ASSERT_TRUE(Read(delta, deltaImage));
    const float weighted = RedAt(weightedImage, kSize / 4, kSize / 2);
    EXPECT_NEAR(Tonemap(weighted), IdealBlend(m_Samples), 0.01f) << "the scene color keeps the weighted mean";
    const float bloomInput = weighted + RedAt(deltaImage, kSize / 4, kSize / 2);
    EXPECT_NEAR(bloomInput, PlainAverage(m_Samples), 0.01f * PlainAverage(m_Samples))
        << m_Samples << "x: weighted " << weighted << " plus the delta gives " << bloomInput;
    EXPECT_FLOAT_EQ(RedAt(deltaImage, 3 * kSize / 4, kSize / 2), 0.0f) << "a flat pixel has no delta";
}

// Each pass that draws into the color declares a resolve. The graph culls one that the next resolve
// overwrites before any read. A read between two resolves keeps the first, and the last always runs.
TEST_P(MsaaColorResolveTest, AResolveReadBeforeTheNextOneRunsAndTheLastOneAlwaysRuns)
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
    EXPECT_NEAR(Tonemap(RedAt(image, kSize / 4, kSize / 2)), IdealBlend(m_Samples), 0.01f) << "the read resolve ran";
    ASSERT_TRUE(Read(last, image));
    EXPECT_FLOAT_EQ(RedAt(image, kSize / 4, kSize / 2), kLater) << "the last resolve ran";
    EXPECT_FLOAT_EQ(RedAt(image, 3 * kSize / 4, kSize / 2), kLater);
    EXPECT_EQ(OverwrittenPassCount(), 1u) << "the unread resolve is culled";
}

// The ocean's copy of the multisampled color takes the Karis-weighted mean, the blend under x / (1 + x).
TEST_P(MsaaColorResolveTest, TheOceanCopyResolvesToTheTonemappedBlend)
{
    DeclareSamples(kDim, true);
    std::string why;
    ASSERT_TRUE(DeclareOceanResolve(why)) << why;
    const auto ticket = DeclareReadback("MsaaResolve.OceanReadback");
    Execute();

    ViewReadbackResult image{};
    ASSERT_TRUE(Read(ticket, image));
    const float edge = RedAt(image, kSize / 4, kSize / 2);
    EXPECT_NEAR(KarisTonemap(edge), IdealKarisBlend(m_Samples), 0.01f) << m_Samples << "x resolved " << edge;
    EXPECT_FLOAT_EQ(RedAt(image, 3 * kSize / 4, kSize / 2), kDim) << "a flat pixel resolves to its value";
}

// The resolve with a view's own buffers: one colored bright sample over grey samples, each look, E / P = 0.25.
// The pixel shows the mean display light of its samples, T(E / P * L_out) = mean T(E / P * L_i), with each
// sample's chromaticity weighted by its light. The Photographic curve is a fit to AgX: its tolerance is wider.
TEST_P(MsaaColorResolveTest, EachPixelShowsTheMeanLightOfItsSamplesUnderTheViewsLookAndExposure)
{
    constexpr ColorLinear kColored(100.0f, 50.0f, 20.0f);
    constexpr float kGrey = 4.0f;
    constexpr float kMeteredExposure = 0.5f;
    constexpr float kPreExposure = 2.0f;
    constexpr float kScale = kMeteredExposure / kPreExposure;
    constexpr Look kLooks[] = {Look::Neutral, Look::Filmic, Look::Photographic, Look::None};
    // In sRGB-encoded 8-bit levels; the fitted curve's error adds to the half-float rounding.
    constexpr float kLevelTolerance = 0.5f;
    constexpr float kFittedLevelTolerance = 1.5f;
    // On each channel of the color divided by its luma.
    constexpr float kChromaTolerance = 0.005f;
    constexpr float kFittedChromaTolerance = 0.015f;
    constexpr float kRelativeTolerance = 2e-3f;

    std::string why;
    m_ColoredProbe = CreateBrightSamplePipeline(*m_Device, m_Samples, kColored, why);
    ASSERT_TRUE(m_ColoredProbe.IsValid()) << why;
    DeclareSamplesWith(kGrey, m_ColoredProbe);

    const auto exposure = m_Frame->AllocUpload<ExposureReadbackFeature::State>();
    ASSERT_TRUE(exposure.Valid());
    *exposure.Ptr = {kMeteredExposure, 1u, kPreExposure, kPreExposure};
    std::shared_ptr<RGReadbackTicket> tickets[std::size(kLooks)];
    for (size_t lookIndex = 0; lookIndex < std::size(kLooks); ++lookIndex)
    {
        const auto viewParams = m_Frame->AllocUpload<ViewParamsUBO>();
        ASSERT_TRUE(viewParams.Valid());
        *viewParams.Ptr = ViewParamsUBO{};
        viewParams.Ptr->ge_exposureParams[0] = 9.0f; // the static scale, which metering overrides
        viewParams.Ptr->ge_exposureParams[1] = 1.0f;
        viewParams.Ptr->ge_exposureParams[2] = static_cast<float>(kLooks[lookIndex]);
        Passes::MsaaResolveViewBuffers view{};
        view.ViewParams.Buffer = viewParams.Buffer;
        view.ViewParams.Offset = viewParams.Offset;
        view.ViewParams.Size = sizeof(ViewParamsUBO);
        view.ExposureHistory.Buffer = exposure.Buffer;
        view.ExposureHistory.Offset = exposure.Offset;
        view.ExposureHistory.Size = sizeof(ExposureReadbackFeature::State);
        const std::string name = "MsaaResolve.Look" + std::to_string(lookIndex);
        const RenderGraph::RGTexture resolved = m_Frame->ImportPersistentTexture(
            (name + ".Target").c_str(), ColorDesc(1, static_cast<uint32_t>(TextureUsage::TransferSrc)));
        ASSERT_TRUE(Passes::AddMsaaColorResolvePassRG(*m_Frame, m_Color, resolved, {}, view, name.c_str()).IsValid());
        tickets[lookIndex] =
            RequestTextureReadbackRG(m_Device.get(), *m_Frame, resolved, (name + ".Readback").c_str());
        m_Frame->MarkOutput(resolved);
    }
    Execute();

    const float coloredLuma = Luma(kColored);
    const float samples = static_cast<float>(m_Samples);
    for (size_t lookIndex = 0; lookIndex < std::size(kLooks); ++lookIndex)
    {
        const Look look = kLooks[lookIndex];
        ViewReadbackResult image{};
        ASSERT_TRUE(Read(tickets[lookIndex], image)) << "look " << static_cast<int>(look);
        constexpr uint32_t kEdgeX = kSize / 4;
        constexpr uint32_t kEdgeY = kSize / 2;
        const ColorLinear resolved(ChannelAt(image, kEdgeX, kEdgeY, 0), ChannelAt(image, kEdgeX, kEdgeY, 1),
                                   ChannelAt(image, kEdgeX, kEdgeY, 2));
        const float luma = Luma(resolved);
        const float coloredLight = LookTonemap(look, kScale * coloredLuma);
        const float greyLight = LookTonemap(look, kScale * kGrey);
        const float idealLight = (coloredLight + (samples - 1.0f) * greyLight) / samples;
        const float shownLight = LookTonemap(look, kScale * luma);
        if (look == Look::None)
            EXPECT_NEAR(shownLight, idealLight, kRelativeTolerance * idealLight)
                << m_Samples << "x, no look: the plain mean";
        else
            EXPECT_NEAR(EncodedLevels(shownLight), EncodedLevels(idealLight),
                        look == Look::Photographic ? kFittedLevelTolerance : kLevelTolerance)
                << m_Samples << "x, look " << static_cast<int>(look) << ": shows " << shownLight
                << ", the samples' mean " << idealLight;
        const float chromaTolerance = look == Look::Photographic ? kFittedChromaTolerance : kChromaTolerance;
        const float coloredChannels[] = {kColored.r, kColored.g, kColored.b};
        const float resolvedChannels[] = {resolved.r, resolved.g, resolved.b};
        for (size_t channel = 0; channel < std::size(coloredChannels); ++channel)
        {
            const float idealChroma =
                (coloredLight * coloredChannels[channel] / coloredLuma + (samples - 1.0f) * greyLight) /
                (coloredLight + (samples - 1.0f) * greyLight);
            EXPECT_NEAR(resolvedChannels[channel] / luma, idealChroma, chromaTolerance)
                << m_Samples << "x, look " << static_cast<int>(look) << ", channel " << channel;
        }
        EXPECT_FLOAT_EQ(RedAt(image, 3 * kSize / 4, kSize / 2), kGrey) << "a flat pixel resolves to its value";
    }
}

INSTANTIATE_TEST_SUITE_P(SampleCaps, MsaaColorResolveTest, testing::Values(kMaxSamples, kForcedSamples), SampleCapName);
