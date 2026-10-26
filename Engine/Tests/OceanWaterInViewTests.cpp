#include <gtest/gtest.h>

#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Matrix4x4.h"
#include "Ocean/OceanFrameStamp.h"
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanRenderNode.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestDeviceHelper.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;
using namespace GameEngine::Ocean;

namespace
{
constexpr uint32_t kTargetExtent = 64u;
// The camera looks along +Z from 50 m above sea level, so a water body 150 m
// ahead is on screen and one 150 m behind is not.
constexpr float kCameraHeight = 50.0f;
constexpr float kWaterDistance = 150.0f;
constexpr float kWaterHalfExtent = 40.0f;

struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(IDevice* device) : Persistent(device), Transient(device), Ring(device, 2, 262144) {}
};

TextureDesc TargetDesc(TextureFormat format, uint32_t usage, uint32_t extent)
{
    TextureDesc desc{};
    desc.width = extent;
    desc.height = extent;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(format);
    desc.usage = usage;
    return desc;
}

RenderPipelineBlueprint::Pass MakeOceanPass(const char* id, const char* json)
{
    RenderPipelineBlueprint::Pass pass;
    pass.id = id;
    pass.type = "OceanRender";
    pass.enabled = true;
    pass.perView = true;
    pass.passJson = json;
    return pass;
}

// Looks along +Z from cameraX on the x axis, kCameraHeight above sea level.
CameraData ForwardCamera(float cameraX)
{
    const Mathematics::Matrix4x4 view = Mathematics::Matrix4x4::LookAt(
        Mathematics::Vector3(cameraX, kCameraHeight, 0.0f), Mathematics::Vector3(cameraX, kCameraHeight, 1.0f),
        Mathematics::Vector3(0.0f, 1.0f, 0.0f));
    const Mathematics::Matrix4x4 projection =
        Mathematics::Matrix4x4::PerspectiveReverseZ(1.0471976f, 16.0f / 9.0f, 0.5f, 600.0f);
    const Mathematics::Matrix4x4 viewProj = projection * view;
    CameraData camera{};
    std::memcpy(camera.view, view.Data(), sizeof(camera.view));
    std::memcpy(camera.proj, projection.Data(), sizeof(camera.proj));
    std::memcpy(camera.viewProj, viewProj.Data(), sizeof(camera.viewProj));
    camera.cameraPos[0] = cameraX;
    camera.cameraPos[1] = kCameraHeight;
    return camera;
}

// Declares one frame of the ocean's pre-world and post-world nodes for a single
// view and returns the names of the passes they declared.
std::vector<std::string> DeclareOceanFrame(IDevice* device, float waterCenterZ, bool gameplayQueriesSurface)
{
    std::vector<std::string> names;
    RenderServices rs;
    EXPECT_TRUE(rs.Initialize(device));
    const CameraId cameraId = rs.Views().AllocateCamera("OceanInViewCamera");
    rs.Views().SetCameraData(cameraId, ForwardCamera(0.0f));
    const ViewId viewId = rs.Views().AllocateView("OceanInViewView", cameraId);
    rs.Views().SetViewRenderLayerMask(viewId, GameEngine::Rendering::RenderLayerMask::Default());
    ViewClearConfig clear{};
    clear.clearColor = true;
    clear.clearDepth = true;
    rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

    auto& feature = rs.EnsureFeature<OceanRenderFeature>();
    EXPECT_TRUE(feature.Initialize(device));
    feature.SetHasOcean(true);
    OceanRenderFeature::WaterBodyBox water{};
    water.CenterZ = waterCenterZ;
    water.HalfX = kWaterHalfExtent;
    water.HalfZ = kWaterHalfExtent;
    feature.SetWaterBodies({water}, true);
    if (feature.IsFoamReady())
        feature.GetFoamSim().SetFrameInputs(feature.GetDisplacementTexture(), feature.GetDisplacementSampler(),
                                            1u, 0.8f, 0.55f, 1.0f, 1.0f / 60.0f, 1.0f, 15.0f, 10.0f, 1.0f,
                                            10.0f, 0.0f, 0.0f);
    if (gameplayQueriesSurface)
        (void)feature.SampleSurface(0.0f, waterCenterZ);

    RenderPipelineNodeRegistry registry;
    EXPECT_TRUE(registry.Register(
        "OceanRender", [] { return std::make_unique<OceanRenderNode>(); }, true));
    RenderPipelineBlueprint blueprint;
    blueprint.pipelineName = "OceanInViewTest";
    blueprint.passes.push_back(MakeOceanPass("Ocean", R"({"id":"Ocean","type":"OceanRender"})"));
    blueprint.passes.push_back(MakeOceanPass(
        "OceanUnderwater", R"({"id":"OceanUnderwater","type":"OceanRender","mode":"underwater"})"));
    RenderPipelineInstance instance(rs, registry);
    instance.SetBlueprint(blueprint);

    FramePools pools(device);
    RenderGraph::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);
    const auto color = frame.ImportPersistentTexture(
        "OceanInView.Color",
        TargetDesc(TextureFormat::RGBA8_UNORM,
                   static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource), kTargetExtent));
    const auto depth = frame.ImportPersistentTexture(
        "OceanInView.Depth",
        TargetDesc(TextureFormat::D32_FLOAT,
                   static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource), kTargetExtent));
    rs.BeginWorldDrawFrame();
    rs.BuildWorldBatchKeys();
    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
    const std::vector<ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
    instance.Declare(frame, targets, views);

    for (size_t pass = 0; pass < frame.Graph().PassCount(); ++pass)
        names.emplace_back(frame.Graph().PassName(static_cast<RenderGraph::RGPassId>(pass)));
    device->WaitForIdle();
    rs.Shutdown();
    return names;
}

