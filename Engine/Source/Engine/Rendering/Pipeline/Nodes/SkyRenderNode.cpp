#include "Engine/Rendering/Pipeline/Nodes/SkyRenderNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/ViewExposureCompensation.h"
#include "Engine/Rendering/Pipeline/ViewExposureHistory.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Pipeline/ForwardFogKeywords.h"
#include "Engine/Rendering/SkyBackdrop.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Sky/SkyRenderer.h"
#include "Rendering/Sky/SkySettings.h"
#include "Rendering/Sky/SkyViewLutAltitude.h"

#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

static constexpr float kDefaultHalfFovDeg = 30.0f;
static constexpr float kDefaultAspect = 16.0f / 9.0f;
static constexpr uint32_t kFallbackViewportWidth = 1280;
static constexpr uint32_t kFallbackViewportHeight = 720;
static constexpr uint32_t kSkyComputeWorkgroupSize = 8;
static constexpr float kProjectionEpsilon = 1e-6f;
// sky_render.frag: skyboxParams.y >= 0 selects HDRI path (intensity may be 0 for black sky).
// Sentinel selects procedural/analytic sky so zero intensity is not confused with "no HDRI".
static constexpr float kSkyHdriProceduralSentinel = -1.0f;

static constexpr uint32_t DivCeil(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

// The falling stars in the sky at once at most: each slot flies one in every kFallingStarSlots x the
// interval (sky_meteor.glsl), and at the shortest interval a slot's window still holds its longest flight.
static constexpr uint32_t kFallingStarSlots = 4;

static uint32_t FallingStarSlots(const Rendering::SkySettings& settings)
{
    return settings.fallingStarsEnabled ? kFallingStarSlots : 0u;
}

// The star light name always resolves, so the tonemap never declines for want of it: black when the
// view draws no stars (no sky, a gradient or HDRI backdrop, daylight).
static void PublishNoStarLight(ViewDeclare& d)
{
    d.PublishTexture(Names::Res::SkyStarLight,
                     d.Frame.ImportExternalTexture("Sky_StarLight_Absent",
                                                   d.Services.Textures().GetDefaultBlackTexture(),
                                                   Rendering::ResourceState::ShaderResource));
}

// The star light target: R16F luminance at the scene colour's extent and sample count. The texture the
// tonemap samples is pool-imported per view, so it has its physical image at declaration, when the
// fullscreen stage builds its bindings (a transient texture gets one only at execution). A
// multisampled view draws into a transient multisampled target and resolves into it.
struct StarLightTargets
{
    RenderGraph::RGTexture Draw;
    RenderGraph::RGTexture Resolve;
};

static StarLightTargets CreateStarLightTargets(ViewDeclare& d, RenderGraph::RGTexture sceneColor)
{
    const RenderGraph::RGResourceDesc& colorDesc = d.Frame.Graph().ResourceDesc(sceneColor.Id);
    if (colorDesc.Width == 0 || colorDesc.Height == 0)
        return {};
    Rendering::TextureDesc td{};
    td.width = colorDesc.Width;
    td.height = colorDesc.Height;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(TextureFormat::R16_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) | static_cast<uint32_t>(TextureUsage::ShaderResource);
    const std::string name = "Sky_StarLight.View" + std::to_string(static_cast<uint32_t>(d.View.id));
    td.debugName = name.c_str();
    StarLightTargets targets{};
    targets.Resolve = d.Frame.ImportPersistentTexture(name.c_str(), td);
    targets.Draw = targets.Resolve;
    if (targets.Resolve.IsValid() && colorDesc.SampleCount > 1)
    {
        td.sampleCount = colorDesc.SampleCount;
        td.debugName = "Sky_StarLight_Multisampled";
        targets.Draw = d.Frame.CreateTexture("Sky_StarLight_Multisampled", td);
    }
    return targets;
}

static void FillAtmosphereFromSettings(const Rendering::SkySettings& settings, Rendering::AtmosphereParametersGPU& atmo)
{
    Rendering::SkyRenderer::FillDefaultAtmosphere(atmo);
    atmo.groundAlbedo[0] = settings.groundAlbedo[0];
    atmo.groundAlbedo[1] = settings.groundAlbedo[1];
    atmo.groundAlbedo[2] = settings.groundAlbedo[2];
    atmo.groundBrightness = settings.groundBrightness;
    atmo.groundNightColor[0] = settings.groundNightColor[0];
    atmo.groundNightColor[1] = settings.groundNightColor[1];
    atmo.groundNightColor[2] = settings.groundNightColor[2];
    atmo.groundNightColorStd140Pad = 0.0f;
    atmo.groundHorizonColor[0] = settings.groundHorizonColor[0];
    atmo.groundHorizonColor[1] = settings.groundHorizonColor[1];
    atmo.groundHorizonColor[2] = settings.groundHorizonColor[2];
    atmo.groundHorizonColorStd140Pad = 0.0f;
    atmo.groundHorizonNightColor[0] = settings.groundHorizonNightColor[0];
    atmo.groundHorizonNightColor[1] = settings.groundHorizonNightColor[1];
    atmo.groundHorizonNightColor[2] = settings.groundHorizonNightColor[2];
    atmo.nightSkyHorizonColor[0] = settings.nightSkyHorizonColor[0];
    atmo.nightSkyHorizonColor[1] = settings.nightSkyHorizonColor[1];
    atmo.nightSkyHorizonColor[2] = settings.nightSkyHorizonColor[2];
    atmo.nightSkyHorizonColorStd140Pad = 0.0f;
    atmo.groundHorizonDayCosWidth = settings.groundHorizonCosWidth;
    atmo.groundHorizonNightCosWidth = settings.groundHorizonNightCosWidth;
    atmo.belowHorizonBlendSharpness = settings.belowHorizonBlendSharpness;
    atmo.belowHorizonDarkness = settings.belowHorizonDarkness;
    atmo.belowHorizonDarkColor[0] = settings.belowHorizonDarkColor[0];
    atmo.belowHorizonDarkColor[1] = settings.belowHorizonDarkColor[1];
    atmo.belowHorizonDarkColor[2] = settings.belowHorizonDarkColor[2];
    atmo.belowHorizonMode = settings.belowHorizonMode;
    atmo.groundHazeStrength = settings.groundHazeStrength;
}

// The camera's world-space position and basis, in world units and unit directions.
struct CameraVectors
{
    Mathematics::Vector3 Position;
    Mathematics::Vector3 Forward;
    Mathematics::Vector3 Up;
    Mathematics::Vector3 Right;
};

// Extracts camera basis vectors and position from a column-major LH view matrix
// (glm::lookAtLH). The upper-left 3x3 rows are right/up/forward directly in LH;
// no sign flip is needed on forward.
static CameraVectors ExtractCameraVectors(const Rendering::CameraData& cam)
{
    const float* v = cam.view;
    CameraVectors out;
    out.Right   = Mathematics::Vector3(v[0], v[4], v[8]);
    out.Up      = Mathematics::Vector3(v[1], v[5], v[9]);
    out.Forward = Mathematics::Vector3(v[2], v[6], v[10]);

    out.Position = Mathematics::Vector3(-(v[0] * v[12] + v[1] * v[13] + v[2]  * v[14]),
                                        -(v[4] * v[12] + v[5] * v[13] + v[6]  * v[14]),
                                        -(v[8] * v[12] + v[9] * v[13] + v[10] * v[14]));
    return out;
}

// Perspective LH projections leave proj[15] (m[3][3]) at 0 because clip.w = view.z.
// Orthographic projections preserve w = 1, so proj[15] == 1 distinguishes the two.
static bool IsOrthographicProjection(const Rendering::CameraData& cam)
{
    return std::abs(cam.proj[15] - 1.0f) < kProjectionEpsilon;
}

// Returns 0.0 for orthographic projections as a sentinel; the sky/star shaders
// treat tanHalfFovY <= 0 as "render the 2D backdrop path" (vertical gradient
// for the sky, NDC-mapped star positions) instead of the perspective ray fan.
static float ExtractTanHalfFovY(const Rendering::CameraData& cam)
{
    if (IsOrthographicProjection(cam))
        return 0.0f;
    float p5 = cam.proj[5];
    if (std::abs(p5) < kProjectionEpsilon)
        return std::tan(kDefaultHalfFovDeg * std::numbers::pi_v<float> / 180.0f);
    return 1.0f / std::abs(p5);
}

static float ExtractAspect(const Rendering::CameraData& cam)
{
    float p0 = cam.proj[0];
    float p5 = cam.proj[5];
    if (std::abs(p0) < kProjectionEpsilon)
        return kDefaultAspect;
    return std::abs(p5) / std::abs(p0);
}

// Populates SkyUBOGPU fields from settings. Camera and viewport fields
// are left at zero and must be filled by the caller for passes that need them.
static void FillSkyUBOFromSettings(const Rendering::SkySettings& settings, SkyUBOGPU& ubo)
{
    ubo.exposureEV = settings.exposureEV;
    ubo.pad0 = settings.sunSize2D;
    ubo.pad1 = settings.moonSize2D;
    ubo.skyPan2D = settings.skyPan2D;
    ubo.sunDirection[0] = settings.scatteringSunDir[0];
    ubo.sunDirection[1] = settings.scatteringSunDir[1];
    ubo.sunDirection[2] = settings.scatteringSunDir[2];
    // The one place the drawn sun size is decided: the physical radius scaled by the stylistic
    // multiplier, so sky_render.frag draws whatever it is given. Radius and brightness stay one
    // decision — the shader derives the disc's radiance from the solid angle the drawn radius
    // covers, so an oversized radius does not brighten the sun, it flattens it. A hidden sun
    // still uploads exactly 0: the shader treats that as "no disc", and sun_disc.glsl floors the
    // radius it divides by so the zero cannot reach SceneColor as a NaN.
    ubo.sunAngularRadius = settings.showSunDisk ? kSunAngularRadiusRad * settings.sunSize : 0.0f;
    ubo.moonDirection[0] = settings.moonDirWS[0];
    ubo.moonDirection[1] = settings.moonDirWS[1];
    ubo.moonDirection[2] = settings.moonDirWS[2];
    ubo.moonIntensity = settings.showMoonDisk ? settings.moonIntensity : 0.0f;
    ubo.moonAngularRadius = settings.moonAngularRadius;
    ubo.skyRotateAroundZenithRadians = 0.0f;
    ubo.moonUboPad2 = settings.moonExposureEV;
    ubo.sunColor[0] = settings.primarySunColor[0];
    ubo.sunColor[1] = settings.primarySunColor[1];
    ubo.sunColor[2] = settings.primarySunColor[2];
    ubo.sunIntensity = settings.primarySunIntensity;
    ubo.timeOfDayHours = settings.timeOfDayHours;

    ubo.nightSkyBlend = settings.nightSkyBlend;
    ubo.skyTimeSeconds = settings.skyTimeSeconds;

    ubo.starModel[0] = settings.starLimitingMagnitude;
    ubo.starModel[1] = settings.starHighlightCompression;
    ubo.starModel[2] = settings.starTwinkleAmount;
    ubo.starModel[3] = settings.starTwinkleRate;
    ubo.starShape[0] = settings.starCoreScale;
    ubo.starShape[1] = settings.starSpikes;
    ubo.starShape[2] = settings.starSpikeLength;
    ubo.starShape[3] = static_cast<float>(settings.starShape);

    ubo.skyboxParams[0] = settings.environmentTexture.IsValid() ? settings.skyboxRotationRadians : 0.0f;
    ubo.skyboxParams[1] = settings.environmentTexture.IsValid() ? settings.hdriIntensity : kSkyHdriProceduralSentinel;
    ubo.skyboxParams[2] = settings.starGlow;
    // The star observer: 1 = the eye (glare, the faint fade), 0 = a camera.
    ubo.skyboxParams[3] = settings.starObserver == 0 ? 1.0f : 0.0f;

    ubo.fallingStarParams[0] = static_cast<float>(FallingStarSlots(settings));
    ubo.fallingStarParams[1] = settings.fallingStarIntervalSeconds;

    // Gradient sky: .rgb already scene-linear (intensity folded in by the system). The mode
    // sentinel rides gradientSkyTop.w: an HDRI backdrop wins (it is a separate component path),
    // so gradient is only active when there is no environment texture.
    const bool gradientMode = ClassifySkyBackdrop(settings) == SkyBackdropKind::Gradient;
    for (int i = 0; i < 3; ++i)
    {
        ubo.gradientSkyTop[i]     = settings.gradientSkyTopColor[i];
        ubo.gradientSkyHorizon[i] = settings.gradientSkyHorizonColor[i];
        ubo.gradientSkyBottom[i]  = settings.gradientSkyBottomColor[i];
    }
    ubo.gradientSkyTop[3]     = gradientMode ? 1.0f : -1.0f;
    ubo.gradientSkyHorizon[3] = 0.0f;
    ubo.gradientSkyBottom[3]  = 0.0f;
}

// FNV-1a 64-bit. Stable across runs; sufficient quality for change detection.
static void MixDigest(uint64_t& h, const void* data, size_t bytes)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
}

