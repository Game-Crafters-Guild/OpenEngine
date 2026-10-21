#version 450

// The night sky's stars: one instanced quad per catalog star, sized in screen pixels.
//
// Each star carries its V magnitude and its colour (StarCatalog.cpp). sky_star_model.glsl turns
// them into what the star puts on screen: its illuminance above the atmosphere, times the
// transmittance LUT along its direction (extinction), on a Gaussian core about a pixel wide, with the
// eye's glare taken from the core and the artistic glow and spikes added. It shows where its magnitude
// after extinction is within the limiting magnitude, lowered by the eye's threshold against the
// sky-view LUT behind it. A star that is not drawn becomes a degenerate triangle. This shader only
// gathers the inputs (GE_StarGeometry decides where the star sits for the sky) and places the quad.

#include "Includes/compat_profile.glsl"
#include "Includes/sky_composite.glsl"
#include "Includes/sky_star_model.glsl"

struct StarInstance {
    vec4 dirAndMagnitude;
    vec4 colorAndSeed;
};

layout(std430, set = 0, binding = 0) readonly buffer StarBuf {
    StarInstance stars[];
} sb;

layout(set = 0, binding = 1) uniform SkyUBO
{
    vec3  cameraPositionWS;   float exposureEV;
    vec3  cameraRightWS;      float pad0;
    vec3  cameraUpWS;         float pad1;
    vec3  cameraForwardWS;    float pad2;
    vec3  sunDirectionWS;     float sunAngularRadius;
    vec3  moonDirectionWS;    float moonIntensity;
    float moonAngularRadius;
    float skyRotateAroundZenithRadians;
    float moonUboPad1;
    float moonUboPad2;
    vec3  sunColor;           float sunIntensity;
    float timeOfDayHours;     float starsUndoCompensation;
    float viewportAspect;     float tanHalfFovY;
    vec2  viewportResolution; float nightSkyBlend; float skyTimeSeconds;
    vec4  starModel;    // x = limiting magnitude, y = highlight compression, z = twinkle amount, w = twinkle rate
    vec4  skyboxParams; // z = the artistic Star glow, w = the observer (1 = the eye, 0 = a camera)
    vec4  fallingStarParams;
    vec4  fallingStarShapeParams;
    vec4  gradientSkyTop;
    vec4  gradientSkyHorizon;
    vec4  gradientSkyBottom;
    vec4  starShape;    // x = Star core, y = Star spikes, z = Spike length, w = shape (0 point, 1 sparkle)
} uSky;

layout(set = 0, binding = 3) uniform sampler2D uSkyViewLUT;
#define GE_ATMOSPHERE_UBO_BINDING 4
#include "Includes/sky_atmosphere_ubo.glsl"
#include "Includes/sky_transmittance_lut.glsl"
layout(set = 0, binding = 5) uniform sampler2D uTransLUT;

#ifdef GE_SKY_HEIGHT_FOG
// The height fog over the sky (sky_stars_billboard_fog, drawn for a view whose height fog is active):
// a star is dimmed by the fog's sky fill in its direction, as the sky behind it is.
layout(set = 0, binding = 7, std140) uniform HeightFogParamsBlock
{
#include "Includes/height_fog_params_fields.glsl"
} HeightFogParams;
#define GE_HEIGHT_FOG_BACKDROP uSkyViewLUT
#include "Includes/height_fog_term.glsl"
#endif

layout(location = 0) out vec2 vPixelOffset;
layout(location = 1) flat out vec3 vCorePeak;
layout(location = 2) flat out float vCoreRadius;
layout(location = 3) flat out vec3 vGlareScale;
layout(location = 4) flat out float vCutRadius;
layout(location = 5) flat out vec3 vGlowPeak;
layout(location = 6) flat out float vGlowRadius;
layout(location = 7) flat out vec4 vSpikePeakAndLength;
layout(location = 8) flat out float vShape;

// The sky-view LUT's height (SkyRenderer::Config::skyViewLutHeight).
const float kSkyViewLutHeight = 256.0;

vec2 QuadOffset(uint vid)
{
    if (vid == 0u) return vec2(-1.0, -1.0);
    if (vid == 1u) return vec2( 1.0, -1.0);
    if (vid == 2u) return vec2(-1.0,  1.0);
    if (vid == 3u) return vec2(-1.0,  1.0);
    if (vid == 4u) return vec2( 1.0, -1.0);
    return              vec2( 1.0,  1.0);
}

void Cull()
{
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
    vPixelOffset = vec2(0.0);
    vCorePeak = vec3(0.0);
    vCoreRadius = 1.0;
    vGlareScale = vec3(0.0);
    vCutRadius = 1.0;
    vGlowPeak = vec3(0.0);
    vGlowRadius = 1.0;
    vSpikePeakAndLength = vec4(0.0, 0.0, 0.0, 1.0);
    vShape = 0.0;
}

// Luminance of the sky behind the star in nits: the sky-view LUT at the star's planet-local direction.
float SkyLuminanceBehind(vec3 lutDirection)
{
    vec2 uv = GE_SkyDirToViewLutUv(lutDirection);
    uv.y = clamp(uv.y, 0.5 / kSkyViewLutHeight, 1.0 - 0.5 / kSkyViewLutHeight);
    vec3 sky = textureLod(uSkyViewLUT, uv, 0.0).rgb;
    return dot(sky, vec3(0.2126, 0.7152, 0.0722)) * kStarSceneLinearNits;
}

