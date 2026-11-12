// The light grid of an orthographic view, run on the GPU as ForwardPlus.rendergraph ships it: the view
// block upload, the light upload, the depth min/max and the clustered light cull. A ground plane and a
// point light 1 m above it; the cluster under the light lists the light. Part of RenderPipelineDeclareTests.
#include "Engine/Rendering/Pipeline/Nodes/EngineNodeTypes.h"
#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "SharedHeadlessDevice.h"
#include "TestCameraEye.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;

namespace
{

constexpr uint32_t kExtent = 128;
constexpr float kNear = 0.1f;
constexpr float kRange = 100.0f;
constexpr float kOrthographicHeight = 20.0f;
constexpr float kLightRange = 3.0f;
// The strategy pose: 20 m up and 20 m back, pitched 45 degrees down at the origin.
const Mathematics::Vector3 kEye(0.0f, 20.0f, -20.0f);
const Mathematics::Vector3 kLightPosition(0.5f, 1.0f, 0.5f);
// 8 m beyond the light along the view: a deeper slice than any point in the light's range.
const Mathematics::Vector3 kFarGroundPoint(0.5f, 0.0f, 8.5f);

nlohmann::json ShippedPipeline()
{
    const std::filesystem::path path =
        std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Engine/Assets/RenderPipelines/ForwardPlus.rendergraph";
    std::ifstream in(path);
    EXPECT_TRUE(in.is_open()) << path;
    std::stringstream text;
    text << in.rdbuf();
    return nlohmann::json::parse(text.str());
}

RenderPipelineBlueprint LightGridBlueprint()
{
    const nlohmann::json pipeline = ShippedPipeline();
    RenderPipelineBlueprint bp;
    bp.pipelineName = "OrthographicLightGrid";
    for (const char* id : {"ViewParamsUpload", "LightUpload", "DepthMinMax", "ClusteredLightCull"})
    {
        bool found = false;
        for (const nlohmann::json& pass : pipeline["passes"])
        {
            if (pass.value("id", std::string()) != id)
                continue;
            RenderPipelineBlueprint::Pass step;
            step.id = id;
            step.type = pass.value("type", std::string());
            step.enabled = true;
            step.perView = true;
            nlohmann::json passJson = pass;
            // No depth prepass runs here: the min/max reads the view's depth target, the ground plane.
            if (passJson.contains("inputs") && passJson["inputs"].contains("DepthTex"))
                passJson["inputs"]["DepthTex"] = "View.Depth";
            step.passJson = passJson.dump();
            bp.passes.push_back(step);
            found = true;
        }
        EXPECT_TRUE(found) << "ForwardPlus.rendergraph has no pass '" << id << "'";
    }
    for (const auto& [name, resource] : pipeline["resources"].items())
        bp.resources.push_back({name, resource.dump()});
    for (const auto& [name, value] : pipeline["constants"].items())
        bp.constants[name] = value.get<double>();
    bp.outputs.push_back({"FinalColor", "View.Resolve"});
    return bp;
}

// The reverse-Z depth of the ground plane (y = 0) at each pixel center; 0 where the pixel sees sky.
// Row 0 is the top of the view (NDC y = +1), as the cull reads it.
std::vector<float> GroundDepth(const Mathematics::Matrix4x4& viewProj)
{
    const Mathematics::Matrix4x4 inverse = Mathematics::Inverse(viewProj);
    std::vector<float> depth(kExtent * kExtent, 0.0f);
    for (uint32_t row = 0; row < kExtent; ++row)
    {
        for (uint32_t column = 0; column < kExtent; ++column)
        {
            const float x = (static_cast<float>(column) + 0.5f) / kExtent * 2.0f - 1.0f;
            const float y = 1.0f - (static_cast<float>(row) + 0.5f) / kExtent * 2.0f;
            const Mathematics::Vector4 nearH = inverse.Transform(Mathematics::Vector4(x, y, 1.0f, 1.0f));
            const Mathematics::Vector4 farH = inverse.Transform(Mathematics::Vector4(x, y, 0.0f, 1.0f));
            const Mathematics::Vector3 a(nearH.x / nearH.w, nearH.y / nearH.w, nearH.z / nearH.w);
            const Mathematics::Vector3 b(farH.x / farH.w, farH.y / farH.w, farH.z / farH.w);
            if ((a.y > 0.0f) == (b.y > 0.0f))
                continue;
            const float t = a.y / (a.y - b.y);
            const Mathematics::Vector3 ground(a.x + (b.x - a.x) * t, 0.0f, a.z + (b.z - a.z) * t);
            const Mathematics::Vector4 clip =
                viewProj.Transform(Mathematics::Vector4(ground.x, ground.y, ground.z, 1.0f));
            depth[row * kExtent + column] = clip.z / clip.w;
        }
    }
    return depth;
}

// An R32F image holding `depth`, in the shader-resource state: the view's resolved depth.
TextureHandle UploadDepth(IDevice& device, const std::vector<float>& depth)
{
    TextureDesc td{};
    td.width = td.height = kExtent;
    td.depth = td.mipLevels = td.sampleCount = td.arrayLayers = 1;
    // A buffer copy into a depth-format image writes nothing on Vulkan, so the depth is a color image.
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    td.debugName = "OrthographicLightGrid.Depth";
    const TextureHandle texture = device.CreateTexture(td);

    BufferDesc uploadDesc{};
    uploadDesc.size = depth.size() * sizeof(float);
    uploadDesc.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
    uploadDesc.memoryUsage = BufferMemoryUsage::Upload;
    uploadDesc.debugName = "OrthographicLightGrid.DepthUpload";
    const BufferHandle upload = device.CreateBuffer(uploadDesc);
    std::memcpy(device.MapBuffer(upload), depth.data(), uploadDesc.size);
    device.UnmapBuffer(upload);

    auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->CopyBufferToTextureSubresource(upload, texture, 0, 0, kExtent, kExtent);
    commands->Barrier(
        ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest, ResourceState::ShaderResource));
    commands->End();
    device.ExecuteCommandLists({commands.get()});
    device.WaitForIdle();
    device.DestroyBuffer(upload);
    return texture;
}