// Digest of the settings the multiscatter and sky-view LUT bakes read. Used by the dirty gates to
// skip the dispatches when nothing a LUT depends on has changed. Saves ~0.026 ms / frame on static
// skies (the common editor-idle case).
//
// IMPORTANT: only hash the fields the LUT shaders actually read. SkyEnvironmentSystem updates
// settings.skyTimeSeconds and several other fields every frame (for the stars' twinkle and the falling
// stars, animated in the star billboard pass), but the LUT shaders don't sample those; hashing them would force
// redundant LUT recomputes every frame and defeat the gate.
//
// LUT shader inputs (see sky_view_lut.comp main()):
//   - uSky.sunDirectionWS   (settings.scatteringSunDir)
//   - uSky.sunColor         (settings.sunSourceColor, the LUT pass's own fill)
//   - uSky.sunIntensity     (settings.sunSourceIntensity, the LUT pass's own fill)
//   - uSky.moonDirectionWS  (settings.moonDirWS)
//   - uSky.moonIntensity    (settings.moonIntensity, 0 while the moon is hidden)
//   - all uAtmos.* fields   (filled from settings via FillAtmosphereFromSettings)
//   - uSky.cameraPositionWS (sky-view only: the altitude band's height, see SkyViewLutDigest)
static uint64_t ComputeSkyLutSettingsDigest(const Rendering::SkySettings& settings)
{
    Rendering::AtmosphereParametersGPU atmo{};
    FillAtmosphereFromSettings(settings, atmo);

    uint64_t h = 14695981039346656037ull;
    // Atmosphere block in its entirety.
    MixDigest(h, &atmo, sizeof(atmo));
    // Only the sun and moon SkyUBO fields the LUT shaders actually consume.
    MixDigest(h, settings.scatteringSunDir, sizeof(settings.scatteringSunDir));
    MixDigest(h, settings.sunSourceColor, sizeof(settings.sunSourceColor));
    MixDigest(h, &settings.sunSourceIntensity, sizeof(settings.sunSourceIntensity));
    // The moon reaches the kernel only while it lights the sky, so a hidden moon's direction does not
    // re-bake the LUT.
    const float moonIntensity = settings.showMoonDisk ? settings.moonIntensity : 0.0f;
    MixDigest(h, &moonIntensity, sizeof(moonIntensity));
    if (moonIntensity > 0.0f)
        MixDigest(h, settings.moonDirWS, sizeof(settings.moonDirWS));
    return h;
}