bool Declared(const std::vector<std::string>& names, const std::string& prefix)
{
    for (const std::string& name : names)
        if (name.rfind(prefix, 0) == 0)
            return true;
    return false;
}

std::string Join(const std::vector<std::string>& names)
{
    std::string joined;
    for (const std::string& name : names)
        joined += name + ' ';
    return joined;
}
// Two main views; the second renders at a smaller extent than kTargetExtent.
struct TwoViews
{
    ViewId First = 0;
    ViewId Second = 0;
    uint32_t SecondExtent = 0;
};

void DeclareTwoViewFrame(RenderServices& rs, RenderPipelineInstance& instance, RenderGraph::RGFrame& frame,
                         uint64_t frameIndex, const TwoViews& views)
{
    const uint32_t colorUsage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    const uint32_t depthUsage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    frame.BeginFrame(frameIndex);
    const std::vector<ViewTargetsRG> targets = {
        ViewTargetsRG{views.First,
                      frame.ImportPersistentTexture(
                          "OceanMirror.A.Color", TargetDesc(TextureFormat::RGBA8_UNORM, colorUsage, kTargetExtent)),
                      frame.ImportPersistentTexture(
                          "OceanMirror.A.Depth", TargetDesc(TextureFormat::D32_FLOAT, depthUsage, kTargetExtent)),
                      {}},
        ViewTargetsRG{views.Second,
                      frame.ImportPersistentTexture("OceanMirror.B.Color",
                                                    TargetDesc(TextureFormat::RGBA8_UNORM, colorUsage, views.SecondExtent)),
                      frame.ImportPersistentTexture("OceanMirror.B.Depth",
                                                    TargetDesc(TextureFormat::D32_FLOAT, depthUsage, views.SecondExtent)),
                      {}}};
    rs.BeginWorldDrawFrame();
    rs.BuildWorldBatchKeys();
    const std::vector<ViewDesc> allViews(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
    instance.Declare(frame, targets, allViews);
}

// The camera a mirror renders from; a default camera when it has none.
CameraData MirrorCamera(RenderServices& rs, const OceanPlanarReflection::Mirror& mirror)
{
    const CameraData* camera = rs.Views().FindCameraData(mirror.CameraId);
    return camera ? *camera : CameraData{};
}

// Declares one view's mirror from a live reference into the view registry, as
// RenderPipelineInstance::Declare hands it to the nodes.
void DeclareMirrorFromLiveView(RenderServices& rs, OceanRenderFeature& feature, RenderGraph::RGFrame& frame,
                               uint64_t frameIndex, ViewId viewId, std::chrono::steady_clock::time_point now)
{
    const ViewDesc* liveView = nullptr;
    for (const ViewDesc& view : rs.Views().GetViews())
        if (view.id == viewId)
            liveView = &view;
    if (!liveView)
    {
        ADD_FAILURE() << "view " << viewId << " is not registered";
        return;
    }
    frame.BeginFrame(frameIndex);
    rs.BeginWorldDrawFrame();
    rs.BuildWorldBatchKeys();
    ViewDeclare declare(frame, *liveView, rs, "Ocean");
    declare.RenderWidth = kTargetExtent;
    declare.RenderHeight = kTargetExtent;
    feature.GetPlanarReflection().DeclareForView(declare, feature.GetParams().SeaLevel,
                                                 feature.GetPlanarReflectionScale(),
                                                 rs.GetDevice()->GetFrameIndex(), now);
}
} // namespace

