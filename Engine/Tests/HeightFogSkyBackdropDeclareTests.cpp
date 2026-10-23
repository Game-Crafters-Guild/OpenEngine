// The sky pass as the shipped graph declares it: SkyRender publishes the texture it draws its
// backdrop from as SkyBackdrop (the surfaces' fog reads it for the fog's sky colour), bakes and
// imports its LUTs, and draws the stars into the star light. Part of RenderPipelineDeclareTests.
#include "Engine/Rendering/Pipeline/Nodes/EngineNodeTypes.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Mathematics/Matrix4x4.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Sky/SkySettings.h"
#include "Rendering/Sky/SkySystem.h"
#include "SharedHeadlessDevice.h"
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;

namespace
{

constexpr const char* kTextureResource =
    R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})";

TextureDesc ViewTargetDesc(TextureFormat format, uint32_t usage)
{
    TextureDesc d{};
    d.width = 64;
    d.height = 64;
    d.depth = 1;
    d.mipLevels = 1;
    d.arrayLayers = 1;
    d.sampleCount = 1;
    d.format = static_cast<uint32_t>(format);
    d.usage = usage;
    return d;
}

// The shipped pipeline's own pass declarations, so the test reads the gate and the input as they ship.
nlohmann::json ShippedPass(const nlohmann::json& pipeline, const char* id)
{
    for (const nlohmann::json& pass : pipeline["passes"])
        if (pass.value("id", std::string()) == id)
            return pass;
    ADD_FAILURE() << "ForwardPlus.rendergraph has no pass '" << id << "'";
    return {};
}

RenderPipelineBlueprint::Pass Step(const nlohmann::json& pass)
{
    RenderPipelineBlueprint::Pass p;
    p.id = pass.value("id", std::string());
    p.type = pass.value("type", std::string());
    p.enabled = true;
    p.perView = true;
    p.passJson = pass.dump();
    return p;
}

struct SkyBackdropFixture
{
    // The shared device, or the fixture's own when a test asks for the validation layer.
    IDevice* Device = nullptr;
    std::unique_ptr<IDevice> m_OwnedDevice;
    std::unique_ptr<RenderServices> Rs;
    RenderPipelineNodeRegistry Registry;
    Rendering::ViewId View{};

    // night: the stars are up (the night sky blend at 1). validation: the device runs the Vulkan
    // validation layer, so the frame's pipelines and barriers are checked as the editor checks them.
    bool Up(Components::HeightFogColorMode mode, bool night = false, bool validation = false)
    {
        if (validation)
        {
            DeviceDesc desc{};
            desc.preferredAPI = GraphicsAPI::Vulkan;
            desc.enableDebugLayer = true;
            desc.enableDynamicRendering = true;
            desc.descriptorBuffers = DescriptorBufferMode::Disabled;
            m_OwnedDevice = DeviceFactory::CreateDevice(desc);
            if (m_OwnedDevice && !m_OwnedDevice->Initialize(desc))
                m_OwnedDevice.reset();
            Device = m_OwnedDevice.get();
        }
        else
            Device = SharedHeadlessDevice();
        if (!Device)
            return false;
        Rs = std::make_unique<RenderServices>();
        EXPECT_TRUE(Rs->Initialize(Device));

        const Mathematics::Matrix4x4 view = Mathematics::Matrix4x4::LookAt(
            Mathematics::Vector3(0.0f, 3.0f, 0.0f), Mathematics::Vector3(0.0f, 3.0f, 1.0f),
            Mathematics::Vector3(0.0f, 1.0f, 0.0f));
        const Mathematics::Matrix4x4 projection =
            Mathematics::Matrix4x4::PerspectiveReverseZ(1.0471976f, 1.0f, 0.5f, 600.0f);
        const Mathematics::Matrix4x4 viewProj = projection * view;
        CameraData camera{};
        std::memcpy(camera.view, view.Data(), sizeof(camera.view));
        std::memcpy(camera.proj, projection.Data(), sizeof(camera.proj));
        std::memcpy(camera.viewProj, viewProj.Data(), sizeof(camera.viewProj));
        camera.cameraPos[1] = 3.0f;
        const CameraId cam = Rs->Views().AllocateCamera("SkyBackdropCam");
        Rs->Views().SetCameraData(cam, camera);
        View = Rs->Views().AllocateView("SkyBackdropView", cam);
        Rs->Views().SetViewRenderLayerMask(View, GameEngine::Rendering::RenderLayerMask::Default());
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        Rs->Views().SetViewTargets(View, 0, 0, 0, clear);

        PostProcessSettings pp{};
        pp.HeightFogIntensity = 1.0f;
        pp.HeightFogDensity = 0.5f;
        pp.HeightFogColorMode = static_cast<int32>(mode);
        Rs->Views().SetViewPostProcessOverride(View, pp);

        // The physical sky, active: what a scene with a Sky Environment hands the renderer.
        auto& sky = Rs->EnsureFeature<SkyRenderFeature>();
        EXPECT_TRUE(sky.Initialize(Device));
        Rendering::SkySettings settings{};
        settings.nightSkyBlend = night ? 1.0f : 0.0f;
        sky.SetSettings(settings, Rendering::SkySunState{});

        Nodes::RegisterEngineNodeTypes(Registry);
        return true;
    }

