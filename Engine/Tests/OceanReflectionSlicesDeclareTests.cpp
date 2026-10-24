// The water surface keeps writing the screen-space reflection slices the world pass started, so the
// reflections replace the water's own reflection: with reflections active its pass attaches the
// three slices the world pass wrote, at the world pass's locations, loaded and stored, and with
// reflections off it attaches none, and it draws with the program that writes them. The
// reflections trace from the depth with the water in it, and march a pyramid built from that
// same depth.
// Part of RenderPipelineDeclareTests.
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/Pipeline/Nodes/DepthResolveNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ScreenSpaceReflectionsNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ViewParamsUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/VolumetricFogNode.h"
#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Matrix4x4.h"
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanRenderNode.h"
#include "Ocean/OceanSurfaceMaterial.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "SharedHeadlessDevice.h"
#include "StagedTestPaths.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;

namespace
{
struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 262144) {}
};

TextureDesc TargetDesc(TextureFormat format, uint32_t usage)
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

struct WaterSliceDecl
{
    bool SurfaceDrawn = false;  // an OceanSurface pass was declared
    bool SlicesWritten = false; // the world pass published the three written slices
    uint32_t SlicesAttached = 0;
    bool LoadedAndStored = true;
    bool AtTheWorldLocations = true;
    bool ReflectionsDeclared = false;           // a reflection pass reads anything
    bool ReflectionsReadPostOceanDepth = false; // a reflection pass samples the depth with the water
    bool ReflectionsReadPreOceanDepth = false;  // a reflection pass samples the depth without it
    // The reflection pyramid (View.SSRHiZ): built this frame, the name of the depth its build reads,
    // and whether every pass that wrote that depth ran after the last write of the view depth.
    bool PyramidBuilt = false;
    std::string PyramidSource;
    bool PyramidSourceAfterLastDepthWrite = false;
    bool ReflectionsSamplePyramidSource = false; // the reflection passes sample that same depth
    uint32_t PostWorldResolves = 0;              // passes that write View.DepthResolvedPostOcean
    // The surface registrations for the world pass's keywords, with and without the slices keyword.
    bool SlicesProgramRegistered = false;
    bool ColorOnlyProgramRegistered = false;
    int SlicesProgramHighestLocation = -1; // the highest color location its fragment stage writes
};

// The highest color location the fragment stage of `material` writes, or -1 with no color output.
int HighestFragmentColorLocation(const GameEngine::Engine::Renderer::Material& material)
{
    const auto& meta = material.GetShaderMeta();
    if (!meta)
        return -1;
    const auto fs = meta->Stages.find("fs");
    if (fs == meta->Stages.end())
        return -1;
    int highest = -1;
    for (const auto& output : fs->second.Outputs)
        if (!output.Builtin)
            highest = std::max(highest, static_cast<int>(output.Location));
    return highest;
}

RenderPipelineBlueprint::Pass Step(const char* id, const char* type, const char* json)
{
    RenderPipelineBlueprint::Pass p;
    p.id = id;
    p.type = type;
    p.enabled = true;
    p.perView = true;
    p.passJson = json;
    return p;
}

