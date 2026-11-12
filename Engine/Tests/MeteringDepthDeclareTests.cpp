// Which depth the auto-exposure meter splits the frame with, and who owns the depth with the water in
// it: the ocean's surface step resolves it once and publishes View.DepthResolvedPostOcean; the fog and
// the meter read it when it resolves, else View.DepthResolved. And which passes each meter declares.
// Part of RenderPipelineDeclareTests.
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/Pipeline/Nodes/AutoExposureNode.h"
#include "Engine/Rendering/Pipeline/Nodes/DepthResolveNode.h"
#include "Engine/Rendering/Pipeline/Nodes/VolumetricFogNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Matrix4x4.h"
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanRenderNode.h"
#include "Ocean/OceanSurfaceMaterial.h"
#include "RGPassQuery.h"
#include "SceneLoadTracker/SceneLoadTracker.h"
#include "SceneLoadTrackerTestAccess.h"
#include "ScopedScriptingFreeEngine.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "StagedTestPaths.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "SharedHeadlessDevice.h"
#include "TestCameraEye.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;
namespace RGQuery = GameEngine::Testing::RGQuery;

namespace
{
struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 262144) {}
};

TextureDesc TargetDesc(TextureFormat format, uint32_t usage, uint32_t samples)
{
    TextureDesc d{};
    d.width = 64;
    d.height = 64;
    d.depth = 1;
    d.mipLevels = 1;
    d.arrayLayers = 1;
    d.sampleCount = samples;
    d.format = static_cast<uint32_t>(format);
    d.usage = usage;
    return d;
}
// An environment that lights the scene and never bakes, so its irradiance reading never lands: the
// state a view is in between a sky appearing and its first reading.
class EnvironmentWithoutReading final : public IEnvironmentSource
{
  public:
    const char* Name() const override { return "EnvironmentWithoutReading"; }
    uint64_t InputDigest() const override { return 1u; }
    bool HasActiveContent() const override { return true; }
    void ScheduleBake(const EnvironmentBakeContext&) override {}
};

TextureDesc ColorTargetDesc(uint32_t samples = 1)
{
    return TargetDesc(TextureFormat::RGBA8_UNORM,
                      static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource), samples);
}
TextureDesc DepthTargetDesc(uint32_t samples = 1)
{
    return TargetDesc(TextureFormat::D32_FLOAT,
                      static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource), samples);
}
} // namespace

namespace
{
// Which depth the auto-exposure meter splits the frame with, and who resolves the depth with the
// water in it. The ocean's post-world step (it draws the surface) resolves the view depth once after
// the surface and publishes View.DepthResolvedPostOcean; fog and auto exposure read it when it
// resolves, else View.DepthResolved.
struct MeteringDepthFixture
{
    // The shared device, or the fixture's own when a test asks for the validation layer.
    IDevice* Device = nullptr;
    std::unique_ptr<IDevice> m_OwnedDevice;
    std::unique_ptr<RenderServices> Rs;
    RenderPipelineNodeRegistry Registry;
    Rendering::ViewId View{};

    // `validation` creates the device with the validation layer and descriptor sets rather than
    // descriptor buffers, for a test that checks what the frame binds: the layer tracks the writes
    // of a descriptor set, and an unwritten descriptor is visible to nothing else.
    bool Up(float waterCenterZ, bool fog, Components::ExposureMeter meter = Components::ExposureMeter::Picture,
            bool environmentWithoutReading = false, bool validation = false)
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
        // The ocean surface material compiles through the real composer from the staged shader
        // mirror (the host normally provides this context); without it the surface never draws.
        Rendering::MaterialBuildContext mbc{};
        mbc.AdapterShaderDir = GameEngine::TestPaths::StagedRenderingShadersDir();
        mbc.CacheRoot = std::filesystem::temp_directory_path() / "ge_metering_depth_cache";
        mbc.IncludeDirs = {mbc.AdapterShaderDir};
        mbc.PackageShaderDirs = {mbc.AdapterShaderDir};
        Rs->Materials().SetMaterialBuildContext(mbc);
        // Looking along +Z from 50 m above the sea: water 150 m ahead is in view, 150 m behind is not.
        const Mathematics::Matrix4x4 view = Mathematics::Matrix4x4::LookAt(
            Mathematics::Vector3(0.0f, 50.0f, 0.0f), Mathematics::Vector3(0.0f, 50.0f, 1.0f),
            Mathematics::Vector3(0.0f, 1.0f, 0.0f));
        const Mathematics::Matrix4x4 projection =
            Mathematics::Matrix4x4::PerspectiveReverseZ(1.0471976f, 1.0f, 0.5f, 600.0f);
        const Mathematics::Matrix4x4 viewProj = projection * view;
        CameraData camera{};
        camera.view = view;
        camera.proj = projection;
        camera.viewProj = viewProj;
        camera.cameraPos.y = 50.0f;
        const CameraId cam = Rs->Views().AllocateCamera("MeteringDepthCam");
        Rs->Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(camera), {0.5f, 600.0f});
        View = Rs->Views().AllocateView("MeteringDepthView", cam);
        Rs->Views().SetViewRenderLayerMask(View, GameEngine::Rendering::RenderLayerMask::Default());
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        Rs->Views().SetViewTargets(View, 0, 0, 0, clear);

