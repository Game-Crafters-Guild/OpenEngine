#version 450

// A star on screen (sky_star_model.glsl): its core (or the sparkle), its glare, its glow and its
// spikes, at the radiance sky_stars_billboard.vert computed, added to scene colour at the frame's
// pre-exposure, and its luminance added to the star light (SkyStarLight), which the tonemap's night
// tint reads to keep the stars' colour. The sparkle's points are narrower than a pixel, so it is averaged over four taps of a
// rotated grid in the pixel; the round shape is smooth at the pixel scale and takes one.

#include "Includes/sky_star_model.glsl"

layout(location = 0) in vec2 vPixelOffset;
layout(location = 1) flat in vec3 vCorePeak;
layout(location = 2) flat in float vCoreRadius;
layout(location = 3) flat in vec3 vGlareScale;
layout(location = 4) flat in float vCutRadius;
layout(location = 5) flat in vec3 vGlowPeak;
layout(location = 6) flat in float vGlowRadius;
layout(location = 7) flat in vec4 vSpikePeakAndLength;
layout(location = 8) flat in float vShape;

layout(location = 0) out vec4 oColor;
layout(location = 1) out float oStarLight;

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
    vec4  starModel;
    vec4  skyboxParams;
} uSky;

// The view's exposure state (ExposureHistory): scene colour is stored at the frame's pre-exposure
// P, so the star's additive radiance is multiplied by P, with the sky's exposure trim.
#define GE_EXPOSURE_HISTORY_BINDING 2
#include "Includes/exposure_history.glsl"

// The stars do not take the exposure compensation curve: the curve darkens a night's sky and ground,
// and the stars keep the brightness the metered exposure gives them, so they stand on the darker sky.
// starsUndoCompensation is 1 while the view's Auto exposure runs its resolve (the state is current).
#define GE_EXPOSURE_COMPENSATION_BINDING 6
#include "Includes/exposure_compensation.glsl"

void main()
{
    if (length(vPixelOffset) >= vCutRadius)
        discard;
    GE_StarDrawn star;
    star.CorePeak = vCorePeak;
    star.CoreRadius = vCoreRadius;
    star.GlareScale = vGlareScale;
    star.CutRadius = vCutRadius;
    star.GlowPeak = vGlowPeak;
    star.GlowRadius = vGlowRadius;
    star.SpikePeak = vSpikePeakAndLength.rgb;
    star.SpikeLength = vSpikePeakAndLength.w;
    star.Shape = vShape;
    vec3 radiance;
    if (vShape >= 0.5)
    {
        radiance = vec3(0.0);
        for (int i = 0; i < 4; ++i)
            radiance += GE_StarRadianceAt(vPixelOffset + kStarPixelTaps[i], star);
        radiance *= 0.25;
    }
    else
    {
        radiance = GE_StarRadianceAt(vPixelOffset, star);
    }
    float undo = uSky.starsUndoCompensation > 0.5 ? GE_ExposureCompensationUndo() : 1.0;
    vec3 stored = radiance * exp2(uSky.exposureEV) * GE_PreExposure() * undo;
    oColor = vec4(stored, 0.0);
    oStarLight = dot(stored, vec3(0.2126, 0.7152, 0.0722));
}