// With no water in view the ocean declares no simulation or scene-grab pass; a
// gameplay surface query keeps the wave simulation and its height-field bake.
// The surface draw needs content this harness does not build, so the grab, which
// shares its gate, stands for the per-view passes.
TEST(OceanWaterInViewTest, OceanWorkFollowsTheWaterInViewAndGameplayQueries)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        const std::vector<std::string> ahead = DeclareOceanFrame(device.get(), kWaterDistance, false);
        for (const char* pass : {"OceanFFTGenerate", "OceanHeightFieldBake", "OceanFoamSim", "OceanSceneGrab"})
            EXPECT_TRUE(Declared(ahead, pass)) << pass << " with water in view; declared: " << Join(ahead);

        const std::vector<std::string> behind = DeclareOceanFrame(device.get(), -kWaterDistance, false);
        EXPECT_FALSE(Declared(behind, "Ocean")) << "no water in view; declared: " << Join(behind);

        const std::vector<std::string> queried = DeclareOceanFrame(device.get(), -kWaterDistance, true);
        for (const char* pass : {"OceanFFTGenerate", "OceanHeightFieldBake"})
            EXPECT_TRUE(Declared(queried, pass)) << pass << " for a gameplay query; declared: " << Join(queried);
        for (const char* pass : {"OceanFoamSim", "OceanSceneGrab"})
            EXPECT_FALSE(Declared(queried, pass)) << pass << " with no water in view; declared: " << Join(queried);
    }
    device->Shutdown();
}

// The surface's depth is a function of the surface and FFT spectrum blocks it is
// drawn from, so the ocean claims dynamic depth only while those change and a
// view sees the water: still water under a fixed ocean time lets the
// depth-derived passes settle, and so does a running ocean nobody sees.
TEST(OceanWaterInViewTest, FixedOceanTimeOrNoWaterInViewClaimsNoDynamicDepth)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        EXPECT_TRUE(rs.Initialize(device.get()));
        const CameraId cameraId = rs.Views().AllocateCamera("OceanClaimCamera");
        rs.Views().SetCameraData(cameraId, ForwardCamera(0.0f));
        const ViewId viewId = rs.Views().AllocateView("OceanClaimView", cameraId);
        rs.Views().SetViewRenderLayerMask(viewId, GameEngine::Rendering::RenderLayerMask::Default());
        ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        auto& feature = rs.EnsureFeature<OceanRenderFeature>();
        EXPECT_TRUE(feature.Initialize(device.get()));
        feature.SetHasOcean(true);
        OceanRenderFeature::WaterBodyBox water{};
        water.CenterZ = kWaterDistance;
        water.HalfX = kWaterHalfExtent;
        water.HalfZ = kWaterHalfExtent;
        feature.SetWaterBodies({water}, true);

        RenderPipelineNodeRegistry registry;
        EXPECT_TRUE(registry.Register(
            "OceanRender", [] { return std::make_unique<OceanRenderNode>(); }, true));
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "OceanClaimTest";
        blueprint.passes.push_back(MakeOceanPass(
            "OceanUnderwater", R"({"id":"OceanUnderwater","type":"OceanRender","mode":"underwater"})"));
        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(blueprint);

        // One frame as the engine orders it: the extraction publishes the claim
        // for the views declared since its previous update, then the views declare.
        uint32 frameIndex = 0;
        const auto frameAtOceanTime = [&](float oceanTime)
        {
            OceanParamsGPU params = feature.GetParams();
            params.Time = oceanTime;
            feature.SetParams(params);
            feature.PublishDepthClaim();
            const bool claimed = feature.WritesDynamicDepth();
            FramePools pools(device.get());
            RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(frameIndex++);
            const auto color = frame.ImportPersistentTexture(
                "OceanClaim.Color",
                TargetDesc(TextureFormat::RGBA8_UNORM,
                           static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource), kTargetExtent));
            const auto depth = frame.ImportPersistentTexture(
                "OceanClaim.Depth",
                TargetDesc(TextureFormat::D32_FLOAT,
                           static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource), kTargetExtent));
            rs.BeginWorldDrawFrame();
            rs.BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
            const std::vector<ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
            instance.Declare(frame, targets, views);
            return claimed;
        };

        (void)frameAtOceanTime(1.0f);
        EXPECT_TRUE(frameAtOceanTime(1.5f)) << "running ocean time with water in view";
        EXPECT_FALSE(frameAtOceanTime(1.5f)) << "fixed ocean time: the surface depth is unchanged";
        EXPECT_FALSE(frameAtOceanTime(1.5f)) << "fixed ocean time, a further frame";
        OceanFFTParamsGPU spectrum{};
        spectrum.WindSpeed = 12.0f;
        feature.SetFFTParams(spectrum, true);
        EXPECT_TRUE(frameAtOceanTime(1.5f)) << "a spectrum edit under fixed ocean time";
        EXPECT_FALSE(frameAtOceanTime(1.5f)) << "the edited spectrum, unchanged since";
        EXPECT_TRUE(frameAtOceanTime(2.0f)) << "ocean time running again";

        water.CenterZ = -kWaterDistance;
        feature.SetWaterBodies({water}, true);
        (void)frameAtOceanTime(2.5f);
        EXPECT_FALSE(frameAtOceanTime(3.0f)) << "running ocean time with no water in view";

        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// Each view that sees water renders its own planar reflection: its own hidden