    RenderPipelineBlueprint Blueprint() const
    {
        const std::filesystem::path path = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                           "Engine/Assets/RenderPipelines/ForwardPlus.rendergraph";
        std::ifstream in(path);
        EXPECT_TRUE(in.is_open()) << path;
        std::stringstream text;
        text << in.rdbuf();
        const nlohmann::json pipeline = nlohmann::json::parse(text.str());

        RenderPipelineBlueprint bp;
        bp.pipelineName = "SkyBackdrop";
        bp.worldColorResolveTargetRef = "VolumetricClouds";
        bp.resources.push_back({"VolumetricClouds", kTextureResource});
        for (const char* id : {"ViewParamsUpload", "HeightFogParamsUpload", "Sky"})
            bp.passes.push_back(Step(ShippedPass(pipeline, id)));
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        return bp;
    }

    void Down()
    {
        Device->WaitForIdle();
        Rs->Shutdown();
        if (m_OwnedDevice)
            m_OwnedDevice->Shutdown();
    }
};

struct DeclaredFrame
{
    bool SkyViewLutImported = false;
    uint32_t SkyViewLutFormat = 0; // the format the graph holds for the imported sky-view LUT
    bool SkyViewLutBaked = false;  // a live pass bakes the sky-view LUT this frame
    // Every texture the sky's draw pass samples, with the stage mask the graph's barrier for that read covers.
    std::vector<std::pair<std::string, uint32_t>> SkySampleStages;
    bool StarsWriteStarLight = false; // the stars pass (Sky.View<N>.Stars) draws into Sky_StarLight.View<N>
    bool StarsReadCompensation = false; // the stars pass reads the view's exposure compensation state
    bool TonemapBindsStarLight = false; // the published star light has its physical image at declaration
};