        PostProcessSettings pp{};
        pp.AutoExposureActive = true;
        pp.AutoExposureMeter = static_cast<int32>(meter);
        pp.VolumetricFogIntensity = fog ? 1.0f : 0.0f;
        pp.VolumetricFogDensity = 0.02f;
        Rs->Views().SetViewPostProcessOverride(View, pp);
        if (environmentWithoutReading)
            Rs->EnsureFeature<ImageBasedLightingFeature>().SetEnvironmentSource(
                std::make_unique<EnvironmentWithoutReading>());

        if (waterCenterZ != 0.0f)
        {
            auto& ocean = Rs->EnsureFeature<GameEngine::Ocean::OceanRenderFeature>();
            EXPECT_TRUE(ocean.Initialize(Device));
            ocean.SetHasOcean(true);
            GameEngine::Ocean::OceanRenderFeature::WaterBodyBox water{};
            water.CenterZ = waterCenterZ;
            water.HalfX = 40.0f;
            water.HalfZ = 40.0f;
            ocean.SetWaterBodies({water}, true);
        }

        EXPECT_TRUE(Registry.Register(
            "DepthResolve", [] { return std::make_unique<Nodes::DepthResolveNode>(); }, true));
        EXPECT_TRUE(Registry.Register(
            "OceanRender", [] { return std::make_unique<GameEngine::Ocean::OceanRenderNode>(); }, true));
        EXPECT_TRUE(Registry.Register(
            "VolumetricFog", [] { return std::make_unique<Nodes::VolumetricFogNode>(); }, true));
        EXPECT_TRUE(Registry.Register(
            "AutoExposure", [] { return std::make_unique<Nodes::AutoExposureNode>(); }, true));
        return true;
    }

    static RenderPipelineBlueprint::Pass Step(const char* id, const char* type, const char* json)
    {
        RenderPipelineBlueprint::Pass p;
        p.id = id;
        p.type = type;
        p.enabled = true;
        p.perView = true;
        p.passJson = json;
        return p;
    }

    RenderPipelineBlueprint Blueprint(bool withOcean, bool withFog) const
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "MeteringDepth";
        bp.worldColorResolveTargetRef = "SceneColor";
        bp.resources.push_back(
            {"SceneColor",
             R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
        bp.passes.push_back(Step("DepthResolve", "DepthResolve", R"({"id":"DepthResolve","type":"DepthResolve"})"));
        if (withOcean)
        {
            bp.passes.push_back(Step("Ocean", "OceanRender", R"({"id":"Ocean","type":"OceanRender"})"));
            bp.passes.push_back(Step("OceanUnderwater", "OceanRender",
                                     R"({"id":"OceanUnderwater","type":"OceanRender","mode":"underwater"})"));
        }
        if (withFog)
            bp.passes.push_back(Step("Fog", "VolumetricFog", R"({"id":"Fog","type":"VolumetricFog","output":"SceneColor"})"));
        bp.passes.push_back(
            Step("AutoExposure", "AutoExposure", R"({"id":"AutoExposure","type":"AutoExposure","input":"SceneColor"})"));
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

struct MeteringDepthFrame
{
    std::vector<std::string> MeterDepthReads; // depth-like textures the histogram pass samples
    std::vector<std::string> FogDepthReads;   // depth-like textures the fog passes sample
    uint32_t PostOceanResolves = 0;           // passes that write the post-ocean depth
    bool SurfaceDrawn = false;                // an OceanSurface pass was declared
    bool MeterDeclared = false;               // the histogram pass was declared
    bool MeterReadsExposure = false;          // the histogram pass reads the view's ExposureHistory
    bool ResolveDeclared = false;             // the resolve pass was declared
    bool ResolveWritesCompensation = false;   // the resolve writes the view's exposure compensation state
};

MeteringDepthFrame DeclareOneFrame(MeteringDepthFixture& f, RenderPipelineInstance& instance, uint32_t index)
{
    MeteringDepthFrame out;
    FramePools pools(f.Device);
    RenderGraph::RGFrame frame(f.Device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(index);
    RenderGraph::RGTexture color = frame.ImportPersistentTexture("MD.Color", ColorTargetDesc());
    RenderGraph::RGTexture depth = frame.ImportPersistentTexture("MD.Depth", DepthTargetDesc());
    f.Rs->BeginWorldDrawFrame();
    f.Rs->BuildWorldBatchKeys();
    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
    const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(), f.Rs->Views().GetViews().end());
    instance.Declare(frame, targets, views);

    const RenderGraph::RGGraph& g = frame.Graph();
    std::set<RenderGraph::RGPassId> resolveWriters;
    for (const RenderGraph::RGAccessRecord& a : g.Accesses())
    {
        const char* pn = g.PassName(a.Pass);
        const char* rn = g.ResourceName(a.Resource);
        if (!pn || !rn)
            continue;
        const std::string pass(pn), res(rn);
        if (pass.find("OceanSurface") != std::string::npos)
            out.SurfaceDrawn = true;
        const bool isDepth = res.find("DepthResolved") != std::string::npos;
        if (res.find("DepthResolvedPostOcean") != std::string::npos && RenderGraph::IsWrite(a.Access))
            resolveWriters.insert(a.Pass);
        const bool sampled = a.Access == RenderGraph::RGAccess::Sampled ||
                             a.Access == RenderGraph::RGAccess::SampledCompute;
        if (pass.find("AutoExposureResolve") != std::string::npos)
        {
            out.ResolveDeclared = true;
            if (res.find("AutoExposure.Compensation") != std::string::npos && RenderGraph::IsWrite(a.Access))
                out.ResolveWritesCompensation = true;
        }
        if (pass.find("AutoExposureHistogram") != std::string::npos)
        {
            out.MeterDeclared = true;
            // The view's ExposureHistory: the blueprint resource, or the import for a graph without one.
            if ((res.find("ExposureHistory") != std::string::npos ||
                 res.find("AutoExposure.History") != std::string::npos) &&
                a.Access == RenderGraph::RGAccess::StorageRead)
                out.MeterReadsExposure = true;
            if (sampled && isDepth)
                out.MeterDepthReads.push_back(res);
        }
        if (sampled && isDepth && RGQuery::Matches(pass, RGQuery::Subtree{"VolumetricFog"}))
            out.FogDepthReads.push_back(res);
    }
    out.PostOceanResolves = static_cast<uint32_t>(resolveWriters.size());
    frame.MarkOutput(color);
    frame.Execute();
    f.Device->WaitForIdle();
    return out;
}

// The first frame registers the ocean's surface contributor and its material may compile over a few
// frames: declare until the surface draws, or a bounded number of frames when it never does.
MeteringDepthFrame DeclareMeteringDepthFrame(MeteringDepthFixture& f, bool withOcean, bool withFog)
{
    RenderPipelineInstance instance(*f.Rs, f.Registry);
    instance.SetBlueprint(f.Blueprint(withOcean, withFog));
    constexpr uint32_t kMaxFrames = 30;
    MeteringDepthFrame last;
    for (uint32_t i = 0; i < kMaxFrames; ++i)
    {
        last = DeclareOneFrame(f, instance, i);
        if (last.SurfaceDrawn || !withOcean)
            break;
    }
    return last;
}

bool OnlyPostOcean(const std::vector<std::string>& reads)
{
    return !reads.empty() && std::all_of(reads.begin(), reads.end(), [](const std::string& r)
                                         { return r.find("DepthResolvedPostOcean") != std::string::npos; });
}
bool OnlyViewDepth(const std::vector<std::string>& reads)
{
    return !reads.empty() && std::all_of(reads.begin(), reads.end(), [](const std::string& r)
                                         { return r.find("DepthResolvedPostOcean") == std::string::npos; });
}
} // namespace

// Water in view, no fog: the ocean's surface step resolves the depth once after the water and the
// meter reads that depth, so the sea counts as scene, not sky.
TEST(RenderPipelineDeclareTests, AutoExposureMetersThePostOceanDepthWhenTheWaterDraws)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/150.0f, /*fog=*/false))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/true, /*withFog=*/false);
    ASSERT_TRUE(r.MeterDeclared);
    ASSERT_TRUE(r.SurfaceDrawn) << "the ocean surface must draw for this pin to cover anything";
    EXPECT_EQ(r.PostOceanResolves, 1u);
    EXPECT_TRUE(OnlyPostOcean(r.MeterDepthReads));
    f.Down();
}

