// The sun glare's caster term (sun_glare_casters.comp) and the ocean's god rays (ocean_god_ray_shadow_probe.comp)
// read the sun's cascades 141 km from the origin as near it: the cascade matrices take positions relative to the
// view's render origin. Part of RenderPipelineDeclareTests.
#include "Engine/Rendering/Pipeline/Nodes/EngineNodeTypes.h"
#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/SunGlareRenderFeature.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector3F64.h"
#include "Mathematics/Vector4.h"
#include "Mathematics/WorldPosition.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
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
constexpr float kRange = 200.0f;
// The near scene is in sector (0,0,0). The far scene is 141 km from the origin, in sector (98, 0, 98).
const WorldPosition kNearBase{200.0, 0.0, 200.0};
const WorldPosition kFarBase{100000.0, 0.0, 100000.0};
const Vector3 kEye{0.0f, 1.7f, -2.0f};
// The direction the sun's light travels: down, a little to the side.
const Vector3 kLightDirection = Vector3{0.1f, -1.0f, 0.15f}.Normalize();
// The sun glare looks steeply down, so the view's first cascade holds the eye.
const Vector3 kGlareForward = Vector3{0.0f, -1.0f, 0.15f}.Normalize();
const Vector3 kGodRayForward = Vector3{0.0f, -0.1f, 1.0f}.Normalize();
// A caster plane across the sun's beam, this far from the eye toward the sun (meters).
constexpr double kCasterDistance = 2.0;
// The angular radius of the disc the glare's caster term samples (radians).
constexpr float kDiscRadius = 0.005f;
constexpr uint32_t kShadowSize = 64;   // the test's cascade map, texels
constexpr uint32_t kCascadeLayers = 4; // the cascade array's layers
constexpr size_t kCastersBufferBytes = 16;
constexpr uint32_t kRingFrames = 2;
constexpr uint64_t kRingBytesPerFrame = 256 * 1024;
constexpr const char* kGodRayProbe = "ocean_god_ray_shadow_probe.comp.spv";

// One sample of the god-ray probe: an fp32 world position and its linear view depth.
struct GodRaySample
{
    Mathematics::Vector4 PositionWS{};
    Mathematics::Vector4 ViewDepth{};
};
static_assert(sizeof(GodRaySample) == 32, "GodRaySample mirrors the probe's std430 struct");

// A camera at `eye` that looks along `forward`, built as an editor builds it: the double eye through SetCameraEye.
CameraData CameraAt(const WorldPosition& eye, const Vector3& forward)
{
    const Vector3 eyeF = eye - WorldPosition{};
    CameraData camera{};
    camera.view = Mathematics::MakeLookAtLH(eyeF, eyeF + forward, Vector3{0.0f, 1.0f, 0.0f});
    camera.proj = Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0472f, 1.0f, kNear, kRange);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, eye);
    return camera;
}

// The exact position of `point` relative to the origin of the camera's render origin sector, in double.
Mathematics::Vector3F64 ExactRelative(const CameraData& camera, const WorldPosition& point)
{
    const WorldPosition origin = WorldPosition::FromSector(CameraRenderOriginSector(camera), Vector3{});
    return Mathematics::Vector3F64{point.X - origin.X, point.Y - origin.Y, point.Z - origin.Z};
}

// The NDC depth of an exact relative position through a cascade matrix, in double.
double NdcDepth(const Mathematics::Matrix4x4& viewProjection, const Mathematics::Vector3F64& position)
{
    const double z = viewProjection[0][2] * position.X + viewProjection[1][2] * position.Y +
                     viewProjection[2][2] * position.Z + viewProjection[3][2];
    const double w = viewProjection[0][3] * position.X + viewProjection[1][3] * position.Y +
                     viewProjection[2][3] * position.Z + viewProjection[3][3];
    return z / w;
}

