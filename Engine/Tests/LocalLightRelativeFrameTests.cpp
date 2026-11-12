// Point and spot lights and their shadows 141 km from the origin, through the production upload nodes, shadow
// declaration and clustered_lighting.glsl (Shaders/clustered_lighting_probe.comp), against the same scene near
// the origin. Part of RenderPipelineDeclareTests.
#include "Engine/Rendering/FeatureDeclareContext.h"
#include "Engine/Rendering/Pipeline/Nodes/EngineNodeTypes.h"
#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector3F64.h"
#include "Mathematics/Vector3Int.h"
#include "Mathematics/Vector4.h"
#include "Mathematics/WorldPosition.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "RenderServicesDetail.h"
#include "SharedHeadlessDevice.h"
#include "TestCameraEye.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;
using Mathematics::Vector3;
using Mathematics::WorldPosition;

namespace
{

constexpr float kNear = 0.1f;
constexpr float kRange = 1000.0f;
// The near scene is in sector (0,0,0). The far scene is 141 km from the origin, in sector (98, 0, 98).
const WorldPosition kNearBase{200.0, 0.0, 200.0};
const WorldPosition kFarBase{100000.0, 0.0, 100000.0};
// A scene on a planet's surface with the origin at its center: 6,371 km, where fp32 steps by 0.5 m.
const WorldPosition kEarthRadiusBase{6371000.0, 0.0, 0.0};
// The local scene: the eye 2 m in front of a wall, a point light and a spot light between them.
const Vector3 kEye{0.0f, 1.7f, -2.0f};
const Vector3 kPointLight{0.4f, 1.6f, 2.1f};
const Vector3 kSpotLight{-0.6f, 2.2f, 1.4f};
// A shadow pass camera one sector ahead of the far view, along the view axis.
const Vector3 kOneSectorAhead{0.0f, 0.0f, static_cast<float>(Mathematics::kWorldSectorSize)};
constexpr float kWallZ = 3.0f;
constexpr int kWallSamplesPerSide = 17;
constexpr uint32_t kPunctualResolution = 256; // the shadow map size, texels
constexpr uint32_t kRingFrames = 2;
constexpr uint64_t kRingBytesPerFrame = 256 * 1024;
// The probe's light grid: one tile wider than any screen, one depth slice, one cluster.
constexpr uint32_t kOneTile = 1u << 16;
constexpr uint32_t kLightsPerCluster = 64;
// The adapter's LightUBO: a mat4, five vec4 and two vec4[3], 240 B.
constexpr size_t kLightUboFloats = 60;
constexpr const char* kLightingProbe = "clustered_lighting_probe.comp.spv";
constexpr const char* kViewDepthProbe = "view_depth_probe.comp.spv";

// One sample of a probe shader: a sector-local position in its instance sector.
struct ProbeSample
{
    Mathematics::Vector4 SectorLocal{};
    int32_t Sector[4]{};
    Mathematics::Vector4 Normal{};
};
static_assert(sizeof(ProbeSample) == 48, "ProbeSample mirrors the probe's std430 struct");

// A camera at `eye` that looks along +Z, built as an editor builds it: the double eye through
// SetCameraEye. The rotation does not depend on the eye, so the near and far views turn alike.
CameraData CameraAt(const WorldPosition& eye)
{
    const Vector3 eyeF = eye - WorldPosition{};
    CameraData camera{};
    camera.view = Mathematics::MakeLookAtLH(eyeF, eyeF + Vector3{0.0f, -0.1f, 1.0f}, Vector3{0.0f, 1.0f, 0.0f});
    camera.proj = Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0472f, 1.0f, kNear, kRange);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, eye);
    return camera;
}

std::vector<ExtractedLight> SceneLights(const WorldPosition& base, uint32 spotCastsShadows)
{
    ExtractedLight point{};
    point.type = Components::LightType::Point;
    point.positionWS = base + kPointLight;
    point.range = 6.0f;
    point.intensity = 20.0f;
    point.SortId = 1;

    ExtractedLight spot{};
    spot.type = Components::LightType::Spot;
    spot.positionWS = base + kSpotLight;
    const Vector3 direction = Vector3{0.2f, -0.3f, 0.93f}.Normalize();
    spot.directionWS[0] = direction.x;
    spot.directionWS[1] = direction.y;
    spot.directionWS[2] = direction.z;
    spot.range = 6.0f;
    spot.intensity = 20.0f;
    spot.innerAngle = 0.4f;
    spot.outerAngle = 0.7f;
    spot.castsShadows = spotCastsShadows;
    spot.SortId = 2;
    return {point, spot};
}