// The sky-view LUT's digest: the settings digest and the camera's altitude band. The band is the
// only camera input; a camera moving inside its band leaves the digest unchanged.
static uint64_t SkyViewLutDigest(uint64_t settingsDigest, uint32_t altitudeBand)
{
    uint64_t h = settingsDigest;
    MixDigest(h, &altitudeBand, sizeof(altitudeBand));
    return h;
}

// The altitude band of the camera the sky-view LUT is baked for. One LUT serves every view, so one
// camera decides: the main camera over every world (ViewRegistry::FindMainCameraData), the camera
// the sun state's horizon dip reads. Band 0 (the ground) when there is none.
static uint32_t SkyViewLutCameraBand(const Engine::Renderer::RenderServices& rs, float planetRadius)
{
    const auto* camData = rs.Views().FindMainCameraData(0);
    if (!camData)
        return 0u;
    const CameraVectors camera = ExtractCameraVectors(*camData);
    return Rendering::SkyViewLutAltitudeBand(Rendering::AltitudeAbovePlanetMeters(camera.Position.v, planetRadius));
}

void SkyRenderNode::EnsureDescriptorSetDescs(const Rendering::SkyRenderer& renderer)
{
    // Cached once: the execute callbacks reuse these, never heap-allocating a
    // bindings vector per frame. Binding indices come from the renderer's
    // reflected slots (single source of truth with the pipeline layouts); the
    // descriptor types stay explicit.
    if (m_DescriptorSetDescsBuilt)
        return;
    m_DescriptorSetDescsBuilt = true;
    const Rendering::SkyRenderer::SkyBindingSlots& slots = renderer.GetBindingSlots();

    using Rendering::DescriptorType;
    constexpr uint32_t kCompute = Rendering::kShaderStageCompute;

    // The storage texel format has to reach the layout: WebGPU bind group
    // layouts declare it and reject a view whose format differs, while
    // Vulkan and Metal carry it on the view and ignore the layout. Left at
    // 0 the backend assumes RGBA8Unorm and every LUT dispatch is refused.
    // The transmittance and multiscatter LUTs hold ratios (a transmittance, a radiance per unit
    // sun illuminance), which binary16 keeps to 3 significant digits; the sky-view LUT holds
    // absolute radiance and takes SkyRenderer::SkyViewLutFormat.
    constexpr uint32_t kLutStorageFormat =
        static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);

    m_TransmittanceDSD.layout.bindings = {
        {slots.Transmittance.TransLUT, DescriptorType::StorageImage, 1, kCompute},
        {slots.Transmittance.Atmos, DescriptorType::UniformBuffer, 1, kCompute}
    };
        m_TransmittanceDSD.layout.bindings[0].storageTexelFormat = kLutStorageFormat;
    m_TransmittanceDSD.layout.debugName = "Sky.Transmittance";
    m_TransmittanceDSD.transient = true;
    m_TransmittanceDSD.debugName = "Sky.Transmittance.Set0";

    m_MultiscatterDSD.layout.bindings = {
        {slots.Multiscatter.MultiscatterLUT, DescriptorType::StorageImage, 1, kCompute},
        {slots.Multiscatter.TransmittanceLUT, DescriptorType::CombinedImageSampler, 1, kCompute},
        {slots.Multiscatter.Atmos, DescriptorType::UniformBuffer, 1, kCompute}
    };
        m_MultiscatterDSD.layout.bindings[0].storageTexelFormat = kLutStorageFormat;
    // The LUT chain filters rgba16f LUTs with linear samplers from compute.
    for (auto& b : m_MultiscatterDSD.layout.bindings)
        if (b.type == DescriptorType::CombinedImageSampler)
            b.imageFilterableFloat = true;
    m_MultiscatterDSD.layout.debugName = "Sky.MultiscatterLUT";
    m_MultiscatterDSD.transient = true;
    m_MultiscatterDSD.debugName = "Sky.MultiscatterLUT.Set0";

    m_SkyViewLutDSD.layout.bindings = {
        {slots.SkyViewLut.SkyViewLUT, DescriptorType::StorageImage, 1, kCompute},
        {slots.SkyViewLut.TransmittanceLUT, DescriptorType::CombinedImageSampler, 1, kCompute},
        {slots.SkyViewLut.Atmos, DescriptorType::UniformBuffer, 1, kCompute},
        {slots.SkyViewLut.Sky, DescriptorType::UniformBuffer, 1, kCompute},
        {slots.SkyViewLut.MultiscatterLUT, DescriptorType::CombinedImageSampler, 1, kCompute}
    };
        m_SkyViewLutDSD.layout.bindings[0].storageTexelFormat = static_cast<uint32_t>(
            Rendering::SkyRenderer::SkyViewLutFormat(renderer.GetDevice()->PreferredShaderSource()));
    for (auto& b : m_SkyViewLutDSD.layout.bindings)
        if (b.type == DescriptorType::CombinedImageSampler)
            b.imageFilterableFloat = true;
    m_SkyViewLutDSD.layout.debugName = "Sky.SkyViewLUT";
    m_SkyViewLutDSD.transient = true;
    m_SkyViewLutDSD.debugName = "Sky.SkyViewLUT.Set0";

}