// Water and fog: both read the one resolve the ocean step owns; nothing resolves the depth twice.
TEST(RenderPipelineDeclareTests, FogAndAutoExposureShareTheOceanStepsPostOceanResolve)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/150.0f, /*fog=*/true))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/true, /*withFog=*/true);
    ASSERT_TRUE(r.MeterDeclared);
    ASSERT_TRUE(r.SurfaceDrawn);
    EXPECT_EQ(r.PostOceanResolves, 1u) << "the fog no longer resolves its own copy";
    EXPECT_TRUE(OnlyPostOcean(r.MeterDepthReads));
    EXPECT_TRUE(OnlyPostOcean(r.FogDepthReads)) << "the fog reads the same post-ocean depth";
    f.Down();
}

// The picture meter builds its histograms from the frame and resolves them.
TEST(RenderPipelineDeclareTests, ThePictureMeterDeclaresTheHistogramAndTheResolve)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/0.0f, /*fog=*/false, Components::ExposureMeter::Picture,
              /*environmentWithoutReading=*/false, /*validation=*/true))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/false, /*withFog=*/false);
    EXPECT_TRUE(r.MeterDeclared);
    EXPECT_TRUE(r.ResolveDeclared);
    // The colour it bins is stored at the frame's pre-exposure: the read orders it after the snapshot,
    // and the binding hands the shader P (an unwritten one is a validation error).
    EXPECT_TRUE(r.MeterReadsExposure) << "the histogram pass must read the view's ExposureHistory";
    // The stars undo the compensation the resolve applied: the resolve records it every frame.
    EXPECT_TRUE(r.ResolveWritesCompensation) << "the resolve must write the view's exposure compensation state";
    const ValidationStats stats = f.Device->GetValidationStats();
    f.Down();
    if (!stats.Enabled)
        GTEST_SKIP() << "the Vulkan validation layer is not installed: the histogram pass's bindings went unchecked";
    EXPECT_EQ(stats.ErrorCount, 0u) << "the frame's descriptors are not all written";
    for (const ValidationVuidStat& v : stats.Vuids)
        EXPECT_FALSE(v.IsError) << v.Vuid << " x" << v.Count << ": " << v.FirstMessage;
}