// One view of `camera`, declared as a frame declares it: the view parameters the ViewParamsUpload node wrote and
// the ShadowData block ShadowMapRenderFeature::BuildShadowDataGPU fills from the view's cascade fit.
class ReaderView
{
  public:
    ReaderView(IDevice& device, const CameraData& camera) : m_Device(device)
    {
        if (!m_Services.Initialize(&device))
            return;
        const CameraId cameraId = m_Services.Views().AllocateCamera("CascadeReaderCam");
        m_Services.Views().SetCameraData(cameraId, camera, {kNear, kRange});
        m_ViewId = m_Services.Views().AllocateView("CascadeReaderView", cameraId);
        m_Services.Views().SetViewRenderLayerMask(m_ViewId, RenderLayerMask::Default());
        m_Services.Views().SetViewTargets(m_ViewId, 0, 0, 0, ViewClearConfig{});

        Nodes::RegisterEngineNodeTypes(m_Registry);
        RenderPipelineBlueprint bp;
        bp.pipelineName = "CascadeReaderProbe";
        RenderPipelineBlueprint::Pass pass;
        pass.id = "ViewParamsUpload";
        pass.type = "ViewParamsUpload";
        pass.enabled = true;
        pass.perView = true;
        pass.passJson = R"({"id":"ViewParamsUpload","type":"ViewParamsUpload"})";
        bp.passes.push_back(pass);
        m_Instance.emplace(m_Services, m_Registry);
        m_Instance->SetBlueprint(bp);

        m_Persistent.emplace(&device);
        m_Transient.emplace(&device);
        m_Ring.emplace(&device, kRingFrames, kRingBytesPerFrame);
        m_Frame.emplace(&device, &*m_Persistent, &*m_Transient, &*m_Ring);
        m_Frame->BeginFrame(0);
        const std::vector<ViewDesc> views(m_Services.Views().GetViews().begin(), m_Services.Views().GetViews().end());
        const std::vector<ViewTargetsRG> targets{ViewTargetsRG{m_ViewId, {}, {}, {}}};
        m_Instance->Declare(*m_Frame, targets, views);
        if (const PipelineFrameResources* resources = m_Instance->FrameResourcesFor(&*m_Frame))
        {
            const auto found = resources->Buffers.find({m_ViewId, "ViewParams"});
            if (found != resources->Buffers.end())
                m_ViewParams = found->second;
        }

        ShadowMapRenderFeature shadows;
        m_Cascades = shadows.ComputeCascades(camera, kNear, kRange, kLightDirection);
        shadows.BuildShadowDataGPU(m_Cascades, m_ViewId, m_Services, *m_Frame, 0u, *m_ShadowData);
    }

    ~ReaderView()
    {
        if (m_Frame)
            m_Frame->Execute();
        m_Device.WaitForIdle();
        m_Frame.reset();
        m_Ring.reset();
        m_Transient.reset();
        m_Persistent.reset();
        m_Instance.reset();
        m_Services.Shutdown();
    }

    ReaderView(const ReaderView&) = delete;
    ReaderView& operator=(const ReaderView&) = delete;

    bool IsDeclared() const { return m_ViewParams.has_value(); }
    const PipelineBufferBindingRG& ViewParams() const { return *m_ViewParams; }
    const CascadeFrameData& Cascades() const { return m_Cascades; }
    const ShadowDataGPU& ShadowData() const { return *m_ShadowData; }

  private:
    IDevice& m_Device;
    RenderServices m_Services;
    RenderPipelineNodeRegistry m_Registry;
    std::optional<RenderPipelineInstance> m_Instance;
    std::optional<RenderGraph::RGResourcePool> m_Persistent;
    std::optional<RenderGraph::RGTransientPool> m_Transient;
    std::optional<RenderGraph::RGUploadRing> m_Ring;
    std::optional<RenderGraph::RGFrame> m_Frame;
    ViewId m_ViewId{};
    std::optional<PipelineBufferBindingRG> m_ViewParams;
    CascadeFrameData m_Cascades{};
    // Held apart: its 16-byte alignment would pad this class.
    std::unique_ptr<ShadowDataGPU> m_ShadowData = std::make_unique<ShadowDataGPU>();
};