// mirror view and camera, and a target sized to that view. Moving one view's
// camera moves only that view's mirror.
TEST(OceanWaterInViewTest, EachViewThatSeesWaterHasItsOwnPlanarReflection)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        EXPECT_TRUE(rs.Initialize(device.get()));
        constexpr uint32_t kSmallExtent = kTargetExtent / 2u;
        const CameraId cameraA = rs.Views().AllocateCamera("OceanMirrorCameraA");
        const CameraId cameraB = rs.Views().AllocateCamera("OceanMirrorCameraB");
        rs.Views().SetCameraData(cameraA, ForwardCamera(0.0f));
        rs.Views().SetCameraData(cameraB, ForwardCamera(10.0f));
        const ViewId viewA = rs.Views().AllocateView("OceanMirrorViewA", cameraA);
        const ViewId viewB = rs.Views().AllocateView("OceanMirrorViewB", cameraB);
        ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        for (const ViewId view : {viewA, viewB})
        {
            rs.Views().SetViewRenderLayerMask(view, GameEngine::Rendering::RenderLayerMask::Default());
            rs.Views().SetViewTargets(view, 0, 0, 0, clear);
        }

        auto& feature = rs.EnsureFeature<OceanRenderFeature>();
        EXPECT_TRUE(feature.Initialize(device.get()));
        feature.SetHasOcean(true);
        OceanRenderFeature::WaterBodyBox water{};
        water.CenterZ = kWaterDistance;
        water.HalfX = kWaterHalfExtent;
        water.HalfZ = kWaterHalfExtent;
        feature.SetWaterBodies({water}, true);
        OceanParamsGPU params = feature.GetParams();
        params.PlanarReflections = 1u;
        feature.SetParams(params);

        RenderPipelineNodeRegistry registry;
        EXPECT_TRUE(registry.Register(
            "OceanRender", [] { return std::make_unique<OceanRenderNode>(); }, true));
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "OceanMirrorTest";
        blueprint.passes.push_back(MakeOceanPass("Ocean", R"({"id":"Ocean","type":"OceanRender"})"));
        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(blueprint);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        uint64_t frameIndex = 0;
        const TwoViews views{viewA, viewB, kSmallExtent};
        DeclareTwoViewFrame(rs, instance, frame, frameIndex++, views);
        const OceanPlanarReflection& reflection = feature.GetPlanarReflection();
        const OceanPlanarReflection::Mirror* mirrorA = reflection.Find(viewA);
        const OceanPlanarReflection::Mirror* mirrorB = reflection.Find(viewB);
        ASSERT_NE(mirrorA, nullptr) << "view A sees water and has no planar reflection";
        ASSERT_NE(mirrorB, nullptr) << "view B sees water and has no planar reflection";
        EXPECT_NE(mirrorA->ViewId, mirrorB->ViewId);
        EXPECT_NE(mirrorA->CameraId, mirrorB->CameraId);
        EXPECT_TRUE(reflection.IsReflectionView(mirrorA->ViewId));
        EXPECT_FALSE(reflection.IsReflectionView(viewA));
        ASSERT_TRUE(mirrorA->ColorTexture.IsValid());
        ASSERT_TRUE(mirrorB->ColorTexture.IsValid());
        EXPECT_NE(mirrorA->ColorTexture, mirrorB->ColorTexture);
        // The default resolution scale is one half of each view's own size.
        EXPECT_EQ(mirrorA->Width, kTargetExtent / 2u);
        EXPECT_EQ(mirrorB->Width, kSmallExtent / 2u);
        // Each mirror camera is its own view's camera reflected below the sea.
        EXPECT_FLOAT_EQ(MirrorCamera(rs, *mirrorA).cameraPos[0], 0.0f);
        EXPECT_FLOAT_EQ(MirrorCamera(rs, *mirrorB).cameraPos[0], 10.0f);
        EXPECT_FLOAT_EQ(MirrorCamera(rs, *mirrorA).cameraPos[1], -kCameraHeight);

        const CameraData mirrorBBefore = MirrorCamera(rs, *mirrorB);
        rs.Views().SetCameraData(cameraA, ForwardCamera(-20.0f));
        DeclareTwoViewFrame(rs, instance, frame, frameIndex++, views);
        EXPECT_FLOAT_EQ(MirrorCamera(rs, *mirrorA).cameraPos[0], -20.0f);
        const CameraData mirrorBAfter = MirrorCamera(rs, *mirrorB);
        EXPECT_EQ(std::memcmp(mirrorBBefore.viewProj, mirrorBAfter.viewProj, sizeof(mirrorBAfter.viewProj)), 0)
            << "moving view A's camera changed view B's reflection camera";
        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// A mirror whose view has stopped declaring is released with its hidden view and
