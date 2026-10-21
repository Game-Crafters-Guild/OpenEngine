#ifndef GE_SKY_ATMOSPHERE_UBO_GLSL
#define GE_SKY_ATMOSPHERE_UBO_GLSL

// Canonical std140 AtmosphereUBO, declared once and shared by every sky pass.
// Byte-identical to AtmosphereParametersGPU (240 B) in
// Rendering/Sky/SkyRenderer.h, whose field offsets are all static_asserted --
// keep this block in lock-step with that struct. Every sky pass uploads the
// full 240 B AtmosphereParametersGPU (the capture packs it at
// kCaptureAtmoUboOffset), so a pass may declare the whole block here even when
// it only reads a prefix of it.
//
// The bind point differs per pass, so define GE_ATMOSPHERE_UBO_BINDING before
// including. It defaults to binding 2 (sky-view / render / multiscatter /
// capture); the transmittance pass overrides it to 1.
#ifndef GE_ATMOSPHERE_UBO_BINDING
#define GE_ATMOSPHERE_UBO_BINDING 2
#endif

layout(set = 0, binding = GE_ATMOSPHERE_UBO_BINDING) uniform AtmosphereUBO
{
    float planetRadius;
    float atmosphereRadius;
    float rayleighScaleHeight;
    float mieScaleHeight;
    vec3  betaRayleigh;   float pad0;
    vec3  betaMie;        float pad1;
    vec3  groundAlbedo;
    float groundBrightness;
    vec3  groundNightColor;
    vec3  groundHorizonColor;
    vec3  groundHorizonNightColor;
    vec3  nightSkyHorizonColor;
    float nightSkyHorizonPad;
    float groundHorizonDayCosWidth;
    float groundHorizonNightCosWidth;
    float belowHorizonBlendSharpness;
    float belowHorizonDarkness;
    vec2  belowHorizonDarkPadding;
    float belowHorizonDarkColorStd140PrePad0;
    float belowHorizonDarkColorStd140PrePad1;
    float belowHorizonDarkColorStd140PrePad2;
    vec3  belowHorizonDarkColor;
    uint  belowHorizonMode;
    float groundHazeStrength;
    vec3  mieAbsorption;  float ozoneCenterHeight;
    vec3  ozoneAbsorption; float ozoneHalfWidth;
} uAtmos;

// The medium at height h above the ground (metres), shared by every pass that integrates the
// atmosphere: Rayleigh and Mie scattering on their exponential profiles, Mie absorption on the
// Mie profile, ozone absorption on its tent (Hillaire 2020, section 3 and table 1).
struct GE_SkyMedium
{
    vec3 scatterRayleigh;
    vec3 scatterMie;
    vec3 extinction;
};

GE_SkyMedium GE_SkyMediumAt(float h)
{
    float rayleighDensity = exp(-h / uAtmos.rayleighScaleHeight);
    float mieDensity = exp(-h / uAtmos.mieScaleHeight);
    float ozoneDensity = max(0.0, 1.0 - abs(h - uAtmos.ozoneCenterHeight) / uAtmos.ozoneHalfWidth);
    GE_SkyMedium m;
    m.scatterRayleigh = uAtmos.betaRayleigh * rayleighDensity;
    m.scatterMie = uAtmos.betaMie * mieDensity;
    m.extinction = m.scatterRayleigh + (uAtmos.betaMie + uAtmos.mieAbsorption) * mieDensity +
                   uAtmos.ozoneAbsorption * ozoneDensity;
    return m;
}

// The aerosol's phase function (per steradian, cosTheta between the view ray and the direction
// toward the sun): a three-lobe Henyey-Greenstein fit to the Mie phase function of the OPAC
// continental-average aerosol at 550 nm (Hess, Koepke and Schult 1998, BAMS 79(5), the
// water-soluble, insoluble and soot components), asymmetry 0.62. The narrow lobe is the coarse
// particles' diffraction peak, the sun's aureole: 14.5 per steradian at the sun against Mie's 14.3,
// within 12 % out to 5 degrees. The broad lobes carry the side and back scatter: within 4 % from 85
// to 95 degrees (0.026 at 90, where a single lobe at g 0.8 gives 0.008 to 0.014) and 20 % from 30
// to 180, so haze lifts the horizon all around the sky. 5 to 30 degrees is the fit's worst: 30 %
// over Mie at 10 degrees, and 0.416 of the scattered energy in that band against Mie's 0.362, which
// three lobes cannot shape away; the tabulated Mie phase is the follow-up (#3959).
float GE_SkyHenyeyGreenstein(float g, float cosTheta)
{
    float d = 1.0 + g * g - 2.0 * g * cosTheta;
    return (1.0 - g * g) / (4.0 * 3.14159265359 * d * sqrt(d));
}

float GE_SkyAerosolPhase(float cosTheta)
{
    const float kPeakWeight = 0.0362;
    const float kPeakG = 0.9795;
    const float kForwardWeight = 0.9530;
    const float kForwardG = 0.6503;
    const float kBackwardWeight = 0.0108;
    const float kBackwardG = -0.7055;
    return kPeakWeight * GE_SkyHenyeyGreenstein(kPeakG, cosTheta) +
           kForwardWeight * GE_SkyHenyeyGreenstein(kForwardG, cosTheta) +
           kBackwardWeight * GE_SkyHenyeyGreenstein(kBackwardG, cosTheta);
}

#endif
