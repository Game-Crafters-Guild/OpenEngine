#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Sky/SkySettings.h"
#include "Rendering/Sky/SkySystem.h"
#include "Rendering/Sky/StarCatalog.h"

#include <cstddef>
#include <filesystem>

namespace GameEngine
{
namespace Rendering
{

struct AtmosphereParametersGPU
{
    float planetRadius;
    float atmosphereRadius;
    float rayleighScaleHeight;
    float mieScaleHeight;
    float betaRayleigh[3];
    float pad0;
    float betaMie[3];
    float pad1;
    float groundAlbedo[3];
    float groundBrightness;
    float groundNightColor[3];
    float groundNightColorStd140Pad;
    float groundHorizonColor[3];
    float groundHorizonColorStd140Pad;
    float groundHorizonNightColor[3];
    // std140 gives every vec3 a full 16-byte slot, so a bare pad must follow each
    // vec3 that is itself followed by another vec3. Omitting this one skewed every
    // field below it by 4 bytes versus the shader's layout.
    float groundHorizonNightColorStd140Pad;
    float nightSkyHorizonColor[3];
    float nightSkyHorizonColorStd140Pad;
    float groundHorizonDayCosWidth;
    float groundHorizonNightCosWidth;
    float belowHorizonBlendSharpness;
    float belowHorizonDarkness;
    float belowHorizonDarkPadding[2];
    // vec3 belowHorizonDarkColor needs a 16-byte boundary; std140 inserts 24 bytes
    // of padding after belowHorizonDarkPadding to reach offset 176.
    float belowHorizonDarkColorStd140PrePad[6];
    float belowHorizonDarkColor[3];
    // Trailing scalar packs into belowHorizonDarkColor's 16-byte slot at offset 188.
    uint32_t belowHorizonMode;
    // PlanetGround aerial-perspective haze strength on its own std140 row at 192.
    float groundHazeStrength;
    float groundHazeStrengthPad[3];
    // Aerosol absorption per metre at sea level (Mie extinction = betaMie + mieAbsorption), on the
    // Mie density profile.
    float mieAbsorption[3];
    // Ozone layer: a tent of density 1 at ozoneCenterHeight falling linearly to 0 at
    // ozoneCenterHeight +- ozoneHalfWidth (metres). It absorbs and does not scatter.
    float ozoneCenterHeight;
    float ozoneAbsorption[3];
    float ozoneHalfWidth;
};

static_assert(sizeof(AtmosphereParametersGPU) == 240,
              "AtmosphereParametersGPU must match GLSL AtmosphereUBO std140 layout");
static_assert(offsetof(AtmosphereParametersGPU, groundNightColor) == 64, "AtmosphereUBO groundNightColor offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, groundHorizonColor) == 80, "AtmosphereUBO groundHorizonColor offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, groundHorizonNightColor) == 96, "AtmosphereUBO groundHorizonNightColor offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, nightSkyHorizonColor) == 112, "AtmosphereUBO nightSkyHorizonColor offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, nightSkyHorizonColorStd140Pad) == 124, "AtmosphereUBO nightSkyHorizonColorStd140Pad offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, groundHorizonDayCosWidth) == 128, "AtmosphereUBO groundHorizonDayCosWidth offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, groundHorizonNightCosWidth) == 132, "AtmosphereUBO groundHorizonNightCosWidth offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, belowHorizonBlendSharpness) == 136, "AtmosphereUBO belowHorizonBlendSharpness offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, belowHorizonDarkness) == 140, "AtmosphereUBO belowHorizonDarkness offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, belowHorizonDarkPadding) == 144, "AtmosphereUBO belowHorizonDarkPadding offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, belowHorizonDarkColorStd140PrePad) == 152,
              "AtmosphereUBO belowHorizonDarkColorStd140PrePad offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, belowHorizonDarkColor) == 176, "AtmosphereUBO belowHorizonDarkColor offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, belowHorizonMode) == 188, "AtmosphereUBO belowHorizonMode offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, groundHazeStrength) == 192, "AtmosphereUBO groundHazeStrength offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, mieAbsorption) == 208, "AtmosphereUBO mieAbsorption offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, ozoneCenterHeight) == 220, "AtmosphereUBO ozoneCenterHeight offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, ozoneAbsorption) == 224, "AtmosphereUBO ozoneAbsorption offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, ozoneHalfWidth) == 236, "AtmosphereUBO ozoneHalfWidth offset mismatch");