bool SkyRenderNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);

    try
    {
        const auto j = nlohmann::json::parse(m_Json);
        if (j.contains("output") && j["output"].is_string())
            m_OutputRef = j["output"].get<std::string>();
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("SkyRenderNode JSON parse error: ") + e.what();
        return false;
    }

    return true;
}

void SkyRenderNode::RebuildPassNames(const std::string& pipelineName)
{
    m_CachedPipelineName = pipelineName;
    std::string prefix = "Pipeline." + pipelineName + "." + m_Id + ".";
    m_TransmittancePassName = prefix + "Transmittance";
    m_MultiscatterPassName = prefix + "MultiscatterLUT";
    m_SkyViewLutPassName = prefix + "SkyViewLUT";
}

Rendering::BufferHandle SkyRenderNode::GetOrCreatePerViewSkyUBO(
    Rendering::IDevice* dev, uint32_t viewKey, uint32_t slot)
{
    auto& entry = m_PerViewUBOs[viewKey];
    if (!entry.skyUBO[slot].IsValid())
    {
        Rendering::BufferDesc bd{};
        bd.size = sizeof(SkyUBOGPU);
        bd.usage = static_cast<uint32_t>(Rendering::BufferUsage::Uniform);
        bd.memoryUsage = Rendering::BufferMemoryUsage::Upload;
        bd.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
        bd.debugName = "Sky_PerView_SkyUBO";
        entry.skyUBO[slot] = dev->CreateBuffer(bd);
    }
    return entry.skyUBO[slot];
}

SkyRenderNode::~SkyRenderNode()
{
    if (m_Device)
    {
        for (auto& [viewKey, perView] : m_PerViewUBOs)
        {
            for (auto& buf : perView.skyUBO)
            {
                if (buf.IsValid())
                    m_Device->DestroyBuffer(buf);
                buf = {};
            }
        }
    }
    m_PerViewUBOs.clear();
}