DeclaredFrame DeclareFrame(SkyBackdropFixture& f)
{
    RenderPipelineInstance instance(*f.Rs, f.Registry);
    instance.SetBlueprint(f.Blueprint());

    RenderGraph::RGResourcePool persistent(f.Device);
    RenderGraph::RGTransientPool transient(f.Device);
    RenderGraph::RGUploadRing ring(f.Device, 2, 262144);
    RenderGraph::RGFrame frame(f.Device, &persistent, &transient, &ring);
    frame.BeginFrame(0);
    const RenderGraph::RGTexture color = frame.ImportPersistentTexture(
        "SB.Color", ViewTargetDesc(TextureFormat::RGBA8_UNORM,
                                   static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource)));
    const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(
        "SB.Depth", ViewTargetDesc(TextureFormat::D32_FLOAT,
                                   static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource)));
    f.Rs->BeginWorldDrawFrame();
    f.Rs->BuildWorldBatchKeys();
    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
    const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(), f.Rs->Views().GetViews().end());
    instance.Declare(frame, targets, views);

    // The backdrop texture is identified by what the sky's own draw samples: the per-view import
    // dedups by handle onto the frame-scope LUT import, so its name is not the published one.
    DeclaredFrame out;
    const RenderGraph::RGGraph& g = frame.Graph();
    for (RenderGraph::RGPassId pass = 0; pass < g.PassCount(); ++pass)
    {
        const std::string name = g.PassName(pass) ? g.PassName(pass) : "";
        const std::string suffix = "SkyViewLUT";
        if (!g.IsCulled(pass) && name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.SkyViewLutBaked = true;
    }
    for (RenderGraph::RGResourceId r = 0; r < g.ResourceCount(); ++r)
    {
        const char* rn = g.ResourceName(r);
        if (rn && std::string(rn).rfind("Sky_StarLight.View", 0) == 0)
            out.TonemapBindsStarLight = frame.PhysicalTexture(RenderGraph::RGTexture{r}).IsValid();
        if (rn && std::string(rn) == "Sky_View_LUT")
        {
            out.SkyViewLutImported = true;
            out.SkyViewLutFormat = g.ResourceDesc(r).Format;
        }
    }
    const auto isSampled = [](const RenderGraph::RGAccessRecord& a)
    {
        return a.Access == RenderGraph::RGAccess::Sampled || a.Access == RenderGraph::RGAccess::SampledVertex ||
               a.Access == RenderGraph::RGAccess::SampledCompute;
    };
    std::vector<std::string> skySamples;
    for (const RenderGraph::RGAccessRecord& a : g.Accesses())
    {
        const char* pn = g.PassName(a.Pass);
        const char* rn = g.ResourceName(a.Resource);
        if (pn && rn && isSampled(a) && std::string(pn).find(".Sky.") != std::string::npos)
        {
            skySamples.emplace_back(rn);
            // The sky's draw (Sky.View<N>), not the compute passes that bake its LUTs.
            if (std::string(pn).find(".Sky.View") != std::string::npos)
                out.SkySampleStages.emplace_back(rn, RenderGraph::MapAccess(a.Access, g.PassQueue(a.Pass)).Stage);
        }
    }
    for (const RenderGraph::RGAccessRecord& a : g.Accesses())
    {
        const char* pn = g.PassName(a.Pass);
        const char* rn = g.ResourceName(a.Resource);
        const std::string suffix = ".Stars";
        const std::string passName = pn ? pn : "";
        if (rn && std::string(rn).rfind("Sky_StarLight", 0) == 0 && passName.size() >= suffix.size() &&
            passName.compare(passName.size() - suffix.size(), suffix.size(), suffix) == 0 && !g.IsCulled(a.Pass))
            out.StarsWriteStarLight = true;
        if (rn && std::string(rn).rfind("Pipeline.AutoExposure.Compensation.View", 0) == 0 &&
            passName.size() >= suffix.size() &&
            passName.compare(passName.size() - suffix.size(), suffix.size(), suffix) == 0 && !g.IsCulled(a.Pass) &&
            !RenderGraph::IsWrite(a.Access))
            out.StarsReadCompensation = true;
    }
    if (skySamples.empty())
        ADD_FAILURE() << "the sky's draw sampled nothing: no backdrop to compare against";
    frame.MarkOutput(color);
    frame.Execute();
    f.Device->WaitForIdle();
    return out;
}

} // namespace

// The sky-view LUT that production creates is RGBA32F on a device that loads the SPIR-V kernels, so a
// moonless night's radiance (about 8e-7 scene-linear) survives it; the SkyAtmosphereLutReference
// tests make their own texture through the same accessor and cannot see the renderer's.
TEST(RenderPipelineDeclareTests, TheSkyViewLutTheRendererCreatesIsRgba32fForSpirVKernels)
{
    SkyBackdropFixture f;
    if (!f.Up(Components::HeightFogColorMode::SkyHorizon))
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_EQ(f.Device->PreferredShaderSource(), ShaderSourceKind::SpirV);
    const SkyRenderFeature* sky = f.Rs->GetFeature<SkyRenderFeature>();
    ASSERT_NE(sky, nullptr);
    EXPECT_EQ(f.Device->GetTextureFormat(sky->GetRenderer().GetSkyViewLutTexture()),
              TextureFormat::R32G32B32A32_FLOAT);
    f.Down();
}

// The render graph holds the sky-view LUT's import at the texture's own format, so the graph's
// readback (capture_resource) can read the LUT in a live editor; an import without a format reads
// as unknown and the readback refuses it.
TEST(RenderPipelineDeclareTests, TheSkyViewLutImportCarriesItsFormat)
{
    SkyBackdropFixture f;
    if (!f.Up(Components::HeightFogColorMode::SkyHorizon))
        GTEST_SKIP() << "No Vulkan device available";
    const DeclaredFrame r = DeclareFrame(f);
    ASSERT_TRUE(r.SkyViewLutImported) << "the sky pass must import the sky-view LUT";
    EXPECT_EQ(r.SkyViewLutFormat, static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT));
    f.Down();
}