// The lights meter reads the world's lights, never the frame: the resolve runs and no histogram pass
// reads the color or the depth.
TEST(RenderPipelineDeclareTests, TheLightsMeterDeclaresTheResolveWithoutTheHistogram)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/0.0f, /*fog=*/false, Components::ExposureMeter::Lights))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/false, /*withFog=*/false);
    EXPECT_FALSE(r.MeterDeclared);
    EXPECT_TRUE(r.MeterDepthReads.empty());
    EXPECT_TRUE(r.ResolveDeclared);
    f.Down();
}

// Before the sky's first irradiance reading lands there is nothing to meter: the lights meter declares
// no resolve, so the exposure history holds instead of flashing a reading without the sky.
TEST(RenderPipelineDeclareTests, TheLightsMeterDeclaresNoResolveBeforeTheSkysFirstReading)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/0.0f, /*fog=*/false, Components::ExposureMeter::Lights,
              /*environmentWithoutReading=*/true))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/false, /*withFog=*/false);
    EXPECT_FALSE(r.MeterDeclared);
    EXPECT_FALSE(r.ResolveDeclared);
    f.Down();
}

// No water and no fog: nothing takes a post-world resolve; the meter reads the view's resolved depth.
TEST(RenderPipelineDeclareTests, AutoExposureMetersTheViewDepthWithoutWaterOrFog)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/0.0f, /*fog=*/false))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/false, /*withFog=*/false);
    ASSERT_TRUE(r.MeterDeclared);
    EXPECT_EQ(r.PostOceanResolves, 0u);
    EXPECT_TRUE(OnlyViewDepth(r.MeterDepthReads));
    f.Down();
}