// Camera + celestial + star upload block. Hand-padded so C++ tight-packing
// matches the GLSL SkyUBO std140 layout byte-for-byte (272 B). Shared by the
// sky-view, render and star-billboard passes; the capture pass uses the smaller
// CaptureSkyUbo prefix instead. Keep in lock-step with the SkyUBO block in
// sky_render.frag / sky_stars_billboard.{vert,frag}.
struct SkyUBOGPU
{
    float cameraPosition[3]; float exposureEV;
    float cameraRight[3];    float pad0;
    float cameraUp[3];       float pad1;
    float cameraForward[3];  float skyPan2D;
    float sunDirection[3];   float sunAngularRadius;
    float moonDirection[3];  float moonIntensity;
    float moonAngularRadius;
    float skyRotateAroundZenithRadians;
    float moonUboPad1;
    float moonUboPad2;
    float sunColor[3];       float sunIntensity;
    float timeOfDayHours;    float starsUndoCompensation; // 1: the star pass undoes the exposure compensation
    float viewportAspect;    float tanHalfFovY;
    float viewportResolution[2]; float nightSkyBlend; float skyTimeSeconds;
    // x = limiting magnitude (V), y = star highlight compression (1 = physical),
    // z = twinkle amount at airmass 10, w = twinkle rate.
    float starModel[4];
    // x = HDRI rotation in radians; y = HDRI intensity, or kSkyHdriProceduralSentinel (< 0) for
    // the procedural sky; z = the stars' artistic glow (0 = none); w = 1 when faint stars fade to white.
    float skyboxParams[4];
    // x = the falling stars' slot count, the first instances of the star billboard's draw (0 = off);
    // y = the mean interval between two falling stars in seconds; zw unused.
    float fallingStarParams[4];
    // Gradient sky: .rgb = scene-linear premultiplied color, gradientSkyTop.w = mode sentinel
    // (>= 0.5 -> render the gradient instead of the atmosphere). Blended by view-dir.y.
    float gradientSkyTop[4];
    float gradientSkyHorizon[4];
    float gradientSkyBottom[4];
    // x = star core size (1 = physical), y = the spikes' share of the glow, z = spike length in glow
    // radii, w = the star shape (0 = point, 1 = sparkle). Only the star billboard declares it.
    float starShape[4];
};

static_assert(sizeof(SkyUBOGPU) == 272, "SkyUBOGPU must match GLSL SkyUBO std140 layout");
static_assert(offsetof(SkyUBOGPU, sunColor) == 112, "SkyUBO sunColor offset mismatch");
static_assert(offsetof(SkyUBOGPU, starModel) == 160, "SkyUBO starModel offset mismatch");
static_assert(offsetof(SkyUBOGPU, skyboxParams) == 176, "SkyUBO skyboxParams offset mismatch");
static_assert(offsetof(SkyUBOGPU, fallingStarParams) == 192, "SkyUBO fallingStarParams offset mismatch");
static_assert(offsetof(SkyUBOGPU, gradientSkyTop) == 208, "SkyUBO gradientSkyTop offset mismatch");
static_assert(offsetof(SkyUBOGPU, gradientSkyHorizon) == 224, "SkyUBO gradientSkyHorizon offset mismatch");
static_assert(offsetof(SkyUBOGPU, gradientSkyBottom) == 240, "SkyUBO gradientSkyBottom offset mismatch");
static_assert(offsetof(SkyUBOGPU, starShape) == 256, "SkyUBO starShape offset mismatch");