void SkyRenderNode::Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx)
{
    auto& rs = ctx.Services;
    auto* device = rs.GetDevice();
    if (!device)
        return;
    m_Device = device;

    auto& sky = rs.EnsureFeature<SkyRenderFeature>();
    if (!sky.IsInitialized() && !sky.Initialize(device))
    {
        LOG_ERROR("SkyRenderNode: failed to initialize SkyRenderFeature");
        return;
    }

    // Inactive sky declares NOTHING — the Disable/Reenable/Invalidate dances
    // die here: the world arm re-decides Clear-vs-Load per frame from the
    // backdrop flag, which DeclareForView simply doesn't set.
    if (!sky.HasActiveSettings())
    {
        if (!m_WarnedNoComponent)
        {
            LOG_WARNING("SkyRender pass is active but no SkyEnvironment component found in scene.");
            m_WarnedNoComponent = true;
        }
        return;
    }
    m_WarnedNoComponent = false;

    const auto& settings = sky.GetSettings();
    if (ClassifySkyBackdrop(settings) != SkyBackdropKind::Atmosphere)
        return; // backdrop-only: the per-view draw samples the env texture / gradient directly

    auto& renderer = sky.GetRenderer();
    EnsureDescriptorSetDescs(renderer);
    const auto& pipelineName = instance.GetBlueprint().pipelineName;
    if (pipelineName != m_CachedPipelineName)
        RebuildPassNames(pipelineName);

    // LUT imports at their HONEST state: never-computed content is
    // discardable (Common → Undefined first transition); computed LUTs are
    // sampled-persisted across frames. Dedup-by-handle makes the per-view
    // draw's re-imports land on these same ids.
    const uint64_t digest = ComputeSkyLutSettingsDigest(settings);
    Rendering::AtmosphereParametersGPU lutAtmosphere{};
    FillAtmosphereFromSettings(settings, lutAtmosphere);
    const uint32_t altitudeBand = SkyViewLutCameraBand(rs, lutAtmosphere.planetRadius);
    const bool transCurrent = renderer.IsTransmittanceComputed();
    const bool multiscatterCurrent = renderer.IsMultiscatterCurrentFor(digest);
    const bool skyViewCurrent = renderer.IsSkyViewLutCurrentFor(SkyViewLutDigest(digest, altitudeBand));
    const RenderGraph::RGTexture trans = ctx.Frame.ImportExternalTexture(
        "Sky_Transmittance_LUT", renderer.GetTransmittanceLutTexture(),
        transCurrent ? Rendering::ResourceState::ShaderResource
                     : Rendering::ResourceState::Common);
    const RenderGraph::RGTexture multiscatter = ctx.Frame.ImportExternalTexture(
        "Sky_Multiscatter_LUT", renderer.GetMultiscatterLutTexture(),
        multiscatterCurrent ? Rendering::ResourceState::ShaderResource
                            : Rendering::ResourceState::Common);
    // The format makes the LUT readable by the render graph's readback (capture_resource).
    const RenderGraph::RGTexture skyView = ctx.Frame.ImportExternalTexture(
        "Sky_View_LUT", renderer.GetSkyViewLutTexture(),
        skyViewCurrent ? Rendering::ResourceState::ShaderResource
                       : Rendering::ResourceState::Common,
        Rendering::SkyRenderer::SkyViewLutFormat(renderer.GetDevice()->PreferredShaderSource()));

    // Dirty gates AT DECLARATION (the activation predicates' immediate-mode
    // form). Settings digests, plus the sky-view LUT's altitude band; never
    // extend to raw camera or depth inputs, which change every frame.
    auto* rendererPtr = &renderer;
    auto* skyPtr = &sky;
    if (!transCurrent)
    {
        ctx.Frame.AddComputePass(
            m_TransmittancePassName.c_str(), Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p) { p.Write(trans, RenderGraph::RGTextureWrite::Storage); },
            [this, rendererPtr](RenderGraph::RGContext& c)
            {
                auto* cl = c.Cmd;
                auto* dev = c.GetDevice();
                if (!cl || !dev)
                    return;

                Rendering::AtmosphereParametersGPU atmo{};
                Rendering::SkyRenderer::FillDefaultAtmosphere(atmo);

                const uint32_t slot = rendererPtr->GetFrameSlot(dev);
                Rendering::BufferHandle buf = rendererPtr->GetTransmittanceAtmoUBO(slot);
                void* data = dev->MapBuffer(buf);
                if (!data)
                    return;
                std::memcpy(data, &atmo, sizeof(atmo));
                dev->UnmapBuffer(buf);

                auto pipe = c.GetOrCreatePipelineVariant(rendererPtr->EnsureTransmittanceId(*dev));
                if (!pipe.IsValid())
                    return; // no latch: the dirty gate re-declares next frame
                const auto& slots = rendererPtr->GetBindingSlots().Transmittance;
                auto ds = dev->CreateDescriptorSet(m_TransmittanceDSD);
                dev->UpdateStorageImageBinding(ds, slots.TransLUT,
                                               rendererPtr->GetTransmittanceLutTexture());
                dev->UpdateBufferBinding(ds, slots.Atmos, buf, 0, sizeof(atmo));

                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                const auto& cfg = rendererPtr->GetConfig();
                cl->Dispatch(DivCeil(cfg.transmittanceLutWidth, kSkyComputeWorkgroupSize),
                             DivCeil(cfg.transmittanceLutHeight, kSkyComputeWorkgroupSize), 1);
                rendererPtr->MarkTransmittanceComputed();
            });
    }
    if (!multiscatterCurrent)
    {
        ctx.Frame.AddComputePass(
            m_MultiscatterPassName.c_str(), Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            {
                // Read the transmittance LUT (Sampled) and write the multiscatter
                // LUT (Storage). These access edges make RG order this pass after
                // transmittance and before sky-view (which reads multiscatter).
                p.Read(trans, RenderGraph::RGTextureRead::Sampled);
                p.Write(multiscatter, RenderGraph::RGTextureWrite::Storage);
            },
            [this, rendererPtr, skyPtr](RenderGraph::RGContext& c)
            {
                auto* cl = c.Cmd;
                auto* dev = c.GetDevice();
                if (!cl || !dev)
                    return;

                Rendering::AtmosphereParametersGPU atmo{};
                FillAtmosphereFromSettings(skyPtr->GetSettings(), atmo);

                const uint32_t slot = rendererPtr->GetFrameSlot(dev);
                Rendering::BufferHandle bAtmo = rendererPtr->GetMultiscatterAtmoUBO(slot);
                void* pAtmo = dev->MapBuffer(bAtmo);
                if (!pAtmo)
                    return;
                std::memcpy(pAtmo, &atmo, sizeof(atmo));
                dev->UnmapBuffer(bAtmo);

                auto pipe = c.GetOrCreatePipelineVariant(rendererPtr->EnsureMultiscatterId(*dev));
                if (!pipe.IsValid())
                    return; // no latch: the dirty gate re-declares next frame
                const auto& slots = rendererPtr->GetBindingSlots().Multiscatter;
                auto ds = dev->CreateDescriptorSet(m_MultiscatterDSD);
                dev->UpdateStorageImageBinding(ds, slots.MultiscatterLUT,
                                               rendererPtr->GetMultiscatterLutTexture());
                dev->UpdateCombinedImageSamplerBinding(ds, slots.TransmittanceLUT,
                                                       rendererPtr->GetTransmittanceLutTexture(),
                                                       rendererPtr->GetLinearSampler());
                dev->UpdateBufferBinding(ds, slots.Atmos, bAtmo, 0, sizeof(atmo));

                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                const auto& cfg = rendererPtr->GetConfig();
                cl->Dispatch(DivCeil(cfg.multiscatterLutSize, kSkyComputeWorkgroupSize),
                             DivCeil(cfg.multiscatterLutSize, kSkyComputeWorkgroupSize), 1);
                rendererPtr->MarkMultiscatterComputed(
                    ComputeSkyLutSettingsDigest(skyPtr->GetSettings()));
            });
    }
    if (!skyViewCurrent)
    {
        ctx.Frame.AddComputePass(
            m_SkyViewLutPassName.c_str(), Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            {
                p.Read(trans, RenderGraph::RGTextureRead::Sampled);
                p.Read(multiscatter, RenderGraph::RGTextureRead::Sampled);
                p.Write(skyView, RenderGraph::RGTextureWrite::Storage);
            },
            [this, rendererPtr, skyPtr, altitudeBand](RenderGraph::RGContext& c)
            {
                auto* cl = c.Cmd;
                auto* dev = c.GetDevice();
                if (!cl || !dev)
                    return;

                const auto& s = skyPtr->GetSettings();
                SkyUBOGPU skyUbo{};
                FillSkyUBOFromSettings(s, skyUbo);
                // The LUT is baked for a camera on the planet's +Y axis at the band's altitude.
                skyUbo.cameraPosition[1] = Rendering::SkyViewLutBandAltitude(altitudeBand);
                // Its sun term is the sun's own source; the moon is its second term
                // (moonDirection / moonIntensity), so neither carries the handover.
                for (int channel = 0; channel < 3; ++channel)
                    skyUbo.sunColor[channel] = s.sunSourceColor[channel];
                skyUbo.sunIntensity = s.sunSourceIntensity;
                Rendering::AtmosphereParametersGPU atmo{};
                FillAtmosphereFromSettings(s, atmo);

                const uint32_t slot = rendererPtr->GetFrameSlot(dev);
                Rendering::BufferHandle bSky = rendererPtr->GetSkyViewLutSkyUBO(slot);
                void* pSky = dev->MapBuffer(bSky);
                if (!pSky)
                    return;
                std::memcpy(pSky, &skyUbo, sizeof(skyUbo));
                dev->UnmapBuffer(bSky);
                Rendering::BufferHandle bAtmo = rendererPtr->GetSkyViewLutAtmoUBO(slot);
                void* pAtmo = dev->MapBuffer(bAtmo);
                if (!pAtmo)
                    return;
                std::memcpy(pAtmo, &atmo, sizeof(atmo));
                dev->UnmapBuffer(bAtmo);

                auto pipe = c.GetOrCreatePipelineVariant(rendererPtr->EnsureSkyViewLutId(*dev));
                if (!pipe.IsValid())
                    return; // no latch: the dirty gate re-declares next frame
                const auto& slots = rendererPtr->GetBindingSlots().SkyViewLut;
                auto ds = dev->CreateDescriptorSet(m_SkyViewLutDSD);
                dev->UpdateStorageImageBinding(ds, slots.SkyViewLUT,
                                               rendererPtr->GetSkyViewLutTexture());
                dev->UpdateCombinedImageSamplerBinding(ds, slots.TransmittanceLUT,
                                                       rendererPtr->GetTransmittanceLutTexture(),
                                                       rendererPtr->GetLinearSampler());
                dev->UpdateBufferBinding(ds, slots.Atmos, bAtmo, 0, sizeof(atmo));
                dev->UpdateBufferBinding(ds, slots.Sky, bSky, 0, sizeof(skyUbo));
                dev->UpdateCombinedImageSamplerBinding(ds, slots.MultiscatterLUT,
                                                       rendererPtr->GetMultiscatterLutTexture(),
                                                       rendererPtr->GetLinearSampler());

                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                const auto& cfg = rendererPtr->GetConfig();
                cl->Dispatch(DivCeil(cfg.skyViewLutWidth, kSkyComputeWorkgroupSize),
                             DivCeil(cfg.skyViewLutHeight, kSkyComputeWorkgroupSize), 1);
                rendererPtr->MarkSkyViewLutComputed(
                    SkyViewLutDigest(ComputeSkyLutSettingsDigest(skyPtr->GetSettings()), altitudeBand));
            });
    }
}