/// WorldRender + OceanRender over one single-sample view looking at a water body, with screen-space
/// reflections active or not for the view; with `withReflectionsNode`, the reflections node after
/// the water as in the shipped ForwardPlus graph; with `withFog`, the volumetric fog after the
/// reflections, active for the view.
class WaterReflectionSlices
{
  public:
    bool Up(IDevice* device, bool reflectionsActive, bool withReflectionsNode = false, bool waterInView = true,
            bool withFog = false)
    {
        m_Device = device;
        m_WithReflectionsNode = withReflectionsNode;
        m_WaterInView = waterInView;
        m_WithFog = withFog;
        m_Rs = std::make_unique<RenderServices>();
        if (!m_Rs->Initialize(device))
            return false;
        // The ocean surface material compiles through the real composer from the staged shader
        // mirror; without it the surface never draws.
        Rendering::MaterialBuildContext mbc{};
        mbc.AdapterShaderDir = GameEngine::TestPaths::StagedRenderingShadersDir();
        mbc.CacheRoot = std::filesystem::temp_directory_path() / "ge_ocean_reflection_slices_cache";
        mbc.IncludeDirs = {mbc.AdapterShaderDir};
        mbc.PackageShaderDirs = {mbc.AdapterShaderDir};
        m_Rs->Materials().SetMaterialBuildContext(mbc);
        // Looking along +Z from 50 m above the sea at water 150 m ahead.
        const Mathematics::Matrix4x4 view = Mathematics::Matrix4x4::LookAt(
            Mathematics::Vector3(0.0f, 50.0f, 0.0f), Mathematics::Vector3(0.0f, 50.0f, 1.0f),
            Mathematics::Vector3(0.0f, 1.0f, 0.0f));
        const Mathematics::Matrix4x4 projection =
            Mathematics::Matrix4x4::PerspectiveReverseZ(1.0471976f, 1.0f, 0.5f, 600.0f);
        const Mathematics::Matrix4x4 viewProj = projection * view;
        CameraData camera{};
        std::memcpy(camera.view, view.Data(), sizeof(camera.view));
        std::memcpy(camera.proj, projection.Data(), sizeof(camera.proj));
        std::memcpy(camera.viewProj, viewProj.Data(), sizeof(camera.viewProj));
        camera.cameraPos[1] = 50.0f;
        const CameraId cam = m_Rs->Views().AllocateCamera("WaterSlicesCam");
        m_Rs->Views().SetCameraData(cam, camera);
        m_View = m_Rs->Views().AllocateView("WaterSlicesView", cam);
        m_Rs->Views().SetViewRenderLayerMask(m_View, GameEngine::Rendering::RenderLayerMask::Default());
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        m_Rs->Views().SetViewTargets(m_View, 0, 0, 0, clear);
        PostProcessSettings pp{};
        pp.SSSRIntensity = reflectionsActive ? 1.0f : 0.0f;
        pp.VolumetricFogIntensity = withFog ? 1.0f : 0.0f;
        pp.VolumetricFogDensity = 0.02f;
        m_Rs->Views().SetViewPostProcessOverride(m_View, pp);

        auto& ocean = m_Rs->EnsureFeature<GameEngine::Ocean::OceanRenderFeature>();
        if (!ocean.Initialize(device))
            return false;
        ocean.SetHasOcean(true);
        GameEngine::Ocean::OceanRenderFeature::WaterBodyBox water{};
        // In front of the camera, or behind it when the view must not see water.
        water.CenterZ = waterInView ? 150.0f : -150.0f;
        water.HalfX = 40.0f;
        water.HalfZ = 40.0f;
        ocean.SetWaterBodies({water}, true);
        m_Rs->EnsureFeature<ImageBasedLightingFeature>().Initialize(device);

        return m_Registry.Register(
                   "DepthResolve", [] { return std::make_unique<Nodes::DepthResolveNode>(); }, true) &&
               m_Registry.Register(
                   "ViewParamsUpload", [] { return std::make_unique<Nodes::ViewParamsUploadNode>(); }, true) &&
               m_Registry.Register(
                   "ScreenSpaceReflections", [] { return std::make_unique<Nodes::ScreenSpaceReflectionsNode>(); },
                   true) &&
               m_Registry.Register(
                   "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true) &&
               m_Registry.Register(
                   "VolumetricFog", [] { return std::make_unique<Nodes::VolumetricFogNode>(); }, true) &&
               m_Registry.Register(
                   "OceanRender", [] { return std::make_unique<GameEngine::Ocean::OceanRenderNode>(); }, true);
    }

    // The first frame registers the ocean's surface contributor and its material may compile over a
    // few frames: declare until the surface draws, or a bounded number of frames when it never does.
    WaterSliceDecl Declare()
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "WaterReflectionSlices";
        bp.passes.push_back(Step("ViewParams", "ViewParamsUpload", R"({"id":"ViewParams","type":"ViewParamsUpload"})"));
        // The view depth resolved before the water draws, as in the shipped graph.
        bp.passes.push_back(Step("DepthResolve", "DepthResolve", R"({"id":"DepthResolve","type":"DepthResolve"})"));
        bp.passes.push_back(Step("Ocean", "OceanRender", R"({"id":"Ocean","type":"OceanRender"})"));
        bp.passes.push_back(Step("World", "WorldRender", R"({"id":"World","type":"WorldRender"})"));
        bp.passes.push_back(Step("OceanUnderwater", "OceanRender",
                                 R"({"id":"OceanUnderwater","type":"OceanRender","mode":"underwater"})"));
        bp.worldColorResolveTargetRef = "SceneColor";
        bp.resources.push_back(
            {"SceneColor",
             R"({"kind":"texture","scope":"perView","format":"R16G16B16A16_FLOAT","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
        if (m_WithReflectionsNode)
        {
            bp.passes.push_back(
                Step("Sssr", "ScreenSpaceReflections",
                     R"({"id":"Sssr","type":"ScreenSpaceReflections","input":"SceneColor","output":"HDRSSSR"})"));
            bp.resources.push_back(
                {"HDRSSSR",
                 R"({"kind":"texture","scope":"perView","format":"R16G16B16A16_FLOAT","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource","unorderedAccess"]})"});
            bp.outputs.push_back({"FinalColor", "HDRSSSR"});
        }
        else
            bp.outputs.push_back({"FinalColor", "SceneColor"});
        if (m_WithFog)
            bp.passes.push_back(Step("Fog", "VolumetricFog",
                                     m_WithReflectionsNode
                                         ? R"({"id":"Fog","type":"VolumetricFog","output":"HDRSSSR"})"
                                         : R"({"id":"Fog","type":"VolumetricFog","output":"SceneColor"})"));
        RenderPipelineInstance instance(*m_Rs, m_Registry);
        instance.SetBlueprint(bp);
        constexpr uint32_t kMaxFrames = 30;
        WaterSliceDecl last;
        for (uint32_t i = 0; i < kMaxFrames && !(m_WaterInView ? last.SurfaceDrawn : last.ReflectionsDeclared); ++i)
            last = DeclareOneFrame(instance, i);
        // The surface registers one program per set of its pass keywords on first use: the
        // registrations present are the programs it drew with.
        auto worldKeywords = MaterialKeyword::None;
        if (const auto kw = m_Rs->GetWorldPassKeywords(m_View))
            worldKeywords = *kw;
        auto& registry = m_Rs->Materials().Registry();
        const MaterialKeyword colorOnly = worldKeywords & ~MaterialKeyword::SSSRNormalRoughness;
        const GameEngine::Engine::Renderer::Material* slices =
            registry.Find(GameEngine::Ocean::OceanSurfaceMaterialGuid(colorOnly | MaterialKeyword::SSSRNormalRoughness));
        last.SlicesProgramRegistered = slices != nullptr;
        last.ColorOnlyProgramRegistered = registry.Find(GameEngine::Ocean::OceanSurfaceMaterialGuid(colorOnly)) != nullptr;
        if (slices)
            last.SlicesProgramHighestLocation = HighestFragmentColorLocation(*slices);
        return last;
    }

    void Down()
    {
        if (!m_Rs)
            return;
        m_Device->WaitForIdle();
        m_Rs->Shutdown();
    }

  private:
    WaterSliceDecl DeclareOneFrame(RenderPipelineInstance& instance, uint32_t index)
    {
        WaterSliceDecl out;
        FramePools pools(m_Device);
        RenderGraph::RGFrame frame(m_Device, &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(index);
        const RenderGraph::RGTexture color = frame.ImportPersistentTexture(
            "WS.Color",
            TargetDesc(TextureFormat::R16G16B16A16_FLOAT,
                       static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource)));
        const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(
            "WS.Depth",
            TargetDesc(TextureFormat::D32_FLOAT,
                       static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource)));
        m_Rs->BeginWorldDrawFrame();
        m_Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{m_View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(m_Rs->Views().GetViews().begin(),
                                                     m_Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        const RenderGraph::RGGraph& g = frame.Graph();
        RenderGraph::RGPassId surface{};
        std::set<RenderGraph::RGPassId> postWorldWriters;
        for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        {
            const char* pn = g.PassName(a.Pass);
            const char* rn = g.ResourceName(a.Resource);
            if (!pn || !rn)
                continue;
            const std::string pass(pn), resource(rn);
            if (pass.find("OceanSurface") != std::string::npos)
            {
                out.SurfaceDrawn = true;
                surface = a.Pass;
            }
            if (resource.find("DepthResolvedPostOcean") != std::string::npos && RenderGraph::IsWrite(a.Access))
                postWorldWriters.insert(a.Pass);
            const bool sampled = a.Access == RenderGraph::RGAccess::Sampled ||
                                 a.Access == RenderGraph::RGAccess::SampledCompute;
            // The reflection passes carry the blueprint step's id.
            const bool reflectionPass = pass.find(".Sssr.") != std::string::npos;
            if (reflectionPass)
                out.ReflectionsDeclared = true;
            if (sampled && reflectionPass && resource.find("DepthResolved") != std::string::npos)
            {
                if (resource.find("DepthResolvedPostOcean") != std::string::npos)
                    out.ReflectionsReadPostOceanDepth = true;
                else
                    out.ReflectionsReadPreOceanDepth = true;
            }
        }
        out.PostWorldResolves = static_cast<uint32_t>(postWorldWriters.size());
        DescribeReflectionPyramid(g, out);
        // The world pass's slice locations (ReflectionsProvider): normal 1, weight 2, radiance 3.
        const std::pair<const char*, uint32_t> slices[] = {{"View.NormalRoughness.Written", 1u},
                                                           {"View.SSRSpecularWeight.Written", 2u},
                                                           {"View.SSRSpecularRadiance.Written", 3u}};
        const auto* resources = instance.FrameResourcesFor(&frame);
        out.SlicesWritten = resources != nullptr;
        for (const auto& [name, location] : slices)
        {
            RenderGraph::RGTexture slice{};
            if (resources)
                if (const auto it = resources->Textures.find({m_View, name}); it != resources->Textures.end())
                    slice = it->second;
            if (!slice.IsValid())
            {
                out.SlicesWritten = false;
                continue;
            }
            for (const auto& att : frame.Attachments())
            {
                if (!out.SurfaceDrawn || att.Pass != surface || att.IsDepth || att.Tex != slice.Id)
                    continue;
                ++out.SlicesAttached;
                out.LoadedAndStored &= att.Ops.Load == RenderGraph::RGLoadOp::Load &&
                                       att.Ops.Store == RenderGraph::RGStoreOp::Store;
                out.AtTheWorldLocations &= att.Slot == location;
            }
        }
        frame.MarkOutput(color);
        frame.Execute();
        m_Device->WaitForIdle();
        return out;
    }

    // The pyramid's build: the depth it reads, and the passes that wrote that depth against the
    // last write of the view depth ("WS.Depth"). Passes are numbered in declaration order.
    static void DescribeReflectionPyramid(const RenderGraph::RGGraph& g, WaterSliceDecl& out)
    {
        const auto nameOf = [&](RenderGraph::RGResourceId id) {
            const char* n = g.ResourceName(id);
            return std::string(n ? n : "");
        };
        RenderGraph::RGPassId lastDepthWrite = 0;
        bool depthWritten = false;
        std::vector<RenderGraph::RGPassId> pyramidPasses;
        for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        {
            const std::string resource = nameOf(a.Resource);
            if (a.Access == RenderGraph::RGAccess::DepthWrite && resource == "WS.Depth")
            {
                lastDepthWrite = depthWritten ? std::max(lastDepthWrite, a.Pass) : a.Pass;
                depthWritten = true;
            }
            if (a.Access == RenderGraph::RGAccess::StorageWrite && resource.find("SSRHiZ") != std::string::npos)
                pyramidPasses.push_back(a.Pass);
        }
        out.PyramidBuilt = !pyramidPasses.empty();
        RenderGraph::RGResourceId source = RenderGraph::kInvalidId;
        for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        {
            const bool pyramidPass =
                std::find(pyramidPasses.begin(), pyramidPasses.end(), a.Pass) != pyramidPasses.end();
            if (pyramidPass && a.Access == RenderGraph::RGAccess::StorageRead &&
                nameOf(a.Resource).find("SSRHiZ") == std::string::npos)
                source = a.Resource;
        }
        if (source == RenderGraph::kInvalidId)
            return;
        out.PyramidSource = nameOf(source);
        bool sourceWritten = false;
        bool afterLastDepthWrite = depthWritten;
        for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        {
            if (a.Resource != source)
                continue;
            if (RenderGraph::IsWrite(a.Access))
            {
                sourceWritten = true;
                afterLastDepthWrite &= a.Pass > lastDepthWrite;
            }
            const char* pn = g.PassName(a.Pass);
            const bool sampled = a.Access == RenderGraph::RGAccess::Sampled ||
                                 a.Access == RenderGraph::RGAccess::SampledCompute;
            if (sampled && pn && std::string(pn).find(".Sssr.") != std::string::npos)
                out.ReflectionsSamplePyramidSource = true;
        }
        out.PyramidSourceAfterLastDepthWrite = sourceWritten && afterLastDepthWrite;
    }

    IDevice* m_Device = nullptr;
    bool m_WithReflectionsNode = false;
    bool m_WaterInView = true;
    bool m_WithFog = false;
    std::unique_ptr<RenderServices> m_Rs;
    RenderPipelineNodeRegistry m_Registry;
    Rendering::ViewId m_View{};
};
} // namespace

TEST(RenderPipelineDeclareTests, WaterSurfaceKeepsWritingTheReflectionSlices)
{
    auto* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    WaterReflectionSlices withReflections;
    ASSERT_TRUE(withReflections.Up(device, true));
    const WaterSliceDecl on = withReflections.Declare();
    withReflections.Down();
    ASSERT_TRUE(on.SurfaceDrawn) << "positive control: the water surface must draw";
    ASSERT_TRUE(on.SlicesWritten) << "positive control: the world pass must write the slices";
    EXPECT_EQ(on.SlicesAttached, 3u) << "the water must attach the normal, weight and radiance slices";
    EXPECT_TRUE(on.LoadedAndStored) << "the slices hold what the world pass wrote: load and store them";
    EXPECT_TRUE(on.AtTheWorldLocations) << "the water writes the slices at the world pass's locations";

    WaterReflectionSlices withoutReflections;
    ASSERT_TRUE(withoutReflections.Up(device, false));
    const WaterSliceDecl off = withoutReflections.Declare();
    withoutReflections.Down();
    ASSERT_TRUE(off.SurfaceDrawn) << "positive control: the water surface must draw without reflections";
    EXPECT_EQ(off.SlicesAttached, 0u) << "without reflections the water attaches no slice";
}

// The pass attaches the slices only when the surface draws with the program that writes them: the
// surface's registration for its pass keywords carries the slices keyword, and its fragment stage
// writes locations 1 to 3. A surface that drew the color-only program would leave the slices'
// clear values under the water, and the reflections would skip it.
TEST(RenderPipelineDeclareTests, WaterSurfaceDrawsTheProgramThatWritesTheSlices)
{
    auto* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    WaterReflectionSlices withReflections;
    ASSERT_TRUE(withReflections.Up(device, true));
    const WaterSliceDecl on = withReflections.Declare();
    withReflections.Down();
    ASSERT_TRUE(on.SurfaceDrawn) << "positive control: the water surface must draw";
    ASSERT_EQ(on.SlicesAttached, 3u) << "positive control: the pass must attach the slices";
    EXPECT_TRUE(on.SlicesProgramRegistered) << "the surface must register the program with the slices keyword";
    EXPECT_FALSE(on.ColorOnlyProgramRegistered) << "with the slices attached the surface must not draw the color-only program";
    EXPECT_EQ(on.SlicesProgramHighestLocation, 3) << "the slices program must write locations 1 to 3";

    WaterReflectionSlices withoutReflections;
    ASSERT_TRUE(withoutReflections.Up(device, false));
    const WaterSliceDecl off = withoutReflections.Declare();
    withoutReflections.Down();
    ASSERT_TRUE(off.SurfaceDrawn) << "positive control: the water surface must draw without reflections";
    EXPECT_TRUE(off.ColorOnlyProgramRegistered) << "without reflections the surface draws the color-only program";
    EXPECT_FALSE(off.SlicesProgramRegistered) << "without reflections the surface registers no slices program";
}

// The water is not in the depth the prepass resolves: a reflection traced from that depth starts at
// the seabed or the sky under the water, and the composite skips sky pixels. The reflections read
// the depth the ocean's step resolves after the surface when the water draws.
TEST(RenderPipelineDeclareTests, ReflectionsTraceFromTheDepthWithTheWater)
{
    auto* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    WaterReflectionSlices rig;
    ASSERT_TRUE(rig.Up(device, true, true));
    const WaterSliceDecl decl = rig.Declare();
    rig.Down();
    ASSERT_TRUE(decl.SurfaceDrawn) << "positive control: the water surface must draw";
    ASSERT_TRUE(decl.ReflectionsDeclared) << "positive control: the reflections must declare";
    EXPECT_TRUE(decl.ReflectionsReadPostOceanDepth)
        << "the reflections must sample View.DepthResolvedPostOcean when the water draws";
    EXPECT_FALSE(decl.ReflectionsReadPreOceanDepth)
        << "the reflections must not trace from the depth without the water";
}

// The reflections march a pyramid and confirm a hit against the depth: both must be the same depth,
// taken after its last writer, or a ray toward the water crosses at the seabed in the pyramid and
// is rejected against the water. With water in view the pyramid is built from the ocean step's
// depth after the surface; without it, from the same name resolved by the reflections after every
// pass that writes the view depth (the phase-B pass among them), so the fog and the exposure meter
// reuse that copy: with the fog on and no water the view resolves that depth once. Never from
// View.DepthResolved, which the prepass resolves before both.
TEST(RenderPipelineDeclareTests, ReflectionPyramidIsBuiltFromTheDepthItsHitsAreTestedAgainst)
{
    auto* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    WaterReflectionSlices water;
    ASSERT_TRUE(water.Up(device, true, true));
    const WaterSliceDecl withWater = water.Declare();
    water.Down();
    ASSERT_TRUE(withWater.SurfaceDrawn) << "positive control: the water surface must draw";
    ASSERT_TRUE(withWater.ReflectionsDeclared) << "positive control: the reflections must declare";
    ASSERT_TRUE(withWater.PyramidBuilt) << "the reflections must build their pyramid this frame";
    EXPECT_NE(withWater.PyramidSource.find("DepthResolvedPostOcean"), std::string::npos)
        << "with water the pyramid must be built from the depth after the surface, not " << withWater.PyramidSource;
    EXPECT_TRUE(withWater.PyramidSourceAfterLastDepthWrite)
        << "the pyramid's depth must be taken after the last write of the view depth";
    EXPECT_TRUE(withWater.ReflectionsSamplePyramidSource) << "the hit test must read the pyramid's depth";

    WaterReflectionSlices dry;
    ASSERT_TRUE(dry.Up(device, true, true, false, true));
    const WaterSliceDecl withoutWater = dry.Declare();
    dry.Down();
    ASSERT_TRUE(withoutWater.ReflectionsDeclared) << "positive control: the reflections must declare";
    EXPECT_FALSE(withoutWater.SurfaceDrawn) << "positive control: the water is behind the camera";
    ASSERT_TRUE(withoutWater.PyramidBuilt) << "the reflections must build their pyramid this frame";
    EXPECT_NE(withoutWater.PyramidSource.find("DepthResolvedPostOcean"), std::string::npos)
        << "without water the pyramid must be built from the depth after the world, not "
        << withoutWater.PyramidSource;
    EXPECT_EQ(withoutWater.PostWorldResolves, 1u) << "the reflections and the fog share one post-world resolve";
    EXPECT_TRUE(withoutWater.PyramidSourceAfterLastDepthWrite)
        << "the pyramid's depth must be taken after the last write of the view depth";
    EXPECT_TRUE(withoutWater.ReflectionsSamplePyramidSource) << "the hit test must read the pyramid's depth";
}