struct ClusterProbe
{
    uint32_t LightsUnderTheLight = 0;
    uint32_t LightsAtTheFarPoint = 0;
};

// The cluster that holds `point` on the ground: its tile from the projected pixel, its slice from the
// view depth on the cull's logarithmic split.
uint32_t ClusterOf(const Mathematics::Vector3& point, const Mathematics::Matrix4x4& view,
                   const Mathematics::Matrix4x4& viewProj, uint32_t tileSize, uint32_t slices)
{
    const Mathematics::Vector4 clip = viewProj.Transform(Mathematics::Vector4(point.x, point.y, point.z, 1.0f));
    const float x = clip.x / clip.w;
    const float y = clip.y / clip.w;
    const uint32_t column = static_cast<uint32_t>((x * 0.5f + 0.5f) * kExtent);
    const uint32_t row = static_cast<uint32_t>((0.5f - y * 0.5f) * kExtent);
    const float viewDepth = view.TransformPoint(point).z;
    const uint32_t slice =
        static_cast<uint32_t>(std::log(viewDepth / kNear) / std::log(kRange / kNear) * static_cast<float>(slices));
    const uint32_t clustersX = (kExtent + tileSize - 1) / tileSize;
    const uint32_t clustersY = (kExtent + tileSize - 1) / tileSize;
    return column / tileSize + (row / tileSize) * clustersX + slice * clustersX * clustersY;
}

// The light count of the cluster that holds `point`, from the cluster buffer's bytes: a 16-byte header
// (clusters x, clusters y, slices, lights per cluster), then (first index, count) per cluster.
uint32_t ClusterLightCount(const std::vector<uint8_t>& clusterBytes, const Mathematics::Vector3& point,
                           const Mathematics::Matrix4x4& view, const Mathematics::Matrix4x4& viewProj)
{
    uint32_t header[4] = {};
    std::memcpy(header, clusterBytes.data(), sizeof(header));
    if (header[0] == 0 || header[2] == 0)
        return 0;
    const uint32_t id = ClusterOf(point, view, viewProj, kExtent / header[0], header[2]);
    uint32_t entry[2] = {};
    const size_t offset = sizeof(header) + static_cast<size_t>(id) * sizeof(entry);
    if (offset + sizeof(entry) <= clusterBytes.size())
        std::memcpy(entry, clusterBytes.data() + offset, sizeof(entry));
    return entry[1];
}

// The view's cluster buffer after the cull ran, or empty when the pipeline published none.
std::vector<uint8_t> ReadClusters(IDevice& device, RenderGraph::RGFrame& frame,
                                  const RenderPipelineInstance& instance, ViewId viewId)
{
    const PipelineFrameResources* resources = instance.FrameResourcesFor(&frame);
    PipelineBufferBindingRG clusters{};
    if (resources)
    {
        const auto found = resources->Buffers.find({viewId, "ClusterBuffer"});
        if (found != resources->Buffers.end())
            clusters = found->second;
    }
    if (!clusters.Graph.IsValid())
    {
        ADD_FAILURE() << "the pipeline published no cluster buffer for the view (resources: "
                      << (resources ? resources->Buffers.size() : 0u) << " buffers)";
        frame.Execute();
        device.WaitForIdle();
        return {};
    }
    const auto ticket = RequestBufferReadbackRG(&device, frame, clusters.Graph, 0, clusters.Size, "LightGrid.Clusters");
    frame.Execute();
    OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
    device.WaitForIdle();
    BufferReadbackResult result{};
    if (!ticket || !ticket->TryGet(result) || result.bytes.size() < 4 * sizeof(uint32_t))
    {
        ADD_FAILURE() << "the cluster buffer readback returned no header (" << clusters.Size << " bytes declared)";
        return {};
    }
    return result.bytes;
}