// camera once view B had gone past the timeout at view A's previous declare;
// the view that still declares keeps its mirror. The later declares are handed
// times past the timeout, not slept.
TEST(OceanWaterInViewTest, AMirrorIdlePastTheTimeoutIsReleased)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        EXPECT_TRUE(rs.Initialize(device.get()));
        const CameraId cameraA = rs.Views().AllocateCamera("OceanIdleCameraA");
        const CameraId cameraB = rs.Views().AllocateCamera("OceanIdleCameraB");
        rs.Views().SetCameraData(cameraA, ForwardCamera(0.0f));
        rs.Views().SetCameraData(cameraB, ForwardCamera(10.0f));
        const ViewId viewA = rs.Views().AllocateView("OceanIdleViewA", cameraA);
        const ViewId viewB = rs.Views().AllocateView("OceanIdleViewB", cameraB);
        ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        for (const ViewId view : {viewA, viewB})
        {
            rs.Views().SetViewRenderLayerMask(view, GameEngine::Rendering::RenderLayerMask::Default());
            rs.Views().SetViewTargets(view, 0, 0, 0, clear);
        }

        auto& feature = rs.EnsureFeature<OceanRenderFeature>();
        EXPECT_TRUE(feature.Initialize(device.get()));
        feature.SetHasOcean(true);
        OceanRenderFeature::WaterBodyBox water{};
        water.CenterZ = kWaterDistance;
        water.HalfX = kWaterHalfExtent;
        water.HalfZ = kWaterHalfExtent;
        feature.SetWaterBodies({water}, true);
        OceanParamsGPU params = feature.GetParams();
        params.PlanarReflections = 1u;
        feature.SetParams(params);

        RenderPipelineNodeRegistry registry;
        EXPECT_TRUE(registry.Register(
            "OceanRender", [] { return std::make_unique<OceanRenderNode>(); }, true));
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "OceanIdleMirrorTest";
        blueprint.passes.push_back(MakeOceanPass("Ocean", R"({"id":"Ocean","type":"OceanRender"})"));
        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(blueprint);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        uint64_t frameIndex = 0;
        DeclareTwoViewFrame(rs, instance, frame, frameIndex++, TwoViews{viewA, viewB, kTargetExtent});
        OceanPlanarReflection& reflection = feature.GetPlanarReflection();
        ASSERT_NE(reflection.Find(viewA), nullptr);
        ASSERT_NE(reflection.Find(viewB), nullptr);
        const ViewId mirrorBViewId = reflection.Find(viewB)->ViewId;
        const CameraId mirrorBCameraId = reflection.Find(viewB)->CameraId;
        const size_t viewCountBefore = rs.Views().GetViews().size();

        // Only view A declares: two frames view B misses inside the timeout, then two
        // past it, so A's previous declare is past the timeout at the last one.
        const auto now = std::chrono::steady_clock::now();
        DeclareMirrorFromLiveView(rs, feature, frame, frameIndex++, viewA, now);
        DeclareMirrorFromLiveView(rs, feature, frame, frameIndex++, viewA, now + std::chrono::seconds(1));
        ASSERT_NE(reflection.Find(viewB), nullptr) << "view B's mirror was released before the timeout";
        const auto pastTimeout = now + kOceanViewHistoryIdleTimeout + std::chrono::seconds(1);
        DeclareMirrorFromLiveView(rs, feature, frame, frameIndex++, viewA, pastTimeout);
        DeclareMirrorFromLiveView(rs, feature, frame, frameIndex++, viewA, pastTimeout + std::chrono::seconds(1));

        EXPECT_NE(reflection.Find(viewA), nullptr) << "view A still declares and lost its mirror";
        EXPECT_EQ(reflection.Find(viewB), nullptr) << "view B's mirror outlived the idle timeout";
        EXPECT_FALSE(reflection.IsReflectionView(mirrorBViewId));
        bool mirrorViewRegistered = false;
        for (const ViewDesc& view : rs.Views().GetViews())
            mirrorViewRegistered = mirrorViewRegistered || view.id == mirrorBViewId;
        EXPECT_FALSE(mirrorViewRegistered) << "the released mirror's hidden view is still registered";
        EXPECT_EQ(rs.Views().FindCameraData(mirrorBCameraId), nullptr);
        EXPECT_EQ(rs.Views().GetViews().size(), viewCountBefore - 1u);
        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// The idle sweep erases released mirror views from the registry. A view that