// Fog without water: the fog takes the post-world resolve itself (after every world depth writer, not
// the copy taken right after the prepass), and the meter after it reads the same depth.
TEST(RenderPipelineDeclareTests, FogTakesThePostWorldResolveWithoutWaterAndTheMeterSharesIt)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/0.0f, /*fog=*/true))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/false, /*withFog=*/true);
    ASSERT_TRUE(r.MeterDeclared);
    EXPECT_EQ(r.PostOceanResolves, 1u) << "one post-world resolve, declared by the fog";
    EXPECT_TRUE(OnlyPostOcean(r.FogDepthReads)) << "the fog reads the post-world depth";
    EXPECT_TRUE(OnlyPostOcean(r.MeterDepthReads)) << "the meter reads the same depth";
    f.Down();
}

// Water in the scene but behind the camera: the surface does not draw, so no post-ocean resolve is
// declared and the meter reads the view's resolved depth.
TEST(RenderPipelineDeclareTests, WaterOutOfViewDeclaresNoPostOceanResolve)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/-150.0f, /*fog=*/false))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/true, /*withFog=*/false);
    ASSERT_TRUE(r.MeterDeclared);
    EXPECT_FALSE(r.SurfaceDrawn);
    EXPECT_EQ(r.PostOceanResolves, 0u);
    EXPECT_TRUE(OnlyViewDepth(r.MeterDepthReads));
    f.Down();
}