// Engine-owned moon art, relative to the install assets root. The Editor's and Player's
// asset staging copy the repo's Engine/Assets/Textures tree next to the executable, so this is a
// staged-tree path and never a repo path.
inline constexpr const char* kSkyMoonFullTextureRelativePath = "Textures/Sky/MoonPhases/moon_full.png";

class SkyRenderer
{
  public:
    struct Config
    {
        uint32_t skyViewLutWidth = 256;
        uint32_t skyViewLutHeight = 256;

        uint32_t transmittanceLutWidth = 256;
        uint32_t transmittanceLutHeight = 64;

        // Hillaire multiscatter LUT is a coarse 2D table (sunCosZenith x height);
        // 32x32 is the production size — the data is very low-frequency.
        uint32_t multiscatterLutSize = 32;

        // Where the host staged the engine assets (ApplicationPaths::GetInstallAssetsRoot()).
        // The renderer does not derive it: install layout is the host's concern, and this
        // module has no dependency that can answer it. Empty leaves engine-owned sky art
        // unresolvable, which the load path reports rather than silently substituting.
        std::filesystem::path installAssetsRoot;
    };

    // Set0 binding indices for each sky pipeline, resolved from the shaderpkg's
    // reflected meta by name at CreatePipelines. Both this class's pipeline
    // layouts and SkyRenderNode's transient descriptor sets are built from these
    // (single source of truth), so a shader layout edit stays in lockstep with
    // the C++ binds. Defaults are the GLSL literals used as a reflection-miss
    // fallback. Only the INDEX comes from reflection; descriptor types stay
    // explicit (the reflected type enum is unreliable for images).
    struct SkyBindingSlots
    {
        struct
        {
            uint32_t TransLUT = 0; // uTransLUT (storage image)
            uint32_t Atmos = 1;    // uAtmos (UBO)
        } Transmittance;
        struct
        {
            uint32_t MultiscatterLUT = 0;  // oMultiscatterLUT (storage image)
            uint32_t TransmittanceLUT = 1; // uTransmittanceLUT (sampler)
            uint32_t Atmos = 2;            // uAtmos (UBO)
        } Multiscatter;
        struct
        {
            uint32_t SkyViewLUT = 0;       // oSkyViewLUT (storage image)
            uint32_t TransmittanceLUT = 1; // uTransmittanceLUT (sampler)
            uint32_t Atmos = 2;            // uAtmos (UBO)
            uint32_t Sky = 3;              // uSky (UBO)
            uint32_t MultiscatterLUT = 4;  // uMultiscatterLUT (sampler)
        } SkyViewLut;
        struct
        {
            uint32_t Sky = 0;        // uSky (UBO)
            uint32_t SkyViewLUT = 1; // uSkyViewLUT (sampler)
            uint32_t Atmos = 2;      // uAtmos (UBO)
            uint32_t TransLUT = 3;   // uTransLUT (sampler)
            uint32_t MoonFull = 4;   // uMoonFullTexture (sampler)
            uint32_t Exposure = 5;   // uExposure (SSBO, the view's ExposureHistory)
            // The height fog variant (sky_render_fog) only:
            uint32_t ViewParams = 6;        // ViewParams (UBO): the pixel's direction and the far plane
            uint32_t HeightFog = 7;         // HeightFogParams (UBO)
            uint32_t HeightFogBackdrop = 8; // uHeightFogBackdrop (sampler): the backdrop the fog's sky colour reads
        } SkyRender;
        struct
        {
            uint32_t Stars = 0;      // sb (SSBO)
            uint32_t Sky = 1;        // uSky (UBO)
            uint32_t Exposure = 2;   // uExposure (SSBO, the view's ExposureHistory)
            uint32_t SkyViewLUT = 3; // uSkyViewLUT (sampler): the sky behind each star, for its contrast
            uint32_t Atmos = 4;      // uAtmos (UBO)
            uint32_t TransLUT = 5;   // uTransLUT (sampler): each star's extinction
            uint32_t Compensation = 6; // uCompensation (SSBO, the view's exposure compensation state)
            uint32_t HeightFog = 7;  // HeightFogParams (UBO), the height fog variant (sky_stars_billboard_fog) only
        } StarBillboard;
    };