// The device objects of one probe run, destroyed with it.
class DeviceObjects
{
  public:
    explicit DeviceObjects(IDevice& device) : m_Device(device) {}
    ~DeviceObjects()
    {
        m_Device.WaitForIdle();
        for (const DescriptorSetHandle set : m_Sets)
            m_Device.DestroyDescriptorSet(set);
        for (const TextureHandle texture : m_Textures)
            m_Device.DestroyTexture(texture);
        for (const BufferHandle buffer : m_Buffers)
            m_Device.DestroyBuffer(buffer);
        for (const SamplerHandle sampler : m_Samplers)
            m_Device.DestroySampler(sampler);
    }

    BufferHandle HostBuffer(const void* data, size_t bytes)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(BufferUsage::Uniform | BufferUsage::Storage);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        desc.debugName = "CascadeReaderProbe.Buffer";
        const BufferHandle buffer = m_Device.CreateBuffer(desc);
        if (buffer.IsValid())
        {
            m_Device.UpdateBuffer(buffer, 0, bytes, data);
            m_Buffers.push_back(buffer);
        }
        return buffer;
    }

    // A cascade array whose layer c holds the depth `layerDepths[c]` in every texel.
    TextureHandle CascadeArray(const std::array<float, kCascadeLayers>& layerDepths)
    {
        TextureDesc desc{};
        desc.width = desc.height = kShadowSize;
        desc.arrayLayers = kCascadeLayers;
        desc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
        desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::DepthStencil);
        desc.debugName = "CascadeReaderProbe.Cascades";
        const TextureHandle texture = m_Device.CreateTexture(desc);
        if (!texture.IsValid())
            return texture;
        m_Textures.push_back(texture);
        auto commands = m_Device.CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        for (uint32_t layer = 0; layer < kCascadeLayers; ++layer)
        {
            RenderPassDesc pass{};
            pass.depthTarget = texture;
            pass.clearDepth = true;
            pass.clearDepthValue = layerDepths[layer];
            pass.useDepthView = true;
            pass.depthViewDesc.aspect = TextureAspect::Depth;
            pass.depthViewDesc.baseLayer = layer;
            pass.depthViewDesc.layerCount = 1;
            pass.depthViewDesc.levelCount = 1;
            commands->BeginRenderPass(pass);
            commands->EndRenderPass();
        }
        commands->Barrier(
            ResourceBarrier::CreateTextureBarrier(texture, ResourceState::DepthWrite, ResourceState::ShaderResource));
        commands->End();
        m_Device.ExecuteCommandLists({commands.get()});
        m_Device.WaitForIdle();
        return texture;
    }

    SamplerHandle CompareSampler()
    {
        const SamplerHandle sampler = m_Device.CreateSampler(SamplerDesc::ShadowComparePCF("CascadeReaderProbe.Compare"));
        if (sampler.IsValid())
            m_Samplers.push_back(sampler);
        return sampler;
    }

    DescriptorSetHandle Set(const DescriptorSetLayoutDesc& layout)
    {
        DescriptorSetDesc desc{};
        desc.layout = layout;
        const DescriptorSetHandle set = m_Device.CreateDescriptorSet(desc);
        if (set.IsValid())
            m_Sets.push_back(set);
        return set;
    }

  private:
    IDevice& m_Device;
    std::vector<DescriptorSetHandle> m_Sets;
    std::vector<TextureHandle> m_Textures;
    std::vector<BufferHandle> m_Buffers;
    std::vector<SamplerHandle> m_Samplers;
};

void Submit(IDevice& device, CommandList& commands)
{
    commands.End();
    device.ExecuteCommandLists({&commands});
    device.WaitForIdle();
}