SkyRenderNode::SkyHeightFog SkyRenderNode::ResolveSkyHeightFog(ViewDeclare& d, const Rendering::SkyRenderer& renderer)
{
    SkyHeightFog fog{};
    if (HeightFogPassKeyword(d) == Rendering::MaterialKeyword::None || !renderer.HasHeightFogPipelines())
        return fog;
    fog.ViewParams = d.ResolveBuffer(Names::Res::ViewParams);
    fog.Params = d.ResolveBuffer(Names::Res::HeightFogParams);
    if (!fog.ViewParams.IsValid() || !fog.Params.IsValid())
        return fog;
    fog.BackdropSampler = d.Services.Textures().GetSampler(Rendering::SamplerPreset::LinearRepeat);
    fog.Active = true;
    return fog;
}

void SkyRenderNode::DeclareForView(ViewDeclare& d)
{
    auto& rs = d.Services;
    auto* skyFeat = rs.GetFeature<SkyRenderFeature>();
    if (!skyFeat || !skyFeat->IsInitialized() || !skyFeat->HasActiveSettings())
    {
        // The name always resolves, so a pass reading the backdrop never declines for want of a
        // sky; its readers learn there is none from their own sky settings check.
        d.PublishTexture(Names::Res::SkyBackdrop,
                         d.Frame.ImportExternalTexture("Sky_Backdrop_Absent",
                                                       rs.Textures().GetDefaultBlackTexture(),
                                                       Rendering::ResourceState::ShaderResource));
        PublishNoStarLight(d);
        m_ClearColorBackdrop.Declare(d);
        return;
    }
    auto& sky = *skyFeat;
    auto& renderer = sky.GetRenderer();
    EnsureDescriptorSetDescs(renderer);
    const Rendering::ViewId viewId = d.View.id;
    const uint32_t viewKey = static_cast<uint32_t>(viewId);

    const auto& settings = sky.GetSettings();
    const Rendering::TextureHandle skySourceTex = settings.environmentTexture.IsValid()
                                                      ? settings.environmentTexture
                                                      : renderer.GetSkyViewLutTexture();
    // Dedup-by-handle lands these on the frame-scope Declare's ids when the
    // LUT path is active; the HDRI env texture imports fresh as sampled.
    const RenderGraph::RGTexture skySource = d.Frame.ImportExternalTexture(
        "Sky_Backdrop_Source", skySourceTex, Rendering::ResourceState::ShaderResource);
    d.PublishTexture(Names::Res::SkyBackdrop, skySource);

    const RenderGraph::RGTexture out = d.ResolveTexture(m_OutputRef);
    // The sky and the stars store their radiance at the frame's pre-exposure P.
    const PipelineBufferBindingRG exposure = ResolveViewExposureHistory(d);
    // The height fog's sky fill, drawn here for a view whose height fog is active: the sky is what
    // the far plane shows, and every surface drawn over it takes its own fog in its own shader.
    const SkyHeightFog fog = ResolveSkyHeightFog(d, renderer);
    const uint32_t starCount = renderer.GetStarInstanceCount(settings.starLimitingMagnitude);
    // The falling stars are the first instances of the stars' draw (sky_stars_billboard.vert).
    const uint32_t instanceCount = FallingStarSlots(settings) + starCount;
    const bool starsUp = out.IsValid() && exposure.IsValid() &&
                         ClassifySkyBackdrop(settings) == SkyBackdropKind::Atmosphere &&
                         settings.nightSkyBlend > 0.0f && starCount > 0 && renderer.GetStarSSBO().IsValid();
    const StarLightTargets starLight = starsUp ? CreateStarLightTargets(d, out) : StarLightTargets{};
    // The stars undo the exposure compensation curve while the view's Auto exposure resolves it.
    const PipelineBufferBindingRG compensation =
        starLight.Draw.IsValid() ? ResolveViewExposureCompensation(d) : PipelineBufferBindingRG{};
    const bool drawsStars = starLight.Draw.IsValid() && starLight.Resolve.IsValid() && compensation.IsValid();
    const bool starsUndoCompensation =
        d.Services.GetEffectivePostProcessSettings(viewId, d.View.worldId).IsAutoExposureActive();
    if (drawsStars)
        d.PublishTexture(Names::Res::SkyStarLight, starLight.Resolve);
    else
        PublishNoStarLight(d);
    if (!out.IsValid() || !exposure.IsValid())
        return;

    // AT DECLARATION: the world arm (later in blueprint order) reads this
    // when deciding Clear-vs-Load for its color attach.
    rs.Views().MarkViewSkyBackdropScheduled(viewId);
    const RenderGraph::RGTexture trans = d.Frame.ImportExternalTexture(
        "Sky_Transmittance_LUT", renderer.GetTransmittanceLutTexture(),
        renderer.IsTransmittanceComputed() ? Rendering::ResourceState::ShaderResource
                                           : Rendering::ResourceState::Common);

    // Viewport snapshot at declaration (the old exec's GetTextureSize +
    // fallback chain dies).
    uint32_t vpX = 0, vpY = 0;
    uint32_t vpW = d.RenderWidth, vpH = d.RenderHeight;
    const ViewLetterbox letterbox = rs.Views().GetViewLetterbox(viewId);
    if (letterbox.active)
    {
        vpX = letterbox.x;
        vpY = letterbox.y;
        vpW = letterbox.width;
        vpH = letterbox.height;
    }

    auto* rendererPtr = &renderer;
    auto* skyPtr = &sky;
    auto* rsPtr = &rs;
    d.Frame.AddPass(
        d.PassName().c_str(), Rendering::PassPhase::kSkyRender,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Clear;
            ops.Store = RenderGraph::RGStoreOp::Store;
            ops.Clear.Color[3] = 1.0f;
            p.AttachColor(0, out, ops);
            // The star billboard's vertex shader samples both LUTs (the sky behind each star and the
            // extinction along it), and the sky-view LUT is baked by compute this frame: the reads must
            // reach the vertex stage, not the fragment stage alone.
            p.Read(skySource, RenderGraph::RGTextureRead::SampledVertex);
            p.Read(trans, RenderGraph::RGTextureRead::SampledVertex);
            p.Read(exposure.Graph, RenderGraph::RGBufferRead::Storage);
        },
        [this, rendererPtr, skyPtr, rsPtr, viewId, viewKey, vpX, vpY, vpW, vpH, exposure,
         starsUndoCompensation, fog](RenderGraph::RGContext& passCtx)
        {
            auto* cl = passCtx.Cmd;
            auto* dev = passCtx.GetDevice();
            if (!cl || !dev)
                return;

            Rendering::CameraId cameraId{0};
            for (const auto& v : rsPtr->Views().GetViews())
            {
                if (v.id == viewId)
                {
                    cameraId = v.cameraId;
                    break;
                }
            }
            const auto* camData = rsPtr->Views().FindCameraData(cameraId);
            if (!camData)
            {
                LOG_WARNING("SkyRenderNode: no camera data for viewId={} cameraId={}",
                            (uint32_t)viewId, (uint32_t)cameraId);
                return;
            }

            const CameraVectors camera = ExtractCameraVectors(*camData);
            const float tanHalfFovY = ExtractTanHalfFovY(*camData);
            const float aspect = ExtractAspect(*camData);

            const float vw = static_cast<float>(std::max(vpW, 1u));
            const float vh = static_cast<float>(std::max(vpH, 1u));

            const auto& settings = skyPtr->GetSettings();
            SkyUBOGPU skyUbo{};
            FillSkyUBOFromSettings(settings, skyUbo);
            skyUbo.cameraPosition[0] = camera.Position.x;
            skyUbo.cameraPosition[1] = camera.Position.y;
            skyUbo.cameraPosition[2] = camera.Position.z;
            skyUbo.cameraRight[0] = camera.Right.x;
            skyUbo.cameraRight[1] = camera.Right.y;
            skyUbo.cameraRight[2] = camera.Right.z;
            skyUbo.cameraUp[0] = camera.Up.x;
            skyUbo.cameraUp[1] = camera.Up.y;
            skyUbo.cameraUp[2] = camera.Up.z;
            skyUbo.cameraForward[0] = camera.Forward.x;
            skyUbo.cameraForward[1] = camera.Forward.y;
            skyUbo.cameraForward[2] = camera.Forward.z;
            skyUbo.viewportAspect = (vh > 0.0f) ? (vw / vh) : aspect;
            skyUbo.tanHalfFovY = tanHalfFovY;
            skyUbo.skyRotateAroundZenithRadians =
                (tanHalfFovY <= 0.0f && camData->cameraPos[3] > 0.5f) ? 1.0f : 0.0f;
            skyUbo.viewportResolution[0] = vw;
            skyUbo.viewportResolution[1] = vh;
            skyUbo.starsUndoCompensation = starsUndoCompensation ? 1.0f : 0.0f;

            const uint32_t slot = rendererPtr->GetFrameSlot(dev);
            Rendering::BufferHandle bSky = GetOrCreatePerViewSkyUBO(dev, viewKey, slot);
            void* pSky = dev->MapBuffer(bSky);
            if (!pSky)
                return;
            std::memcpy(pSky, &skyUbo, sizeof(skyUbo));
            dev->UnmapBuffer(bSky);

            Rendering::AtmosphereParametersGPU atmo{};
            FillAtmosphereFromSettings(settings, atmo);
            Rendering::BufferHandle bAtmo = rendererPtr->GetRenderAtmoUBO(slot);
            void* pAtmo = dev->MapBuffer(bAtmo);
            if (!pAtmo)
                return;
            std::memcpy(pAtmo, &atmo, sizeof(atmo));
            dev->UnmapBuffer(bAtmo);

            auto pipeVar =
                passCtx.GetOrCreatePipelineVariant(rendererPtr->EnsureSkyRenderId(*dev, fog.Active));
            if (!pipeVar.IsValid())
                return;
            cl->SetPipeline(pipeVar);
            cl->SetViewport(static_cast<float>(vpX), static_cast<float>(vpY), vw, vh);
            cl->SetScissor(vpX, vpY, vpW, vpH);

            const auto& slots = rendererPtr->GetBindingSlots().SkyRender;
            auto ds = dev->CreateDescriptorSet(rendererPtr->GetSkyRenderSetDesc(fog.Active));
            dev->UpdateBufferBinding(ds, slots.Sky, bSky, 0, sizeof(skyUbo));
            dev->UpdateCombinedImageSamplerBinding(
                ds, slots.SkyViewLUT,
                settings.environmentTexture.IsValid() ? settings.environmentTexture
                                                      : rendererPtr->GetSkyViewLutTexture(),
                rendererPtr->GetSkyViewSampler());
            dev->UpdateBufferBinding(ds, slots.Atmos, bAtmo, 0, sizeof(atmo));
            dev->UpdateCombinedImageSamplerBinding(ds, slots.TransLUT,
                                                   rendererPtr->GetTransmittanceLutTexture(),
                                                   rendererPtr->GetLinearSampler());
            dev->UpdateCombinedImageSamplerBinding(ds, slots.MoonFull,
                                                   rendererPtr->EnsureMoonFullTexture(),
                                                   rendererPtr->GetLinearSampler());
            dev->UpdateStorageBufferBinding(ds, slots.Exposure, exposure.Buffer, exposure.Offset,
                                            exposure.Size);
            if (fog.Active)
            {
                dev->UpdateBufferBinding(ds, slots.ViewParams, fog.ViewParams.Buffer, fog.ViewParams.Offset,
                                         fog.ViewParams.Size);
                dev->UpdateBufferBinding(ds, slots.HeightFog, fog.Params.Buffer, fog.Params.Offset, fog.Params.Size);
                dev->UpdateCombinedImageSamplerBinding(
                    ds, slots.HeightFogBackdrop,
                    settings.environmentTexture.IsValid() ? settings.environmentTexture
                                                          : rendererPtr->GetSkyViewLutTexture(),
                    fog.BackdropSampler);
            }

            cl->BindDescriptorSet(0, ds, pipeVar);
            cl->Draw(3, 1);
        });

    if (!drawsStars)
        return;
    const std::string starsPassName = d.PassName() + ".Stars";
    d.Frame.AddPass(
        starsPassName.c_str(), Rendering::PassPhase::kSkyRender,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps colorOps{};
            colorOps.Load = RenderGraph::RGLoadOp::Load;
            colorOps.Store = RenderGraph::RGStoreOp::Store;
            p.AttachColor(0, out, colorOps);
            RenderGraph::RGAttachmentOps starOps{};
            starOps.Load = RenderGraph::RGLoadOp::Clear;
            starOps.Store = RenderGraph::RGStoreOp::Store;
            if (starLight.Resolve.Id != starLight.Draw.Id)
                p.AttachColorResolve(1, starLight.Draw, starLight.Resolve, starOps);
            else
                p.AttachColor(1, starLight.Draw, starOps);
            // The vertex shader reads the sky behind each star and the extinction along it.
            p.Read(skySource, RenderGraph::RGTextureRead::SampledVertex);
            p.Read(trans, RenderGraph::RGTextureRead::SampledVertex);
            p.Read(exposure.Graph, RenderGraph::RGBufferRead::Storage);
            p.Read(compensation.Graph, RenderGraph::RGBufferRead::Storage);
        },
        [this, rendererPtr, viewKey, vpX, vpY, vpW, vpH, exposure, compensation, starCount, instanceCount,
         fog](RenderGraph::RGContext& passCtx)
        {
            RecordStars(passCtx, *rendererPtr, viewKey, vpX, vpY, vpW, vpH, exposure, compensation, starCount,
                        instanceCount, fog);
        });
}