// The wall points in front of the lights: world positions, exact in double.
std::vector<WorldPosition> WallPoints(const WorldPosition& base)
{
    std::vector<WorldPosition> points;
    for (int row = 0; row < kWallSamplesPerSide; ++row)
    {
        for (int column = 0; column < kWallSamplesPerSide; ++column)
        {
            const double x = -1.0 + 2.0 * column / (kWallSamplesPerSide - 1);
            const double y = 0.5 + 2.0 * row / (kWallSamplesPerSide - 1);
            points.push_back(WorldPosition{base.X + x, base.Y + y, base.Z + kWallZ});
        }
    }
    return points;
}

std::vector<ProbeSample> ProbeSamples(const std::vector<WorldPosition>& points)
{
    std::vector<ProbeSample> samples;
    for (const WorldPosition& point : points)
    {
        Mathematics::Vector3Int sector;
        Vector3 local;
        point.ToSector(sector, local);
        ProbeSample sample{};
        sample.SectorLocal = Mathematics::Vector4{local, 0.0f};
        sample.Sector[0] = sector.x;
        sample.Sector[1] = sector.y;
        sample.Sector[2] = sector.z;
        sample.Normal = Mathematics::Vector4{0.0f, 0.0f, -1.0f, 0.0f};
        samples.push_back(sample);
    }
    return samples;
}

// A view with its camera, registered with `rs`.
ViewId AddView(RenderServices& rs, const CameraData& camera)
{
    const CameraId cameraId = rs.Views().AllocateCamera("RelativeLightCam");
    rs.Views().SetCameraData(cameraId, camera, {kNear, kRange});
    const ViewId viewId = rs.Views().AllocateView("RelativeLightView", cameraId);
    rs.Views().SetViewRenderLayerMask(viewId, RenderLayerMask::Default());
    rs.Views().SetViewTargets(viewId, 0, 0, 0, ViewClearConfig{});
    return viewId;
}

BufferHandle HostBuffer(IDevice& device, std::vector<BufferHandle>& owned, const void* data, size_t bytes)
{
    BufferDesc desc{};
    desc.size = bytes;
    desc.usage = static_cast<uint32_t>(BufferUsage::Uniform | BufferUsage::Storage);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.debugName = "RelativeLightProbe.Buffer";
    const BufferHandle buffer = device.CreateBuffer(desc);
    if (buffer.IsValid())
    {
        device.UpdateBuffer(buffer, 0, bytes, data);
        owned.push_back(buffer);
    }
    return buffer;
}

// A buffer a view published this frame, or null.
const PipelineBufferBindingRG* FindViewBuffer(const PipelineFrameResources* resources, ViewId viewId,
                                              const char* name)
{
    if (resources == nullptr)
        return nullptr;
    const auto found = resources->Buffers.find({viewId, name});
    return found != resources->Buffers.end() ? &found->second : nullptr;
}

struct ProbeRun
{
    ViewParamsUBO ViewParams{};
    // Per sample: [0] with the relative inputs, [1] with the absolute inputs.
    std::vector<std::array<Mathematics::Vector4, 2>> Results;
};