// The glare's caster term for `camera` through the production pipeline, push constants and ShadowData block,
// against a caster plane kCasterDistance from the exact eye toward the sun. Nullopt when a piece is missing.
std::optional<float> RunGlareCasters(IDevice& device, SunGlareRenderFeature& glare, const CameraData& camera)
{
    const ReaderView view(device, camera);
    const Vector3 sunDirection = kLightDirection * -1.0f;
    const Mathematics::Vector3F64 eye = ExactRelative(camera, CameraEye(camera));
    const Mathematics::Vector3F64 caster{eye.X + kCasterDistance * sunDirection.x,
                                         eye.Y + kCasterDistance * sunDirection.y,
                                         eye.Z + kCasterDistance * sunDirection.z};
    std::array<float, kCascadeLayers> casterDepths{};
    for (uint32_t c = 0; c < kCascadeLayers; ++c)
        casterDepths[c] = static_cast<float>(std::clamp(NdcDepth(view.Cascades().LightVPRel[c], caster), 0.0, 1.0));

    DeviceObjects objects(device);
    const TextureHandle shadowArray = objects.CascadeArray(casterDepths);
    const SamplerHandle compare = objects.CompareSampler();
    const Mathematics::Vector4 zeros{};
    const BufferHandle shadowData = objects.HostBuffer(&view.ShadowData(), sizeof(ShadowDataGPU));
    const BufferHandle result = objects.HostBuffer(&zeros, kCastersBufferBytes);
    const DescriptorSetHandle set = objects.Set(glare.GetCastersLayout());
    const auto pipeline = device.GetOrCreateComputePipeline(glare.GetCastersPipelineId());
    if (!shadowArray.IsValid() || !compare.IsValid() || !set.IsValid() || !pipeline.IsValid())
        return std::nullopt;
    device.UpdateCombinedImageSamplerBinding(set, glare.GetCastersShadowArrayBinding(), shadowArray, compare);
    device.UpdateBufferBinding(set, glare.GetCastersShadowDataBinding(), shadowData, 0, sizeof(ShadowDataGPU));
    device.UpdateStorageBufferBinding(set, glare.GetCastersOutputBinding(), result, 0, kCastersBufferBytes);

    const SunGlareCastersPushConstants pc =
        SunGlareRenderFeature::MakeCastersPushConstants(camera, sunDirection, kDiscRadius);
    auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->SetPipeline(pipeline);
    commands->BindDescriptorSet(0, set, pipeline);
    commands->SetPushConstants(pc);
    commands->Dispatch(1, 1, 1);
    Submit(device, *commands);

    const auto* mapped = static_cast<const float*>(device.MapBuffer(result));
    const float visibility = mapped[0];
    device.UnmapBuffer(result);
    return visibility;
}

// Points under the eye's view, 4 m to 20 m ahead and 1 m below the ground: world positions, exact in double.
std::vector<WorldPosition> WaterPoints(const WorldPosition& base)
{
    std::vector<WorldPosition> points;
    for (int row = 0; row < 5; ++row)
        for (int column = 0; column < 5; ++column)
            points.push_back(WorldPosition{base.X - 2.0 + column, base.Y - 1.0, base.Z + 4.0 + 4.0 * row});
    return points;
}