// declares from a live registry reference keeps its own camera when the sweep
// releases a mirror view registered before it.
TEST(OceanWaterInViewTest, TheIdleSweepKeepsTheDeclaringViewsCamera)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        EXPECT_TRUE(rs.Initialize(device.get()));
        ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        const CameraId cameraA = rs.Views().AllocateCamera("OceanSweepCameraA");
        rs.Views().SetCameraData(cameraA, ForwardCamera(0.0f));
        const ViewId viewA = rs.Views().AllocateView("OceanSweepViewA", cameraA);
        rs.Views().SetViewRenderLayerMask(viewA, GameEngine::Rendering::RenderLayerMask::Default());
        rs.Views().SetViewTargets(viewA, 0, 0, 0, clear);

        auto& feature = rs.EnsureFeature<OceanRenderFeature>();
        EXPECT_TRUE(feature.Initialize(device.get()));
        feature.SetHasOcean(true);
        OceanParamsGPU params = feature.GetParams();
        params.PlanarReflections = 1u;
        feature.SetParams(params);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        const auto start = std::chrono::steady_clock::now();
        DeclareMirrorFromLiveView(rs, feature, frame, 0, viewA, start); // registry: A, mirror A

        const CameraId cameraB = rs.Views().AllocateCamera("OceanSweepCameraB");
        rs.Views().SetCameraData(cameraB, ForwardCamera(10.0f));
        const ViewId viewB = rs.Views().AllocateView("OceanSweepViewB", cameraB);
        rs.Views().SetViewRenderLayerMask(viewB, GameEngine::Rendering::RenderLayerMask::Default());
        rs.Views().SetViewTargets(viewB, 0, 0, 0, clear);
        DeclareMirrorFromLiveView(rs, feature, frame, 1, viewB, start); // registry: A, mirror A, B, mirror B
        const OceanPlanarReflection::Mirror* mirrorB = feature.GetPlanarReflection().Find(viewB);
        ASSERT_NE(mirrorB, nullptr);
        const CameraId mirrorBCamera = mirrorB->CameraId;
        const float mirroredY = MirrorCamera(rs, *mirrorB).cameraPos[1];
        EXPECT_LT(mirroredY, 0.0f);

        // Only view B declares, a frame view A misses and then two past the timeout:
        // the sweep releases mirror A, which sits before B.
        const auto pastTimeout = start + kOceanViewHistoryIdleTimeout + std::chrono::seconds(1);
        DeclareMirrorFromLiveView(rs, feature, frame, 2, viewB, start + std::chrono::seconds(1));
        DeclareMirrorFromLiveView(rs, feature, frame, 3, viewB, pastTimeout);
        DeclareMirrorFromLiveView(rs, feature, frame, 4, viewB, pastTimeout + std::chrono::seconds(1));
        ASSERT_EQ(feature.GetPlanarReflection().Find(viewA), nullptr);
        const CameraData* mirrorBCameraData = rs.Views().FindCameraData(mirrorBCamera);
        ASSERT_NE(mirrorBCameraData, nullptr);
        EXPECT_FLOAT_EQ(mirrorBCameraData->cameraPos[1], mirroredY)
            << "view B's mirror was built from another view's camera after the sweep";
        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// One frame longer than the idle timeout (a movie recorder waiting for its