// Dispatches the probe `shader` over `samples` with the view parameters and the light buffer the production
// nodes wrote for `camera` and `lights`, and with `passCamera` as the camera that draws the pass (Cam). Every
// light sits in the one cluster.
std::optional<ProbeRun> RunProbe(IDevice& device, const char* shader, const CameraData& camera,
                                 const CameraData& passCamera, const std::vector<ExtractedLight>& lights,
                                 const std::vector<ProbeSample>& samples)
{
    const std::string spvPath = std::string(ENGINE_TEST_BUILD_DIR) + "/Shaders/" + shader;
    std::ifstream file(spvPath, std::ios::binary);
    std::vector<uint8_t> spv{std::istreambuf_iterator<char>(file), {}};
    if (spv.empty())
    {
        ADD_FAILURE() << "missing probe shader " << spvPath;
        return std::nullopt;
    }

    RenderServices rs;
    if (!rs.Initialize(&device))
        return std::nullopt;
    const ViewId viewId = AddView(rs, camera);

    RenderPipelineNodeRegistry registry;
    Nodes::RegisterEngineNodeTypes(registry);
    RenderPipelineBlueprint bp;
    bp.pipelineName = "RelativeLightProbe";
    for (const char* type : {"ViewParamsUpload", "LightUpload"})
    {
        RenderPipelineBlueprint::Pass pass;
        pass.id = type;
        pass.type = type;
        pass.enabled = true;
        pass.perView = true;
        pass.passJson = std::string(R"({"id":")") + type + R"(","type":")" + type + R"("})";
        bp.passes.push_back(pass);
    }
    RenderPipelineInstance instance(rs, registry);
    instance.SetBlueprint(bp);

    std::optional<ProbeRun> run;
    std::vector<BufferHandle> owned;
    DescriptorSetHandle set{};
    {
        RenderGraph::RGResourcePool persistent(&device);
        RenderGraph::RGTransientPool transient(&device);
        RenderGraph::RGUploadRing ring(&device, kRingFrames, kRingBytesPerFrame);
        RenderGraph::RGFrame frame(&device, &persistent, &transient, &ring);
        frame.BeginFrame(0);
        rs.BeginWorldDrawFrame();
        for (const ExtractedLight& light : lights)
            rs.SubmitLight(0u, light);
        rs.FinalizeWorldLights(0u);
        const std::vector<ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
        const std::vector<ViewTargetsRG> targets{ViewTargetsRG{viewId, {}, {}, {}}};
        instance.Declare(frame, targets, views);

        const PipelineFrameResources* resources = instance.FrameResourcesFor(&frame);
        const PipelineBufferBindingRG* viewParams = FindViewBuffer(resources, viewId, "ViewParams");
        const PipelineBufferBindingRG* lightBuffer = FindViewBuffer(resources, viewId, "LightBuffer");
        if (viewParams == nullptr || lightBuffer == nullptr)
        {
            ADD_FAILURE() << "the upload nodes published no ViewParams or LightBuffer";
        }
        else
        {
            const uint32_t lightCount = static_cast<uint32_t>(lights.size());
            const std::array<uint32_t, 6> clusters{1u, 1u, 1u, kLightsPerCluster, 0u, lightCount};
            std::vector<uint32_t> indices(lightCount);
            for (uint32_t i = 0; i < lightCount; ++i)
                indices[i] = i;
            const std::array<uint32_t, 4> clusterParams{kOneTile, kOneTile, 1u, kLightsPerCluster};
            const std::array<float, kLightUboFloats> noDirectionalLight{};
            std::vector<Mathematics::Vector4> zeros(samples.size() * 2);

            DescriptorSetLayoutDesc layout{};
            for (uint32_t binding : {0u, 1u, 4u, 10u, 11u})
                layout.bindings.push_back({binding, DescriptorType::StorageBuffer, 1, kShaderStageCompute});
            for (uint32_t binding : {2u, 5u, 6u, 7u})
                layout.bindings.push_back({binding, DescriptorType::UniformBuffer, 1, kShaderStageCompute});
            ComputePipelineDesc pipelineDesc{};
            pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(spv));
            pipelineDesc.DescriptorSetLayouts.push_back(device.InternDescriptorSetLayout(layout));
            const auto pipeline = device.GetOrCreateComputePipeline(device.InternComputePipeline(pipelineDesc));
            DescriptorSetDesc setDesc{};
            setDesc.layout = layout;
            set = device.CreateDescriptorSet(setDesc);

            const BufferHandle indexBuffer = HostBuffer(device, owned, indices.data(), indices.size() * sizeof(uint32_t));
            const BufferHandle clusterBuffer = HostBuffer(device, owned, clusters.data(), sizeof(clusters));
            const BufferHandle paramsBuffer = HostBuffer(device, owned, clusterParams.data(), sizeof(clusterParams));
            const BufferHandle lightUbo = HostBuffer(device, owned, noDirectionalLight.data(), sizeof(noDirectionalLight));
            const BufferHandle cameraUbo = HostBuffer(device, owned, &passCamera, sizeof(CameraData));
            const BufferHandle sampleBuffer =
                HostBuffer(device, owned, samples.data(), samples.size() * sizeof(ProbeSample));
            const BufferHandle resultBuffer =
                HostBuffer(device, owned, zeros.data(), zeros.size() * sizeof(Mathematics::Vector4));
            device.UpdateStorageBufferBinding(set, 0, indexBuffer, 0, indices.size() * sizeof(uint32_t));
            device.UpdateStorageBufferBinding(set, 1, clusterBuffer, 0, sizeof(clusters));
            device.UpdateBufferBinding(set, 2, paramsBuffer, 0, sizeof(clusterParams));
            device.UpdateBufferBinding(set, 5, cameraUbo, 0, sizeof(CameraData));
            device.UpdateStorageBufferBinding(set, 4, lightBuffer->Buffer, lightBuffer->Offset,
                                              lightBuffer->Size);
            device.UpdateBufferBinding(set, 6, lightUbo, 0, sizeof(noDirectionalLight));
            device.UpdateBufferBinding(set, 7, viewParams->Buffer, viewParams->Offset,
                                       viewParams->Size);
            device.UpdateStorageBufferBinding(set, 10, sampleBuffer, 0, samples.size() * sizeof(ProbeSample));
            device.UpdateStorageBufferBinding(set, 11, resultBuffer, 0, zeros.size() * sizeof(Mathematics::Vector4));

            auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
            commands->Begin();
            commands->SetPipeline(pipeline);
            commands->BindDescriptorSet(0, set, pipeline);
            commands->Dispatch(static_cast<uint32_t>(samples.size()), 1, 1);
            commands->End();
            device.ExecuteCommandLists({commands.get()});
            device.WaitForIdle();

            run.emplace();
            const auto* mappedParams = static_cast<const uint8_t*>(device.MapBuffer(viewParams->Buffer));
            std::memcpy(&run->ViewParams, mappedParams + viewParams->Offset, sizeof(ViewParamsUBO));
            device.UnmapBuffer(viewParams->Buffer);
            const auto* mapped = static_cast<const Mathematics::Vector4*>(device.MapBuffer(resultBuffer));
            for (size_t i = 0; i < samples.size(); ++i)
                run->Results.push_back({mapped[2 * i], mapped[2 * i + 1]});
            device.UnmapBuffer(resultBuffer);
        }
        frame.Execute();
        device.WaitForIdle();
    }
    if (set.IsValid())
        device.DestroyDescriptorSet(set);
    for (const BufferHandle buffer : owned)
        device.DestroyBuffer(buffer);
    rs.Shutdown();
    return run;
}