namespace
{
// The pipeline builds of a view paused from boot that sees the ocean: in its two paused frames and in its
// first live frame, and whether the foam pipeline is warm when the pause ends.
struct PausedOceanBuilds
{
    uint64_t Paused = 0;
    uint64_t FirstLiveFrame = 0;
    bool FoamWarmAfterPause = false;
    // In the frame that registers the surface material: its base compile runs on a worker.
    bool SurfaceRegistered = false;
    bool SurfaceCompileOnWorker = false;
    // After the pause: the view's recorded submersion turns the underwater look on.
    bool UnderwaterAfterPause = false;
    // With the engine up, after the first frame: ready to show once the compile tasks return and before
    // the surface material publishes; and ready after the publish, with the tracker's status then.
    bool ReadyBeforePublish = false;
    bool ReadyAfterPublish = false;
    std::string StatusAfterPublish;
};

// `Samples` is the sample count of the view's colour and depth. With `FoamStepPending` the foam has one
// step to run, as after an extraction tick in which time passed. The water is in view at
// WaterCenterZ 150 and behind the camera at -150. With `SurfaceQuery` gameplay samples the waves
// before each frame. With `EngineUp` the engine and its job system run, so a material can compile on a
// worker, and the material system reports its compiles to a load tracker. `UnderwaterLook` is the
// authored underwater toggle. With `LiveFromBoot` the view draws its world from the first frame.
struct PausedOceanSetup
{
    uint32_t Samples = 1;
    bool FoamStepPending = true;
    float WaterCenterZ = 150.0f;
    float CameraY = 50.0f;
    bool SurfaceQuery = false;
    bool EngineUp = false;
    bool UnderwaterLook = false;
    bool LiveFromBoot = false;
};

// The GUID of the surface material of the view's world pass keywords.
GUID SurfaceMaterialGuid(MeteringDepthFixture& f)
{
    const Rendering::MaterialKeyword passKeywords =
        f.Rs->Views().GetViewDeclaredWorldPassKeywords(f.View).value_or(Rendering::MaterialKeyword::None) &
        ~Rendering::MaterialKeyword::SSSRNormalRoughness;
    return GameEngine::Ocean::OceanSurfaceMaterialGuid(passKeywords);
}

// True when the surface material is registered and its base compile runs on a worker, so its
// registering frame built no pipeline on the main thread.
bool IsSurfaceCompileOnWorker(MeteringDepthFixture& f)
{
    const GUID guid = SurfaceMaterialGuid(f);
    const Engine::Renderer::Material* material = f.Rs->Materials().Registry().Find(guid);
    return material && f.Rs->Materials().IsBaseCompileInFlight(guid) && !material->GetGraphicsPipelineId().IsValid();
}

// Waits, for at most 10 s, until every compile task that the material system submitted has returned.
// The publishes of the base compiles stay pending until the next flush.
bool WaitForTheCompileTasks(const MaterialSystem& materials)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const MaterialSystem::PrewarmProgress progress = materials.GetPrewarmProgress();
        if (progress.completed == progress.submitted)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// Ticks `tracker` as a frame boundary does and returns whether the scene is ready to show.
bool TickReadyToShow(SceneLoadTracker& tracker)
{
    SceneLoadTrackerTestAccess::Tick(tracker, std::chrono::steady_clock::now());
    return tracker.IsReadyToShow();
}

// Places the fixture's camera at `cameraY`, looking along +Z.
void PlaceCameraAt(MeteringDepthFixture& f, float cameraY)
{
    const Mathematics::Matrix4x4 view = Mathematics::Matrix4x4::LookAt(
        Mathematics::Vector3(0.0f, cameraY, 0.0f), Mathematics::Vector3(0.0f, cameraY, 1.0f),
        Mathematics::Vector3(0.0f, 1.0f, 0.0f));
    const Mathematics::Matrix4x4 projection = Mathematics::Matrix4x4::PerspectiveReverseZ(1.0471976f, 1.0f, 0.5f, 600.0f);
    CameraData camera{};
    camera.view = view;
    camera.proj = projection;
    camera.viewProj = projection * view;
    camera.cameraPos.y = cameraY;
    const auto* desc = f.Rs->Views().FindViewDesc(f.View);
    ASSERT_NE(desc, nullptr);
    f.Rs->Views().SetCameraData(desc->cameraId, ::GameEngine::Tests::TestCameraWithEye(camera), {0.5f, 600.0f});
}

// Empty without a Vulkan device.
std::optional<PausedOceanBuilds> RunPausedOceanView(const PausedOceanSetup& setup)
{
    namespace fs = std::filesystem;
    std::optional<GameEngine::Testing::ScopedScriptingFreeEngine> engine;
    if (setup.EngineUp)
    {
        const fs::path workspace =
            fs::temp_directory_path() /
            ("ge_paused_ocean_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::create_directories(workspace / "Assets", ec);
        engine.emplace("PausedOceanTest", workspace);
        if (!engine->Up())
        {
            ADD_FAILURE() << "EngineCore failed to initialize (scripting-free config)";
            return std::nullopt;
        }
    }
    // Outlives the fixture, whose shutdown revokes the material system's report.
    SceneLoadTracker tracker;
    // The fixture's own device: no earlier test has built the ocean's pipelines on it.
    MeteringDepthFixture f;
    if (!f.Up(setup.WaterCenterZ, false, Components::ExposureMeter::Picture, false, true))
        return std::nullopt;
    PlaceCameraAt(f, setup.CameraY);
    if (setup.EngineUp)
        f.Rs->Materials().ReportLoadingWork(tracker);
    auto& ocean = f.Rs->EnsureFeature<GameEngine::Ocean::OceanRenderFeature>();
    EXPECT_TRUE(ocean.IsFoamReady()) << "the foam step did not initialize: build StageTestAssets, which "
                                        "stages Shaders/Ocean next to the test executable";
    if (setup.UnderwaterLook)
    {
        GameEngine::Ocean::OceanParamsGPU params = ocean.GetParams();
        params.Underwater = 1u;
        ocean.SetParams(params);
    }
    if (setup.FoamStepPending)
    {
        constexpr float kTick = 1.0f / 30.0f;
        ocean.GetFoamSim().SetFrameInputs(ocean.GetDisplacementTexture(), ocean.GetDisplacementSampler(),
                                          ocean.GetFFT().GetCascadeCount(), 0.0f, 0.0f, 0.0f, kTick, 0.0f, 0.0f,
                                          0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    RenderPipelineBlueprint bp;
    bp.pipelineName = "PausedOceanWarm";
    bp.worldColorResolveTargetRef = "SceneColor";
    bp.resources.push_back(
        {"SceneColor",
         R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
    // The shipping order: the surface samples the depth copy, not the depth attachment it draws into.
    bp.passes.push_back(
        MeteringDepthFixture::Step("DepthResolve", "DepthResolve", R"({"id":"DepthResolve","type":"DepthResolve"})"));
    bp.passes.push_back(MeteringDepthFixture::Step("Ocean", "OceanRender", R"({"id":"Ocean","type":"OceanRender"})"));
    bp.passes.push_back(MeteringDepthFixture::Step("OceanUnderwater", "OceanRender",
                                                   R"({"id":"OceanUnderwater","type":"OceanRender","mode":"underwater"})"));
    bp.outputs.push_back({"FinalColor", "View.Resolve"});
    RenderPipelineInstance instance(*f.Rs, f.Registry);
    instance.SetBlueprint(bp);
    f.Rs->Views().SetViewWorldPaused(f.View, !setup.LiveFromBoot);

    FramePools pools(f.Device);
    RenderGraph::RGFrame frame(f.Device, &pools.Persistent, &pools.Transient, &pools.Ring);
    PausedOceanBuilds builds{};
    const auto declareFrame = [&](uint64_t frameIndex)
    {
        if (setup.SurfaceQuery)
            (void)ocean.SampleSurface(0.0f, -150.0f);
        frame.BeginFrame(frameIndex);
        const RenderGraph::RGTexture color = frame.ImportPersistentTexture("POW.Color", ColorTargetDesc(setup.Samples));
        const RenderGraph::RGTexture depth = frame.ImportPersistentTexture("POW.Depth", DepthTargetDesc(setup.Samples));
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(), f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);
        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();
        if (!builds.SurfaceRegistered && f.Rs->Materials().Registry().Find(SurfaceMaterialGuid(f)))
        {
            builds.SurfaceRegistered = true;
            builds.SurfaceCompileOnWorker = IsSurfaceCompileOnWorker(f);
        }
        const bool probeReadiness = setup.EngineUp && frameIndex == 0;
        if (probeReadiness)
        {
            EXPECT_TRUE(WaitForTheCompileTasks(f.Rs->Materials())) << "the compile tasks did not return in 10 s";
            builds.ReadyBeforePublish = TickReadyToShow(tracker);
        }
        f.Rs->Materials().FlushAsyncMaterialCompiles();
        if (probeReadiness)
        {
            builds.ReadyAfterPublish = TickReadyToShow(tracker);
            builds.StatusAfterPublish = tracker.Describe();
        }
        return f.Device->GetPipelineCacheStats().Inserts;
    };

    const uint64_t atBoot = f.Device->GetPipelineCacheStats().Inserts;
    // The surface material compiles on a worker, so its pipeline is asked for on the next paused frame.
    declareFrame(0);
    const uint64_t afterPause = declareFrame(1);
    builds.Paused = afterPause - atBoot;
    builds.FoamWarmAfterPause = f.Device->TryGetWarmComputePipeline(ocean.GetFoamSim().GetPipelineId()).IsValid();
    builds.UnderwaterAfterPause = ocean.GetParamsForView(f.View).Underwater != 0u;
    f.Rs->Views().SetViewWorldPaused(f.View, false);
    builds.FirstLiveFrame = declareFrame(2) - afterPause;
    f.Down();
    return builds;
}
} // namespace

// A view paused from boot asks for the ocean's pipelines: the view simulation of the pre-world step and the
// scene grab, surface and depth copy of the post-world step build during the pause, and the first live
// frame builds none.
TEST(RenderPipelineDeclareTests, APausedViewFromBootRequestsTheOceanPipelinesOfItsFirstLiveFrame)
{
    const std::optional<PausedOceanBuilds> builds = RunPausedOceanView({});
    if (!builds)
        GTEST_SKIP() << "No Vulkan device available";
    EXPECT_GE(builds->Paused, 2u) << "the paused frames build the view simulation and the surface";
    EXPECT_TRUE(builds->FoamWarmAfterPause) << "the foam step of the live frame builds during the pause";
    EXPECT_EQ(builds->FirstLiveFrame, 0u) << "the first live frame builds no ocean pipeline";
}

// With multisampled view targets the pause builds the variants of the live frame: the grab's resolve, the
// surface at the view's sample count and the depth resolve.
TEST(RenderPipelineDeclareTests, APausedMultisampledViewRequestsTheOceanPipelinesOfItsFirstLiveFrame)
{
    const std::optional<PausedOceanBuilds> builds = RunPausedOceanView({.Samples = 4u});
    if (!builds)
        GTEST_SKIP() << "No Vulkan device available";
    EXPECT_GE(builds->Paused, 2u) << "the paused frames build the view simulation and the surface";
    EXPECT_EQ(builds->FirstLiveFrame, 0u) << "the first live frame builds no ocean pipeline";
}

// With no foam step pending the live frame runs no foam, so the pause builds no foam pipeline.
TEST(RenderPipelineDeclareTests, APausedViewRequestsNoFoamPipelineWithoutAPendingFoamStep)
{
    const std::optional<PausedOceanBuilds> builds = RunPausedOceanView({.FoamStepPending = false});
    if (!builds)
        GTEST_SKIP() << "No Vulkan device available";
    EXPECT_FALSE(builds->FoamWarmAfterPause) << "the live frame binds no foam pipeline";
    EXPECT_EQ(builds->FirstLiveFrame, 0u) << "the first live frame builds no ocean pipeline";
}

// A camera under the sea level runs the view simulation with no water in the frustum, so the pause
// builds it.
TEST(RenderPipelineDeclareTests, APausedSubmergedViewRequestsTheOceanSimulationPipelinesOfItsFirstLiveFrame)
{
    const std::optional<PausedOceanBuilds> builds = RunPausedOceanView({.WaterCenterZ = -150.0f, .CameraY = -5.0f});
    if (!builds)
        GTEST_SKIP() << "No Vulkan device available";
    EXPECT_GE(builds->Paused, 1u) << "the submerged view runs the simulation, so the pause builds it";
    EXPECT_TRUE(builds->FoamWarmAfterPause) << "the foam step of the live frame builds during the pause";
    EXPECT_EQ(builds->FirstLiveFrame, 0u) << "the first live frame builds no ocean pipeline";
}

// With the engine's job system up, the paused surface registers its material as scene materials
// register: the base program and pipeline build on a worker, not on the main thread in the paused
// frame. The first live frame draws with that material and builds no ocean pipeline.
TEST(RenderPipelineDeclareTests, APausedViewCompilesTheOceanSurfaceMaterialOffTheMainThread)
{
    const std::optional<PausedOceanBuilds> builds = RunPausedOceanView({.EngineUp = true});
    if (!builds)
        GTEST_SKIP() << "No Vulkan device available";
    EXPECT_TRUE(builds->SurfaceCompileOnWorker) << "the paused frame compiled the surface material on the main thread";
    EXPECT_GE(builds->Paused, 2u) << "the paused frames build the view simulation and the surface";
    EXPECT_EQ(builds->FirstLiveFrame, 0u) << "the first live frame builds no ocean pipeline";
}

// The surface material holds ready-to-show until its base pipeline publishes, not only while it
// compiles: the paused frame after the publish asks for the surface pipeline, which holds in turn.
TEST(RenderPipelineDeclareTests, APausedViewIsNotReadyToShowUntilItsOceanSurfaceMaterialPublishes)
{
    const std::optional<PausedOceanBuilds> builds = RunPausedOceanView({.EngineUp = true});
    if (!builds)
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(builds->SurfaceCompileOnWorker) << "the surface material compiled on the main thread";
    EXPECT_FALSE(builds->ReadyBeforePublish) << "ready to show before the surface material published";
    EXPECT_TRUE(builds->ReadyAfterPublish) << "held after the publish: " << builds->StatusAfterPublish;
}

// The live draw registers the surface material with its base compile inline: the frame that
// registers it has its pipeline, so a live view never draws a frame without water.
TEST(RenderPipelineDeclareTests, ALiveViewCompilesTheOceanSurfaceMaterialInTheFrameThatRegistersIt)
{
    const std::optional<PausedOceanBuilds> builds = RunPausedOceanView({.EngineUp = true, .LiveFromBoot = true});
    if (!builds)
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(builds->SurfaceRegistered) << "the live frames drew no surface";
    EXPECT_FALSE(builds->SurfaceCompileOnWorker) << "the live draw left the surface material to a worker";
}

// With the underwater look on, the paused pre-world step records the view's submersion, as the live
// one does: the post-world step and the surface's two-sided choice read it.
TEST(RenderPipelineDeclareTests, APausedSubmergedViewWithTheUnderwaterLookRecordsItsSubmersion)
{
    const std::optional<PausedOceanBuilds> builds =
        RunPausedOceanView({.WaterCenterZ = -150.0f, .CameraY = -5.0f, .UnderwaterLook = true});
    if (!builds)
        GTEST_SKIP() << "No Vulkan device available";
    EXPECT_TRUE(builds->UnderwaterAfterPause) << "the paused frames recorded no submersion for the view";
}

// Gameplay queries of the waves keep the wave simulation running with no water in view, so the pause
// builds it.
TEST(RenderPipelineDeclareTests, APausedViewWithoutWaterRequestsTheWavePipelinesThatGameplayQueriesRun)
{
    const std::optional<PausedOceanBuilds> builds =
        RunPausedOceanView({.FoamStepPending = false, .WaterCenterZ = -150.0f, .SurfaceQuery = true});
    if (!builds)
        GTEST_SKIP() << "No Vulkan device available";
    EXPECT_GE(builds->Paused, 1u) << "the queries run the wave simulation, so the pause builds it";
    EXPECT_EQ(builds->FirstLiveFrame, 0u) << "the first live frame builds no ocean pipeline";
}