// encoder) releases no mirror: view B declared after view A's previous declare,
// so A's late declare finds B's mirror current, and B keeps the same mirror. A
// re-created mirror draws nothing on its first frame, so the surface would show
// the sky for that frame.
TEST(OceanWaterInViewTest, AFrameLongerThanTheIdleTimeoutKeepsEveryMirror)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        EXPECT_TRUE(rs.Initialize(device.get()));
        ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        const CameraId cameraA = rs.Views().AllocateCamera("OceanLongFrameCameraA");
        const CameraId cameraB = rs.Views().AllocateCamera("OceanLongFrameCameraB");
        rs.Views().SetCameraData(cameraA, ForwardCamera(0.0f));
        rs.Views().SetCameraData(cameraB, ForwardCamera(10.0f));
        const ViewId viewA = rs.Views().AllocateView("OceanLongFrameViewA", cameraA);
        const ViewId viewB = rs.Views().AllocateView("OceanLongFrameViewB", cameraB);
        for (const ViewId view : {viewA, viewB})
        {
            rs.Views().SetViewRenderLayerMask(view, GameEngine::Rendering::RenderLayerMask::Default());
            rs.Views().SetViewTargets(view, 0, 0, 0, clear);
        }

        auto& feature = rs.EnsureFeature<OceanRenderFeature>();
        EXPECT_TRUE(feature.Initialize(device.get()));
        feature.SetHasOcean(true);
        OceanParamsGPU params = feature.GetParams();
        params.PlanarReflections = 1u;
        feature.SetParams(params);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        const auto start = std::chrono::steady_clock::now();
        DeclareMirrorFromLiveView(rs, feature, frame, 0, viewA, start);
        DeclareMirrorFromLiveView(rs, feature, frame, 0, viewB, start + std::chrono::milliseconds(1));
        const OceanPlanarReflection& reflection = feature.GetPlanarReflection();
        ASSERT_NE(reflection.Find(viewB), nullptr);
        const ViewId mirrorBViewId = reflection.Find(viewB)->ViewId;

        // The next frame starts past the timeout; view A declares first.
        const auto late = start + kOceanViewHistoryIdleTimeout + std::chrono::seconds(1);
        DeclareMirrorFromLiveView(rs, feature, frame, 1, viewA, late);
        ASSERT_NE(reflection.Find(viewB), nullptr) << "one long frame released view B's mirror";
        DeclareMirrorFromLiveView(rs, feature, frame, 1, viewB, late + std::chrono::milliseconds(1));
        ASSERT_NE(reflection.Find(viewB), nullptr);
        EXPECT_EQ(reflection.Find(viewB)->ViewId, mirrorBViewId) << "view B's mirror was re-created";
        EXPECT_TRUE(reflection.IsReflectionView(mirrorBViewId));
        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

namespace
{
// Views that declare only their planar reflection mirrors, each declare at a time
// the test hands in, so the idle rule is judged against a chosen cadence and not
// the wall clock.
class OceanMirrorIdleTest : public testing::Test
{
protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Services = std::make_unique<RenderServices>();
        EXPECT_TRUE(m_Services->Initialize(m_Device.get()));
        m_Feature = &m_Services->EnsureFeature<OceanRenderFeature>();
        EXPECT_TRUE(m_Feature->Initialize(m_Device.get()));
        m_Feature->SetHasOcean(true);
        OceanParamsGPU params = m_Feature->GetParams();
        params.PlanarReflections = 1u;
        m_Feature->SetParams(params);
        m_Pools = std::make_unique<FramePools>(m_Device.get());
        m_Frame = std::make_unique<RenderGraph::RGFrame>(m_Device.get(), &m_Pools->Persistent, &m_Pools->Transient,
                                                         &m_Pools->Ring);
        m_Start = std::chrono::steady_clock::now();
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        m_Services->Shutdown();
        m_Frame.reset();
        m_Pools.reset();
        m_Services.reset();
        m_Device->Shutdown();
    }

    void AddViews(int count)
    {
        ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        for (int index = 0; index < count; ++index)
        {
            const std::string name = "OceanMirrorIdleView" + std::to_string(index);
            const CameraId camera = m_Services->Views().AllocateCamera((name + "Camera").c_str());
            m_Services->Views().SetCameraData(camera, ForwardCamera(10.0f * static_cast<float>(index)));
            const ViewId view = m_Services->Views().AllocateView(name.c_str(), camera);
            m_Services->Views().SetViewRenderLayerMask(view, GameEngine::Rendering::RenderLayerMask::Default());
            m_Services->Views().SetViewTargets(view, 0, 0, 0, clear);
            m_Views.push_back(view);
        }
    }

    // Declares view `index`'s mirror `at` after the test's start.
    void Declare(int index, std::chrono::steady_clock::duration at)
    {
        DeclareMirrorFromLiveView(*m_Services, *m_Feature, *m_Frame, m_FrameIndex++, m_Views[index], m_Start + at);
    }