float Luminance(const Mathematics::Vector4& c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

// The largest difference in luminance over the brightest luminance of the near scene.
double LargestRelativeDifference(const ProbeRun& nearRun, const ProbeRun& farRun)
{
    float brightest = 0.0f;
    for (const auto& lit : nearRun.Results)
        brightest = std::max(brightest, Luminance(lit[0]));
    double largest = 0.0;
    for (size_t i = 0; i < nearRun.Results.size(); ++i)
        largest = std::max(largest, static_cast<double>(std::abs(Luminance(farRun.Results[i][0]) - Luminance(nearRun.Results[i][0]))));
    return brightest > 0.0f ? largest / brightest : 1.0;
}

// The spot shadow data the production shadow declaration uploads for a view, if it declared one.
std::optional<SpotShadowDataGPU> DeclareSpotShadow(IDevice& device, const CameraData& camera,
                                                   const std::vector<ExtractedLight>& lights)
{
    RenderServices rs;
    if (!rs.Initialize(&device))
        return std::nullopt;
    const ViewId viewId = AddView(rs, camera);
    auto& feature = rs.EnsureFeature<ShadowMapRenderFeature>();
    CascadedShadowConfig config{};
    config.NumCascades = 1;
    config.Resolution = kPunctualResolution;
    if (!feature.Initialize(&device, config))
        return std::nullopt;
    // A caster, so the view declares its shadow passes.
    auto emit = rs.RegisterDepthEmit(
        [viewId](DepthEmitContext& emit, DepthPassType passType)
        {
            if (emit.ViewId == viewId && passType == DepthPassType::SpotShadow)
                emit.Services->EmitDepthCommand(emit.ViewId, passType, DrawCommand{});
        });

    std::optional<SpotShadowDataGPU> upload;
    {
        RenderGraph::RGResourcePool persistent(&device);
        RenderGraph::RGTransientPool transient(&device);
        RenderGraph::RGUploadRing ring(&device, kRingFrames, kRingBytesPerFrame);
        RenderGraph::RGFrame frame(&device, &persistent, &transient, &ring);
        frame.BeginFrame(0);
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        for (const ExtractedLight& light : lights)
            rs.SubmitLight(0u, light);
        rs.FinalizeWorldLights(0u);
        const FeatureDeclareContext declareContext =
            rs.MakeFeatureDeclareContext(frame, viewId, config.NumCascades, kPunctualResolution, nullptr, {});
        if (feature.DeclareSpotPass(frame, rs, declareContext).IsValid())
        {
            const auto inputs = rs.GetLocalShadowInputsRG(frame, viewId);
            if (inputs.SpotData.IsValid())
            {
                upload.emplace();
                const auto* mapped = static_cast<const uint8_t*>(device.MapBuffer(inputs.SpotData));
                std::memcpy(&*upload, mapped + inputs.SpotOffset, sizeof(SpotShadowDataGPU));
                device.UnmapBuffer(inputs.SpotData);
            }
        }
        frame.Execute();
        device.WaitForIdle();
    }
    emit.Reset();
    rs.Shutdown();
    return upload;
}

// The spot shadow map coordinates of a receiver, as GE_SampleSpotShadow computes them in fp32 from the
// matrix and the position it is given: x, y = shadow UV, z = depth.
Vector3 SpotShadowCoordinates(const float viewProjection[16], const Vector3& position)
{
    float clip[4];
    for (int row = 0; row < 4; ++row)
        clip[row] = viewProjection[0 * 4 + row] * position.x + viewProjection[1 * 4 + row] * position.y +
                    viewProjection[2 * 4 + row] * position.z + viewProjection[3 * 4 + row];
    return Vector3{clip[0] / clip[3] * 0.5f + 0.5f, 0.5f - clip[1] / clip[3] * 0.5f, clip[2] / clip[3]};
}

// The spot shadow map coordinates of an exact world position, in double: the light's view rotation and
// projection applied to the position minus the light's exact position.
Mathematics::Vector3F64 ExactShadowCoordinates(const SpotShadowFrameInfo& info, const WorldPosition& light,
                                               const WorldPosition& point)
{
    const Mathematics::Vector3F64 offset{point.X - light.X, point.Y - light.Y, point.Z - light.Z};
    double view[4] = {0.0, 0.0, 0.0, 1.0};
    for (int row = 0; row < 3; ++row)
        view[row] = static_cast<double>(info.lightView[0][row]) * offset.X +
                    static_cast<double>(info.lightView[1][row]) * offset.Y +
                    static_cast<double>(info.lightView[2][row]) * offset.Z;
    double clip[4] = {};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            clip[row] += static_cast<double>(info.lightProj[column][row]) * view[column];
    return Mathematics::Vector3F64{clip[0] / clip[3] * 0.5 + 0.5, 0.5 - clip[1] / clip[3] * 0.5, clip[2] / clip[3]};
}

// The largest difference in meters between a shadow's light position and the light's exact position, both
// relative to the camera's render origin: the frame the shadow matrices and the light buffer share.
double LightPositionError(const CameraData& camera, const WorldPosition& shadowPosition, const WorldPosition& light)
{
    const WorldPosition origin = WorldPosition::FromSector(CameraRenderOriginSector(camera), Vector3{});
    const Vector3 relative = ViewRelativePoint(camera, shadowPosition);
    return std::max({std::abs(relative.x - (light.X - origin.X)), std::abs(relative.y - (light.Y - origin.Y)),
                     std::abs(relative.z - (light.Z - origin.Z))});
}

} // namespace