void main()
{
    StarInstance star = sb.stars[gl_InstanceIndex];
    vec3 starDir = star.dirAndMagnitude.xyz;
    float magnitude = star.dirAndMagnitude.w;
    vec3 starColor = star.colorAndSeed.rgb;
    float seed = star.colorAndSeed.w;

    vec3 fwd = normalize(uSky.cameraForwardWS);
    vec3 right = normalize(uSky.cameraRightWS);
    vec3 camUp = normalize(uSky.cameraUpWS);
    vec3 viewDir = vec3(dot(starDir, right), dot(starDir, camUp), dot(starDir, fwd));

    // tanHalfFovY <= 0 is the orthographic 2D backdrop: stars on a virtual dome of tan 1.
    bool isOrtho = uSky.tanHalfFovY <= 0.0;
    float tanHalf = isOrtho ? 1.0 : uSky.tanHalfFovY;
    if (uSky.nightSkyBlend <= 0.0 || viewDir.z <= 0.05)
    {
        Cull();
        return;
    }

    vec3 planetCenter = vec3(0.0, -uAtmos.planetRadius, 0.0);
    float camR = length(uSky.cameraPositionWS - planetCenter);
    vec3 up = (uSky.cameraPositionWS - planetCenter) / max(camR, 1e-5);
    GE_StarViewGeometry geometry = GE_StarGeometry(starDir, up, tanHalf, uSky.viewportResolution.y);

    // Extinction: the transmittance from the camera to the top of the atmosphere along the star.
    // Below the planet's horizon it is 0, which hides the star there.
    vec3 transmittance = isOrtho ? vec3(1.0)
                                 : GE_SkyTransmittanceToTop(uTransLUT, camR, geometry.SinAltitude,
                                                            uAtmos.planetRadius, uAtmos.atmosphereRadius);
    float transmittanceY = dot(transmittance, vec3(0.2126, 0.7152, 0.0722));
    if (transmittanceY <= 1e-6)
    {
        Cull();
        return;
    }

    float skyNits = isOrtho ? 0.0 : SkyLuminanceBehind(geometry.LutDirection);

    // The twinkle (scintillation): three beats per star, its amplitude growing with the airmass.
    float beat = uSky.skyTimeSeconds * uSky.starModel.w * (0.7 + 0.6 * fract(seed * 17.13));
    float phase = seed * 6.2831853;
    float flicker = (0.15 * sin(beat * 2.3 + phase) + 0.08 * sin(beat * 5.7 + phase * 1.7) +
                     0.04 * sin(beat * 11.0 + phase * 2.9)) / 0.27;
    flicker = 1.0 + flicker * GE_StarScintillationAmplitude(geometry.SinAltitude, uSky.starModel.z);

    GE_StarShapeDials shape;
    shape.CoreScale = uSky.starShape.x;
    shape.Spikes = uSky.starShape.y;
    shape.SpikeLength = uSky.starShape.z;
    shape.Shape = uSky.starShape.w;
    shape.Eye = uSky.skyboxParams.w;
    GE_StarDrawn drawn = GE_StarShade(magnitude, starColor, transmittance, skyNits, uSky.starModel.x,
                                      uSky.starModel.y, uSky.skyboxParams.z, shape, flicker, geometry.PixelSolidAngle);

    // The moon's disc hides the stars behind it.
    if (uSky.moonIntensity > 0.0 && !isOrtho)
    {
        float moonDot = dot(starDir, normalize(uSky.moonDirectionWS));
        float moonRadius = max(uSky.moonAngularRadius, 0.0);
        drawn.Visibility *= 1.0 - smoothstep(cos(moonRadius * 1.25), cos(moonRadius * 0.95), moonDot);
    }
#ifdef GE_SKY_HEIGHT_FOG
    drawn.Visibility *= 1.0 - GE_HeightFogSkyAmount(starDir);
#endif
    if (drawn.Visibility <= 0.0)
    {
        Cull();
        return;
    }
    vCorePeak = drawn.CorePeak * drawn.Visibility;
    vGlareScale = drawn.GlareScale * drawn.Visibility;
    vGlowPeak = drawn.GlowPeak * drawn.Visibility;
    vGlowRadius = drawn.GlowRadius;
    vCoreRadius = drawn.CoreRadius;
    vSpikePeakAndLength = vec4(drawn.SpikePeak * drawn.Visibility, drawn.SpikeLength);
    vShape = drawn.Shape;
    float cutRadius = drawn.CutRadius;

    // The quad covers the glare's cut radius, at least three core radii (the Gaussian is 1e-4 of its
    // peak there) and never less than a pixel and a half, so a faint star always rasterises.
    cutRadius = max(cutRadius, 1.5);
    vec2 corner = QuadOffset(uint(gl_VertexIndex) % 6u);
    vPixelOffset = corner * cutRadius;
    vCutRadius = cutRadius;

    float aspect = max(uSky.viewportAspect, 0.001);
    vec2 centreNdc = vec2(viewDir.x / (aspect * tanHalf), viewDir.y / tanHalf) / viewDir.z;
    vec2 pixelToNdc = 2.0 / max(uSky.viewportResolution, vec2(1.0));
    gl_Position = vec4(centreNdc + corner * cutRadius * pixelToNdc, 0.5, 1.0);
}