    SkyRenderer();
    ~SkyRenderer();

    bool Initialize(IDevice* device, const Config& config);

    // Q6 slice 4 (§8-completion): after an in-place device rebuild, the LUT
    // textures, samplers, star SSBO and per-frame UBOs are dead but their handles
    // still read IsValid(). Re-run Initialize (which overwrites every handle with a
    // fresh resource — no DestroyTexture/Buffer on the dead ones, so no double-free)
    // and reset the one-shot LUT-compute flags so the transmittance / sky-view /
    // multiscatter LUTs recompute into the fresh textures. The lazily-loaded moon
    // texture is zeroed so its getter reloads it.
    void ReprovisionAfterDeviceRebuild();

    struct CameraState
    {
        float position[3] = {0, 0, 0};
        // LH +Z. Live sky does not read this (SkyRenderNode::ExtractCameraVectors
        // copies view row 2). The default must still match that convention so a
        // future SetCamera caller does not inherit a right-handed look.
        float forward[3] = {0, 0, 1};
        float up[3] = {0, 1, 0};
        float right[3] = {1, 0, 0};
        float fovYDegrees = 60.0f;
        float aspect = 1.777f;
        uint32_t viewportWidth = 1280;
        uint32_t viewportHeight = 720;
    };

    void SetCamera(const CameraState& camera);

    void Update(const SkySettings& settings, const SkySystemState& state);

    static void FillDefaultAtmosphere(AtmosphereParametersGPU& atmo);

    /// The sky-view LUT's texel format for a device that loads kernels of the given kind.
    ///
    /// The LUT stores absolute scene-linear radiance (1.0 = 203 nits, never pre-exposed), from a
    /// noon sky near 30 down to a moonless night near 8e-7. Binary16's smallest normal is 6.1e-5,
    /// and its subnormals are 6e-8 apart, so the SPIR-V kernel writes RGBA32F. The compat (WGSL)
    /// kernel keeps RGBA16F: WebGPU filters 32-bit floats only behind an optional feature. The
    /// kernel's storage qualifier (sky_view_lut.comp) and this format change together.
    static TextureFormat SkyViewLutFormat(ShaderSourceKind kernels);

    IDevice* GetDevice() const { return m_Device; }
    const Config& GetConfig() const { return m_Config; }
    TextureHandle GetTransmittanceLutTexture() const { return m_TransmittanceLutTex; }
    TextureHandle GetSkyViewLutTexture() const { return m_SkyViewLutTex; }
    TextureHandle GetMultiscatterLutTexture() const { return m_MultiscatterLutTex; }
    TextureHandle EnsureMoonFullTexture();
    SamplerHandle GetLinearSampler() const { return m_LinearSampler; }
    // Sampler for the sky-view LUT reads only: Repeat on u so the full-circle
    // azimuth wraps seamlessly across u=0/1, ClampToEdge on v for the elevation
    // poles. Other LUT reads (transmittance, multiscatter, moon) keep using the
    // fully-clamped m_LinearSampler.
    SamplerHandle GetSkyViewSampler() const { return m_SkyViewSampler; }
    // Lazy-intern the compute / graphics pipeline ids on first call. Subsequent
    // calls return the cached id. Thread-safe: intern is idempotent across
    // callers via the device's pipeline cache.
    ComputePipelineId  EnsureTransmittanceId(IDevice& device);
    ComputePipelineId  EnsureMultiscatterId(IDevice& device);
    ComputePipelineId  EnsureSkyViewLutId(IDevice& device);
    // The sky, with the height fog's sky fill over it when heightFog is set (sky_render_fog). The
    // height fog variants exist only where their packages were built (HasHeightFogPipelines).
    GraphicsPipelineId EnsureSkyRenderId(IDevice& device, bool heightFog);
    // The stars, added to colour 0 (scene colour) and colour 1 (the star light, R16F luminance),
    // dimmed by the height fog's sky fill when heightFog is set (sky_stars_billboard_fog).
    GraphicsPipelineId EnsureStarBillboardId(IDevice& device, bool heightFog);
    // Whether the height fog variants of the sky and the stars loaded. A runtime without their
    // packages (the web player, whose graph declares no height fog) draws the sky without the fill.
    bool HasHeightFogPipelines() const { return m_HasHeightFogPipelines; }
    BufferHandle GetStarSSBO() const { return m_StarSSBO; }
    /// The instance count that draws every star of the field down to the limiting magnitude, plus
    /// the quarter magnitude over which the vertex shader fades the limit out.
    uint32_t GetStarInstanceCount(float limitingMagnitude) const;
    bool IsTransmittanceComputed() const { return m_TransmittanceComputed; }
    void MarkTransmittanceComputed() { m_TransmittanceComputed = true; }