// A point and a spot light 141 km from the origin light a wall as the same lights light the same wall
// near the origin. The light vectors subtract positions relative to the view's sector.
TEST(LocalLightRelativeFrame, LightsAtAFarSectorLightAWallAsNearTheOrigin)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    const CameraData nearCamera = CameraAt(kNearBase + kEye);
    const CameraData farCamera = CameraAt(kFarBase + kEye);
    const auto nearRun = RunProbe(*device, kLightingProbe, nearCamera, nearCamera, SceneLights(kNearBase, 0),
                                  ProbeSamples(WallPoints(kNearBase)));
    const auto farRun = RunProbe(*device, kLightingProbe, farCamera, farCamera, SceneLights(kFarBase, 0),
                                 ProbeSamples(WallPoints(kFarBase)));
    ASSERT_TRUE(nearRun.has_value() && farRun.has_value());
    ASSERT_EQ(farRun->ViewParams.ge_renderOrigin[0], 98.0f) << "the far view is in sector (98, 0, 98)";

    float brightest = 0.0f;
    for (const auto& lit : nearRun->Results)
        brightest = std::max(brightest, Luminance(lit[0]));
    ASSERT_GT(brightest, 0.0f) << "the lights reach the near wall";
    const double difference = LargestRelativeDifference(*nearRun, *farRun);
    RecordProperty("largestRelativeLuminanceDifference", std::to_string(difference));
    EXPECT_LT(difference, 1e-3) << "the far wall is lit differently from the near wall";
}