// The god-ray visibility of each point for `camera`, through the view parameters and the ShadowData block the
// production upload wrote, and a cascade array whose every texel holds `storedDepth`.
std::optional<std::vector<float>> RunGodRays(IDevice& device, const CameraData& camera,
                                             const std::vector<WorldPosition>& points, float storedDepth)
{
    const std::string spvPath = std::string(ENGINE_TEST_BUILD_DIR) + "/Shaders/" + kGodRayProbe;
    std::ifstream file(spvPath, std::ios::binary);
    std::vector<uint8_t> spv{std::istreambuf_iterator<char>(file), {}};
    if (spv.empty())
    {
        ADD_FAILURE() << "missing probe shader " << spvPath;
        return std::nullopt;
    }
    std::vector<GodRaySample> samples;
    for (const WorldPosition& point : points)
    {
        GodRaySample sample{};
        const Vector3 position = point - WorldPosition{};
        sample.PositionWS = Mathematics::Vector4(position.x, position.y, position.z, 1.0f);
        sample.ViewDepth.x = static_cast<float>(Tests::ExactViewDepth(camera, point));
        samples.push_back(sample);
    }

    const ReaderView view(device, camera);
    if (!view.IsDeclared())
    {
        ADD_FAILURE() << "the upload node published no ViewParams";
        return std::nullopt;
    }
    DescriptorSetLayoutDesc layout{};
    layout.bindings.push_back({8u, DescriptorType::UniformBuffer, 1, kShaderStageCompute});
    layout.bindings.push_back({9u, DescriptorType::CombinedImageSampler, 1, kShaderStageCompute});
    layout.bindings.push_back({10u, DescriptorType::StorageBuffer, 1, kShaderStageCompute});
    layout.bindings.push_back({11u, DescriptorType::StorageBuffer, 1, kShaderStageCompute});
    layout.bindings.push_back({20u, DescriptorType::UniformBuffer, 1, kShaderStageCompute});
    ComputePipelineDesc pipelineDesc{};
    pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(spv));
    pipelineDesc.DescriptorSetLayouts.push_back(device.InternDescriptorSetLayout(layout));
    const auto pipeline = device.GetOrCreateComputePipeline(device.InternComputePipeline(pipelineDesc));

    DeviceObjects objects(device);
    std::array<float, kCascadeLayers> depths{};
    depths.fill(storedDepth);
    const std::vector<Mathematics::Vector4> zeros(samples.size());
    const size_t sampleBytes = samples.size() * sizeof(GodRaySample);
    const size_t resultBytes = zeros.size() * sizeof(Mathematics::Vector4);
    const TextureHandle shadowArray = objects.CascadeArray(depths);
    const SamplerHandle compare = objects.CompareSampler();
    const BufferHandle shadowData = objects.HostBuffer(&view.ShadowData(), sizeof(ShadowDataGPU));
    const BufferHandle sampleBuffer = objects.HostBuffer(samples.data(), sampleBytes);
    const BufferHandle resultBuffer = objects.HostBuffer(zeros.data(), resultBytes);
    const DescriptorSetHandle set = objects.Set(layout);
    if (!pipeline.IsValid() || !shadowArray.IsValid() || !compare.IsValid() || !set.IsValid())
        return std::nullopt;
    const PipelineBufferBindingRG& viewParams = view.ViewParams();
    device.UpdateBufferBinding(set, 8, shadowData, 0, sizeof(ShadowDataGPU));
    device.UpdateCombinedImageSamplerBinding(set, 9, shadowArray, compare);
    device.UpdateStorageBufferBinding(set, 10, sampleBuffer, 0, sampleBytes);
    device.UpdateStorageBufferBinding(set, 11, resultBuffer, 0, resultBytes);
    device.UpdateBufferBinding(set, 20, viewParams.Buffer, viewParams.Offset, viewParams.Size);
    auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->SetPipeline(pipeline);
    commands->BindDescriptorSet(0, set, pipeline);
    commands->Dispatch(static_cast<uint32_t>(samples.size()), 1, 1);
    Submit(device, *commands);

    std::vector<float> visibility;
    const auto* mapped = static_cast<const Mathematics::Vector4*>(device.MapBuffer(resultBuffer));
    for (size_t i = 0; i < samples.size(); ++i)
        visibility.push_back(mapped[i].x);
    device.UnmapBuffer(resultBuffer);
    return visibility;
}

} // namespace