// The star billboards, additive into scene colour and the star light: instanceCount quads, the
// falling stars' slots first, then the starCount catalog stars. The sky pass filled this view's sky
// UBO and the atmosphere UBO for this frame slot.
void SkyRenderNode::RecordStars(RenderGraph::RGContext& passCtx, Rendering::SkyRenderer& renderer, uint32_t viewKey,
                                uint32_t vpX, uint32_t vpY, uint32_t vpW, uint32_t vpH,
                                const PipelineBufferBindingRG& exposure,
                                const PipelineBufferBindingRG& compensation, uint32_t starCount,
                                uint32_t instanceCount, const SkyHeightFog& fog)
{
    auto* cl = passCtx.Cmd;
    auto* dev = passCtx.GetDevice();
    if (!cl || !dev)
        return;
    auto* rendererPtr = &renderer;
    const uint32_t slot = rendererPtr->GetFrameSlot(dev);
    const Rendering::BufferHandle bSky = GetOrCreatePerViewSkyUBO(dev, viewKey, slot);
    const Rendering::BufferHandle bAtmo = rendererPtr->GetRenderAtmoUBO(slot);
    cl->SetViewport(static_cast<float>(vpX), static_cast<float>(vpY), static_cast<float>(std::max(vpW, 1u)),
                    static_cast<float>(std::max(vpH, 1u)));
    cl->SetScissor(vpX, vpY, vpW, vpH);
    auto starPipe =
        passCtx.GetOrCreatePipelineVariant(rendererPtr->EnsureStarBillboardId(*dev, fog.Active));
    if (!starPipe.IsValid())
        return;
    cl->SetPipeline(starPipe);
    const auto& starSlots = rendererPtr->GetBindingSlots().StarBillboard;
    auto starDS = dev->CreateDescriptorSet(rendererPtr->GetStarBillboardSetDesc(fog.Active));
    dev->UpdateStorageBufferBinding(starDS, starSlots.Stars, rendererPtr->GetStarSSBO(), 0,
                                    starCount * sizeof(Rendering::StarInstanceGPU));
    dev->UpdateBufferBinding(starDS, starSlots.Sky, bSky, 0, sizeof(SkyUBOGPU));
    dev->UpdateStorageBufferBinding(starDS, starSlots.Exposure, exposure.Buffer,
                                    exposure.Offset, exposure.Size);
    dev->UpdateStorageBufferBinding(starDS, starSlots.Compensation, compensation.Buffer, compensation.Offset,
                                    compensation.Size);
    dev->UpdateCombinedImageSamplerBinding(starDS, starSlots.SkyViewLUT,
                                           rendererPtr->GetSkyViewLutTexture(),
                                           rendererPtr->GetSkyViewSampler());
    dev->UpdateBufferBinding(starDS, starSlots.Atmos, bAtmo, 0, sizeof(Rendering::AtmosphereParametersGPU));
    dev->UpdateCombinedImageSamplerBinding(starDS, starSlots.TransLUT,
                                           rendererPtr->GetTransmittanceLutTexture(),
                                           rendererPtr->GetLinearSampler());
    if (fog.Active)
        dev->UpdateBufferBinding(starDS, starSlots.HeightFog, fog.Params.Buffer, fog.Params.Offset, fog.Params.Size);
    cl->BindDescriptorSet(0, starDS, starPipe);
    cl->Draw(6, instanceCount);
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