// In sector (0,0,0) a lit fragment computes the same bits from the relative inputs (vPosRel,
// ge_cameraPosRel, ge_viewRel) as from the absolute ones (vPosWS, ge_cameraPosWS, ge_view), and the
// light buffer holds each position rounded to fp32.
TEST(LocalLightRelativeFrame, InSectorZeroTheRelativeAndAbsoluteInputsLightTheSameBits)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    const CameraData camera = CameraAt(kNearBase + kEye);
    const auto run = RunProbe(*device, kLightingProbe, camera, camera, SceneLights(kNearBase, 0),
                              ProbeSamples(WallPoints(kNearBase)));
    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->ViewParams.ge_renderOrigin[0], 0.0f);
    size_t differing = 0;
    for (const auto& lit : run->Results)
        differing += std::memcmp(&lit[0], &lit[1], sizeof(Mathematics::Vector4)) != 0 ? 1 : 0;
    EXPECT_EQ(differing, 0u) << "samples whose relative and absolute lighting differ";
}

// A spot shadow receiver 141 km from the origin lands where the light's view and projection, in double on the
// exact receiver, put it. The uploaded matrix takes positions relative to the view's sector, the frame of vPosRel.
TEST(LocalLightRelativeFrame, ASpotShadowReceiverAtAFarSectorProjectsAsTheExactProjection)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    const CameraData camera = CameraAt(kFarBase + kEye);
    const std::vector<ExtractedLight> lights = SceneLights(kFarBase, 1);
    const auto upload = DeclareSpotShadow(*device, camera, lights);
    ASSERT_TRUE(upload.has_value()) << "the spot shadow declared";
    const SpotShadowFrameInfo info = BuildSpotShadowFrameInfo(lights);
    ASSERT_TRUE(info.valid);

    double largestUv = 0.0;
    double largestDepth = 0.0;
    double largestWorldMatrixUv = 0.0;
    for (const WorldPosition& point : WallPoints(kFarBase))
    {
        const Mathematics::Vector3F64 exact = ExactShadowCoordinates(info, lights[1].positionWS, point);
        const Vector3 relative = SpotShadowCoordinates(upload->spotShadowVP, ViewRelativePoint(camera, point));
        const Vector3 world = SpotShadowCoordinates(info.lightVP.Data(), point - WorldPosition{});
        largestUv = std::max({largestUv, std::abs(relative.x - exact.X), std::abs(relative.y - exact.Y)});
        largestDepth = std::max(largestDepth, std::abs(relative.z - exact.Z));
        largestWorldMatrixUv = std::max({largestWorldMatrixUv, std::abs(world.x - exact.X), std::abs(world.y - exact.Y)});
    }
    RecordProperty("largestUvDifference", std::to_string(largestUv));
    RecordProperty("largestDepthDifference", std::to_string(largestDepth));
    RecordProperty("worldSpaceMatrixLargestUvDifference", std::to_string(largestWorldMatrixUv));
    // A quarter of a texel of a 1024 map.
    EXPECT_LT(largestUv, 2.5e-4) << "the far receiver lands on another shadow map texel";
    EXPECT_LT(largestDepth, 1e-5) << "the far receiver's shadow depth moved";
}