// A caster across the sun's beam hides the sun glare 141 km from the origin as it does near the origin. The caster
// term's eye is relative to the view's render origin, the frame of the cascade matrices.
TEST(CascadeReaderRelativeFrame, ACasterHidesTheSunGlareAtAFarSectorAsNearTheOrigin)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    SunGlareRenderFeature glare;
    ASSERT_TRUE(glare.Initialize(device));
    ASSERT_TRUE(glare.GetCastersPipelineId().IsValid()) << "sun_glare_casters.shaderpkg did not load";

    const CameraData nearCamera = CameraAt(kNearBase + kEye, kGlareForward);
    const CameraData farCamera = CameraAt(kFarBase + kEye, kGlareForward);
    ASSERT_EQ(CameraRenderOriginSector(farCamera).x, 98) << "the far view is in sector (98, 0, 98)";
    const std::optional<float> nearVisibility = RunGlareCasters(*device, glare, nearCamera);
    const std::optional<float> farVisibility = RunGlareCasters(*device, glare, farCamera);
    ASSERT_TRUE(nearVisibility.has_value() && farVisibility.has_value());
    RecordProperty("nearVisibility", std::to_string(*nearVisibility));
    RecordProperty("farVisibility", std::to_string(*farVisibility));
    ASSERT_LT(*nearVisibility, 0.01f) << "the caster does not hide the sun near the origin: the instrument is void";
    EXPECT_NEAR(*farVisibility, *nearVisibility, 0.01f) << "the far caster does not hide the sun";

    const Mathematics::Vector3F64 exactEye = ExactRelative(farCamera, CameraEye(farCamera));
    const SunGlareCastersPushConstants pc =
        SunGlareRenderFeature::MakeCastersPushConstants(farCamera, Vector3{0.0f, 1.0f, 0.0f}, kDiscRadius);
    const double eyeError = std::max({std::abs(pc.Eye.x - exactEye.X), std::abs(pc.Eye.y - exactEye.Y),
                                      std::abs(pc.Eye.z - exactEye.Z)});
    RecordProperty("eyeError", std::to_string(eyeError));
    EXPECT_LT(eyeError, 1e-4) << "the caster term's eye is not the eye relative to the view's render origin";
}

// In sector (0,0,0) the caster term's eye is the camera's eye bit for bit.
TEST(CascadeReaderRelativeFrame, InSectorZeroTheSunGlareEyeIsTheCameraEye)
{
    const CameraData camera = CameraAt(kNearBase + kEye, kGlareForward);
    const SunGlareCastersPushConstants pc =
        SunGlareRenderFeature::MakeCastersPushConstants(camera, Vector3{0.0f, 1.0f, 0.0f}, kDiscRadius);
    EXPECT_EQ(std::memcmp(&pc.Eye.x, &camera.cameraPos.x, 3 * sizeof(float)), 0);
}

// Water points under a sunlit sky 141 km from the origin are in the cascades' shadow as near the origin. The god
// rays read the cascades with the position relative to the view's render origin.
TEST(CascadeReaderRelativeFrame, GodRaysAtAFarSectorReadTheCascadesAsNearTheOrigin)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    constexpr float kOccluderEverywhere = 1.0f; // the reverse-Z depth nearest the light
    constexpr float kEmpty = 0.0f;
    const CameraData nearCamera = CameraAt(kNearBase + kEye, kGodRayForward);
    const CameraData farCamera = CameraAt(kFarBase + kEye, kGodRayForward);
    const auto nearShadowed = RunGodRays(*device, nearCamera, WaterPoints(kNearBase), kOccluderEverywhere);
    const auto nearEmpty = RunGodRays(*device, nearCamera, WaterPoints(kNearBase), kEmpty);
    const auto farShadowed = RunGodRays(*device, farCamera, WaterPoints(kFarBase), kOccluderEverywhere);
    ASSERT_TRUE(nearShadowed.has_value() && nearEmpty.has_value() && farShadowed.has_value());

    float largestNearShadowed = 0.0f;
    float smallestNearEmpty = 1.0f;
    float largestFarShadowed = 0.0f;
    for (size_t i = 0; i < nearShadowed->size(); ++i)
    {
        largestNearShadowed = std::max(largestNearShadowed, (*nearShadowed)[i]);
        smallestNearEmpty = std::min(smallestNearEmpty, (*nearEmpty)[i]);
        largestFarShadowed = std::max(largestFarShadowed, (*farShadowed)[i]);
    }
    RecordProperty("largestFarVisibility", std::to_string(largestFarShadowed));
    ASSERT_EQ(smallestNearEmpty, 1.0f) << "an empty cascade shadows the water: the instrument is void";
    ASSERT_EQ(largestNearShadowed, 0.0f) << "the cascades do not hold the near water: the instrument is void";
    EXPECT_EQ(largestFarShadowed, 0.0f) << "the far water reads the cascades away from its own position";
}