    // The hidden view of view `index`'s mirror, or 0 when it has none.
    ViewId MirrorOf(int index) const
    {
        const OceanPlanarReflection::Mirror* mirror = m_Feature->GetPlanarReflection().Find(m_Views[index]);
        return mirror ? mirror->ViewId : 0;
    }

private:
    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<RenderServices> m_Services;
    OceanRenderFeature* m_Feature = nullptr;
    std::unique_ptr<FramePools> m_Pools;
    std::unique_ptr<RenderGraph::RGFrame> m_Frame;
    std::vector<ViewId> m_Views;
    std::chrono::steady_clock::time_point m_Start;
    uint64_t m_FrameIndex = 0;
};
} // namespace

using namespace std::chrono_literals;

// Two views that take turns every frame keep their mirrors, here at 1 s frames
// for 12 s.
TEST_F(OceanMirrorIdleTest, ViewsThatTakeTurnsKeepTheirMirrors)
{
    AddViews(2);
    Declare(0, 0ms);
    Declare(1, 1ms);
    const ViewId mirror0 = MirrorOf(0);
    const ViewId mirror1 = MirrorOf(1);
    for (std::chrono::seconds at{1}; at <= 12s; ++at)
    {
        Declare(0, at);
        Declare(1, at + 1ms);
    }
    EXPECT_EQ(MirrorOf(0), mirror0);
    EXPECT_EQ(MirrorOf(1), mirror1);
}

// A view that stops declaring (play mode ends) loses its mirror at the first
// declare of the other view whose previous declare is past the timeout: with
// 100 ms frames and the last declare at 1 ms, the declare at 5.2 s.
TEST_F(OceanMirrorIdleTest, AStoppedViewIsReleasedAfterTheTimeout)
{
    AddViews(2);
    Declare(0, 0ms);
    Declare(1, 1ms);
    std::chrono::milliseconds releasedAt{0};
    for (std::chrono::milliseconds at{100}; at <= 7000ms; at += 100ms)
    {
        Declare(0, at);
        if (releasedAt == 0ms && MirrorOf(1) == 0)
            releasedAt = at;
    }
    EXPECT_EQ(releasedAt.count(), 5200);
}

// One long frame keeps both mirrors when the views declare in the other order.
TEST_F(OceanMirrorIdleTest, ALongFrameKeepsMirrorsInEitherDeclareOrder)
{
    AddViews(2);
    Declare(1, 0ms);
    Declare(0, 1ms);
    const ViewId mirror0 = MirrorOf(0);
    const ViewId mirror1 = MirrorOf(1);
    Declare(1, 6s);
    Declare(0, 6s + 1ms);
    EXPECT_EQ(MirrorOf(0), mirror0);
    EXPECT_EQ(MirrorOf(1), mirror1);
}

// A view that declares again after one long frame keeps the same mirror.
TEST_F(OceanMirrorIdleTest, AViewThatDeclaresAgainAfterALongFrameKeepsItsMirror)
{
    AddViews(2);
    Declare(0, 0ms);
    Declare(0, 16ms);
    Declare(1, 17ms);
    const ViewId mirror1 = MirrorOf(1);
    Declare(0, 6s);
    Declare(1, 6s + 1ms);
    Declare(0, 6s + 16ms);
    Declare(1, 6s + 17ms);
    EXPECT_EQ(MirrorOf(1), mirror1);
}

// Three views keep their mirrors across one long frame.
TEST_F(OceanMirrorIdleTest, ThreeViewsKeepTheirMirrorsAcrossALongFrame)
{
    AddViews(3);
    Declare(0, 0ms);
    Declare(1, 1ms);
    Declare(2, 2ms);
    const ViewId mirror0 = MirrorOf(0);
    const ViewId mirror1 = MirrorOf(1);
    const ViewId mirror2 = MirrorOf(2);
    Declare(0, 6s);
    Declare(1, 6s + 1ms);
    Declare(2, 6s + 2ms);
    EXPECT_EQ(MirrorOf(0), mirror0);
    EXPECT_EQ(MirrorOf(1), mirror1);
    EXPECT_EQ(MirrorOf(2), mirror2);
}

// View 0 declares twice per declare of view 1, and the long frame falls between
// view 0's two declares: view 1 keeps its mirror.
TEST_F(OceanMirrorIdleTest, ALongFrameKeepsTheMirrorOfASlowerView)
{
    AddViews(2);
    Declare(0, 0ms);
    Declare(1, 1ms);
    Declare(0, 16ms);
    const ViewId mirror1 = MirrorOf(1);
    Declare(0, 6s);
    EXPECT_EQ(MirrorOf(1), mirror1) << "one long frame released the slower view's mirror";
}