    // Sky-view LUT dirty tracking. The LUT is a function of SkySettings
    // (sun direction, atmosphere, etc.) and of the camera's altitude band
    // (Rendering/Sky/SkyViewLutAltitude.h), never of the raw camera. The pipeline
    // node computes a digest of those inputs each frame and
    // calls IsSkyViewLutCurrentFor() in its activation predicate; if the
    // digest matches the last-computed value, the dispatch is skipped.
    // MarkSkyViewLutComputed() is called from the execute lambda after
    // a successful dispatch.
    bool IsSkyViewLutCurrentFor(uint64_t inputDigest) const
    {
        return m_SkyViewLutComputedOnce && m_SkyViewLutInputDigest == inputDigest;
    }
    void MarkSkyViewLutComputed(uint64_t inputDigest)
    {
        m_SkyViewLutInputDigest = inputDigest;
        m_SkyViewLutComputedOnce = true;
    }
    // The digest of the sky state the LUT currently CONTAINS (stamped after the
    // compute executes; 0 until first computed). Consumers that capture the LUT
    // (probe bakes) fold this into their own input digests so a capture
    // triggered on the frame the settings change re-triggers once the LUT
    // catches up — instead of silently keeping the previous sky forever.
    uint64_t GetSkyViewLutComputedDigest() const
    {
        return m_SkyViewLutComputedOnce ? m_SkyViewLutInputDigest : 0;
    }

    // Multiscatter LUT dirty tracking. Takes the sky-view LUT's settings digest
    // without the altitude band (the multiscatter LUT is a function of the same
    // atmosphere + sun-zenith parameterization, over every height). Gated independently so the three-pass chain
    // (transmittance -> multiscatter -> sky-view) re-runs together on a change.
    bool IsMultiscatterCurrentFor(uint64_t inputDigest) const
    {
        return m_MultiscatterComputedOnce && m_MultiscatterInputDigest == inputDigest;
    }
    void MarkMultiscatterComputed(uint64_t inputDigest)
    {
        m_MultiscatterInputDigest = inputDigest;
        m_MultiscatterComputedOnce = true;
    }

    const SkySettings& GetSettings() const { return m_Settings; }
    const SkySystemState& GetSystemState() const { return m_State; }
    const CameraState& GetCamera() const { return m_Camera; }

    // Reflected set0 binding indices shared with SkyRenderNode (see SkyBindingSlots).
    const SkyBindingSlots& GetBindingSlots() const { return m_Bindings; }
    // The descriptor sets SkyRenderNode binds against the sky and star pipelines, built by
    // Initialize. Their layouts are the pipelines' own set0 layouts: WebGPU refuses a bind group
    // whose layout differs from its pipeline's in any entry.
    const DescriptorSetDesc& GetSkyRenderSetDesc(bool heightFog) const
    {
        return heightFog ? m_SkyRenderFog.SetDesc : m_SkyRender.SetDesc;
    }
    const DescriptorSetDesc& GetStarBillboardSetDesc(bool heightFog) const
    {
        return heightFog ? m_StarBillboardFog.SetDesc : m_StarBillboard.SetDesc;
    }

    uint32_t GetFrameSlot(IDevice* device) const;