// In a shadow pass the camera that draws (Cam) is the light's, here one sector ahead of the view, and ViewParams
// hold the receiver view. A material reads the receiver view's depth (Distance Fade, Proximity Fade).
TEST(LocalLightRelativeFrame, AShadowPassInAnotherSectorReadsTheReceiverViewDepth)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    const CameraData view = CameraAt(kFarBase + kEye);
    const CameraData passCamera = CameraAt(kFarBase + kEye + kOneSectorAhead);
    ASSERT_EQ(CameraRenderOriginSector(passCamera).z, CameraRenderOriginSector(view).z + 1)
        << "the pass camera is one sector ahead of the view";
    const std::vector<WorldPosition> points = WallPoints(kFarBase);
    const auto run =
        RunProbe(*device, kViewDepthProbe, view, passCamera, SceneLights(kFarBase, 0), ProbeSamples(points));
    ASSERT_TRUE(run.has_value());

    double largest = 0.0;
    double largestWorldPosition = 0.0;
    for (size_t i = 0; i < points.size(); ++i)
    {
        const double exact = Tests::ExactViewDepth(view, points[i]);
        largest = std::max(largest, std::abs(run->Results[i][0][0] - exact));
        largestWorldPosition = std::max(largestWorldPosition, std::abs(run->Results[i][1][0] - exact));
    }
    RecordProperty("largestDepthDifference", std::to_string(largest));
    RecordProperty("worldPositionLargestDepthDifference", std::to_string(largestWorldPosition));
    // A millimeter: the fp32 rounding of a position a sector from its origin is 0.12 mm.
    EXPECT_LT(largest, 1e-3) << "the shadow pass reads another view depth than the receiver view's";
}

// The spot, area and point shadows of lights 141 km and 6,371 km from the origin hold each light's position at the
// precision of the light buffer: within 0.1 mm of the exact position in the frame of the view's render origin.
TEST(LocalLightRelativeFrame, TheShadowFrameInfosHoldTheLightPositionsAtFullPrecision)
{
    constexpr uint32_t kPacked = 0;
    for (const WorldPosition& base : {kFarBase, kEarthRadiusBase})
    {
        const CameraData camera = CameraAt(base + kEye);
        std::vector<ExtractedLight> lights = SceneLights(base, 1);
        ExtractedLight area{};
        area.type = Components::LightType::Area;
        area.positionWS = base + Vector3{0.3f, 2.6f, 1.1f};
        area.directionWS[2] = 1.0f;
        area.range = 6.0f;
        area.intensity = 20.0f;
        area.areaWidth = 1.0f;
        area.areaHeight = 0.5f;
        area.castsShadows = 1;
        area.SortId = 3;
        lights.push_back(area);

        const SpotShadowFrameInfo spot = BuildSpotShadowFrameInfo(lights);
        const AreaShadowFrameInfo areaInfo = BuildAreaShadowFrameInfo(lights);
        PointShadowFrameInfo point{};
        PopulatePointShadowGeometry(point, lights[0].positionWS, lights[0].range, kPunctualShadowTierInherit, kPacked,
                                    nullptr);
        ASSERT_TRUE(spot.valid && areaInfo.valid && point.valid);
        const double spotError = LightPositionError(camera, spot.position, lights[1].positionWS);
        const double areaError = LightPositionError(camera, areaInfo.position, area.positionWS);
        const double pointError = LightPositionError(camera, point.position, lights[0].positionWS);
        RecordProperty(base.X > kFarBase.X ? "earthRadiusLargestError" : "farLargestError",
                       std::to_string(std::max({spotError, areaError, pointError})));
        EXPECT_LT(spotError, 1e-4) << "spot light at x = " << base.X;
        EXPECT_LT(areaError, 1e-4) << "area light at x = " << base.X;
        EXPECT_LT(pointError, 1e-4) << "point light at x = " << base.X;
    }
}