// The star billboard reads both LUTs in its vertex shader (the sky behind each star and the extinction
// along it), and the sky-view LUT is baked by compute earlier in the frame. So every texture the sky's
// passes sample is read under a barrier that reaches the vertex stage; a fragment-only read would leave
// the compute write invisible to the vertex shader.
TEST(RenderPipelineDeclareTests, TheSkyPassSamplesItsLutsFromTheVertexStage)
{
    SkyBackdropFixture f;
    if (!f.Up(Components::HeightFogColorMode::SkyHorizon, /*night=*/true))
        GTEST_SKIP() << "No Vulkan device available";
    const DeclaredFrame r = DeclareFrame(f);
    ASSERT_GE(r.SkySampleStages.size(), 2u) << "the sky pass samples the sky-view and transmittance LUTs";
    for (const auto& [name, stage] : r.SkySampleStages)
    {
        EXPECT_TRUE(stage & RenderGraph::RGStage::VertexShader)
            << "the sky pass samples " << name << " under a barrier scoped to stage mask " << stage
            << ", which omits VertexShader: the star billboard's vertex shader reads it";
        EXPECT_TRUE(stage & RenderGraph::RGStage::FragmentShader) << name;
    }
    f.Down();
}

// At night the stars draw in their own pass, into scene colour and into the star light (SkyStarLight)
// that the tonemap's night tint reads, which exists at declaration so the tonemap binds it. The pass
// reads the view's exposure compensation state, which it undoes. The frame records and runs under the
// validation layer with no error.
TEST(RenderPipelineDeclareTests, AtNightTheStarsPassWritesTheStarLightWithoutAValidationError)
{
    SkyBackdropFixture f;
    if (!f.Up(Components::HeightFogColorMode::SkyHorizon, /*night=*/true, /*validation=*/true))
        GTEST_SKIP() << "No Vulkan device available";
    const DeclaredFrame r = DeclareFrame(f);
    EXPECT_TRUE(r.StarsWriteStarLight) << "the stars pass must draw into Sky_StarLight.View<N>";
    EXPECT_TRUE(r.StarsReadCompensation)
        << "the stars pass must read Pipeline.AutoExposure.Compensation.View<N> to undo the compensation curve";
    EXPECT_TRUE(r.TonemapBindsStarLight)
        << "the published star light needs its physical image at declaration: the tonemap's fullscreen stage builds "
           "its bindings then, and binds black for a texture that has none";
    const ValidationStats stats = f.Device->GetValidationStats();
    f.Down();
    if (!stats.Enabled)
        GTEST_SKIP() << "the Vulkan validation layer is not installed: the stars pass's pipeline went unchecked";
    EXPECT_EQ(stats.ErrorCount, 0u);
    for (const ValidationVuidStat& v : stats.Vuids)
        EXPECT_FALSE(v.IsError) << v.Vuid << " x" << v.Count << ": " << v.FirstMessage;
}

namespace
{
// The default sky with a moon of a full moon's source (about 0.25 lx lifted to the top of the air)
// toward (x, y, z), shown or hidden.
Rendering::SkySettings SkyWithMoonAt(float x, float y, float z, bool shown)
{
    Rendering::SkySettings s{};
    s.moonDirWS[0] = x;
    s.moonDirWS[1] = y;
    s.moonDirWS[2] = z;
    s.moonIntensity = 1.5e-3f;
    s.showMoonDisk = shown;
    return s;
}
} // namespace

// The sky-view LUT bakes only when what it holds changes. The moon is its second source, so moving the
// moon re-bakes it; a hidden moon is not in the bake, so moving a hidden moon does not.
TEST(RenderPipelineDeclareTests, MovingTheMoonRebakesTheSkyViewLutAndAHiddenMoonDoesNot)
{
    SkyBackdropFixture f;
    if (!f.Up(Components::HeightFogColorMode::SkyHorizon))
        GTEST_SKIP() << "No Vulkan device available";
    auto& sky = f.Rs->EnsureFeature<SkyRenderFeature>();
    struct Step
    {
        Rendering::SkySettings Settings;
        bool Rebakes;
        const char* What;
    };
    const Step steps[] = {
        {SkyWithMoonAt(0.0f, 0.6f, 0.8f, true), true, "the first frame bakes"},
        {SkyWithMoonAt(0.0f, 0.6f, 0.8f, true), false, "a steady sky does not re-bake"},
        {SkyWithMoonAt(0.6f, 0.8f, 0.0f, true), true, "a moved moon re-bakes"},
        {SkyWithMoonAt(0.6f, 0.8f, 0.0f, false), true, "hiding the moon re-bakes (its light leaves the sky)"},
        {SkyWithMoonAt(0.0f, 0.6f, 0.8f, false), false, "a hidden moon that moves does not re-bake"},
    };
    for (const Step& step : steps)
    {
        sky.SetSettings(step.Settings, Rendering::SkySunState{});
        EXPECT_EQ(DeclareFrame(f).SkyViewLutBaked, step.Rebakes) << step.What;
    }
    f.Down();
}