std::optional<ClusterProbe> RunLightGrid(IDevice& device, const Mathematics::Matrix4x4& projection)
{
    RenderServices rs;
    EXPECT_TRUE(rs.Initialize(&device));
    const Mathematics::Matrix4x4 view =
        Mathematics::Matrix4x4::LookAt(kEye, Mathematics::Vector3(0.0f, 0.0f, 0.0f), Mathematics::Vector3(0.0f, 1.0f, 0.0f));
    const Mathematics::Matrix4x4 viewProj = projection * view;
    CameraData camera{};
    camera.view = view;
    camera.proj = projection;
    camera.viewProj = viewProj;
    camera.viewProjRel = viewProj;
    camera.cameraPos = Mathematics::Vector4(kEye.x, kEye.y, kEye.z, 0.0f);
    const CameraId cameraId = rs.Views().AllocateCamera("LightGridCam");
    rs.Views().SetCameraData(cameraId, ::GameEngine::Tests::TestCameraWithEye(camera), {kNear, kRange});
    const ViewId viewId = rs.Views().AllocateView("LightGridView", cameraId);
    rs.Views().SetViewRenderLayerMask(viewId, RenderLayerMask::Default());
    rs.Views().SetViewTargets(viewId, 0, 0, 0, ViewClearConfig{});

    RenderPipelineNodeRegistry registry;
    Nodes::RegisterEngineNodeTypes(registry);
    RenderPipelineInstance instance(rs, registry);
    instance.SetBlueprint(LightGridBlueprint());

    const TextureHandle depthImage = UploadDepth(device, GroundDepth(viewProj));
    std::optional<ClusterProbe> probe;
    {
        RenderGraph::RGResourcePool persistent(&device);
        RenderGraph::RGTransientPool transient(&device);
        RenderGraph::RGUploadRing ring(&device, 2, 262144);
        RenderGraph::RGFrame frame(&device, &persistent, &transient, &ring);
        frame.BeginFrame(0);
        TextureDesc colorDesc{};
        colorDesc.width = colorDesc.height = kExtent;
        colorDesc.depth = colorDesc.mipLevels = colorDesc.sampleCount = colorDesc.arrayLayers = 1;
        colorDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        colorDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
        const RenderGraph::RGTexture color = frame.ImportPersistentTexture("LightGrid.Color", colorDesc);
        const RenderGraph::RGTexture depth = frame.ImportExternalTexture(
            "LightGrid.Depth", depthImage, ResourceState::ShaderResource, TextureFormat::R32_FLOAT);

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        ExtractedLight light{};
        light.type = Components::LightType::Point;
        light.positionWS = Mathematics::WorldPosition::FromVector3(kLightPosition);
        light.range = kLightRange;
        light.intensity = 10.0f;
        light.SortId = 1;
        rs.SubmitLight(0u, light);

        const std::vector<ViewTargetsRG> targets{ViewTargetsRG{viewId, color, depth, {}}};
        const std::vector<ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
        instance.Declare(frame, targets, views);

        const std::vector<uint8_t> clusters = ReadClusters(device, frame, instance, viewId);
        if (!clusters.empty())
        {
            const Mathematics::Vector3 underTheLight(kLightPosition.x, 0.0f, kLightPosition.z);
            probe = ClusterProbe{ClusterLightCount(clusters, underTheLight, view, viewProj),
                                 ClusterLightCount(clusters, kFarGroundPoint, view, viewProj)};
        }
    }
    device.DestroyTexture(depthImage);
    rs.Shutdown();
    return probe;
}

} // namespace

// An orthographic view linearizes its depth with the orthographic terms: the cull keeps the light's
// slice, so the ground under the light gets the light. Perspective terms on this view drop the slice.
TEST(OrthographicLightGrid, TheClusterUnderAPointLightListsItInAnOrthographicView)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    const float halfHeight = kOrthographicHeight * 0.5f;
    const auto probe = RunLightGrid(*device, Mathematics::MakeOrthographicLH_ZO_ReverseZ(
                                                 -halfHeight, halfHeight, -halfHeight, halfHeight, kNear, kRange));
    ASSERT_TRUE(probe.has_value());
    EXPECT_EQ(probe->LightsUnderTheLight, 1u) << "the cluster under the light does not list it";
    EXPECT_EQ(probe->LightsAtTheFarPoint, 0u) << "a cluster 8 m beyond the light's range lists it";
}

// The control: the same light and pose through a perspective projection.
TEST(OrthographicLightGrid, TheClusterUnderAPointLightListsItInAPerspectiveView)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    const auto probe =
        RunLightGrid(*device, Mathematics::Matrix4x4::PerspectiveReverseZ(1.0471976f, 1.0f, kNear, kRange));
    ASSERT_TRUE(probe.has_value());
    EXPECT_EQ(probe->LightsUnderTheLight, 1u) << "the cluster under the light does not list it";
    EXPECT_EQ(probe->LightsAtTheFarPoint, 0u) << "a cluster 8 m beyond the light's range lists it";
}