    BufferHandle GetSkyViewLutSkyUBO(uint32_t slot) const { return m_SkyViewLutSkyUBO[slot]; }
    BufferHandle GetSkyViewLutAtmoUBO(uint32_t slot) const { return m_SkyViewLutAtmoUBO[slot]; }
    BufferHandle GetMultiscatterAtmoUBO(uint32_t slot) const { return m_MultiscatterAtmoUBO[slot]; }
    BufferHandle GetSkyRenderUBO(uint32_t slot) const { return m_SkyRenderUBO[slot]; }
    BufferHandle GetTransmittanceAtmoUBO(uint32_t slot) const { return m_TransmittanceAtmoUBO[slot]; }
    BufferHandle GetRenderAtmoUBO(uint32_t slot) const { return m_RenderAtmoUBO[slot]; }

  private:
    // A sky draw pipeline: its description, the descriptor set its node binds against it, and the
    // interned id (on first Ensure).
    struct SkyDrawPipeline
    {
        PipelineDesc Pipe;
        DescriptorSetDesc SetDesc;
        GraphicsPipelineId Id{};
    };

    bool CreateSkyRenderPipeline(const char* packagePath, bool heightFog, SkyDrawPipeline& out);
    bool CreateStarBillboardPipeline(const char* packagePath, bool heightFog, SkyDrawPipeline& out);
    GraphicsPipelineId EnsureDrawPipelineId(IDevice& device, SkyDrawPipeline& pipeline);

    IDevice* m_Device = nullptr;
    Config m_Config;
    SkyBindingSlots m_Bindings;

    TextureHandle m_TransmittanceLutTex;
    TextureHandle m_SkyViewLutTex;
    TextureHandle m_MultiscatterLutTex;
    TextureHandle m_MoonFullTexture;
    SamplerHandle m_LinearSampler;
    SamplerHandle m_SkyViewSampler;

    // Source PipelineDescs kept as shader/state holders during pipeline
    // construction; descs are translated into interned compute/graphics ids
    // on first Execute and the resulting ids drive lookup thereafter.
    PipelineDesc m_TransmittancePipe;
    PipelineDesc m_MultiscatterPipe;
    PipelineDesc m_SkyViewLutPipe;
    SkyDrawPipeline m_SkyRender;
    SkyDrawPipeline m_SkyRenderFog;
    SkyDrawPipeline m_StarBillboard;
    SkyDrawPipeline m_StarBillboardFog;
    bool m_HasHeightFogPipelines = false;

    ComputePipelineId  m_TransmittanceId{};
    ComputePipelineId  m_MultiscatterId{};
    ComputePipelineId  m_SkyViewLutId{};
    BufferHandle m_StarSSBO;
    uint32_t m_StarCount = 0;
    // The drawn field's magnitudes, brightest first: GetStarInstanceCount's prefix search.
    std::vector<float> m_StarMagnitudes;

    static constexpr uint32_t kMaxFramesInFlight = IDevice::kMaxSupportedFramesInFlight;
    uint32_t m_FramesInFlight = 1;

    BufferHandle m_SkyViewLutSkyUBO[kMaxFramesInFlight] = {};
    BufferHandle m_SkyViewLutAtmoUBO[kMaxFramesInFlight] = {};
    BufferHandle m_MultiscatterAtmoUBO[kMaxFramesInFlight] = {};
    BufferHandle m_SkyRenderUBO[kMaxFramesInFlight] = {};
    BufferHandle m_TransmittanceAtmoUBO[kMaxFramesInFlight] = {};
    BufferHandle m_RenderAtmoUBO[kMaxFramesInFlight] = {};

    bool m_TransmittanceComputed = false;
    uint64_t m_SkyViewLutInputDigest = 0;
    bool m_SkyViewLutComputedOnce = false;
    uint64_t m_MultiscatterInputDigest = 0;
    bool m_MultiscatterComputedOnce = false;

    bool CreatePipelines();
    void GenerateStarCatalog();
    void DestroyResources();

    SkySettings m_Settings;
    SkySystemState m_State;
    CameraState m_Camera;
};

} // namespace Rendering
} // namespace GameEngine
