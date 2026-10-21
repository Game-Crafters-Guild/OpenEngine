#version 450


// Folds the temporally-resolved cloud buffer (R=light energy,
// G=transmittance) over the scene: scene * transmittance + energy * sunColor
// (upstream Clouds.shader composite; the sun tint is applied here so the
// march/resolve stay scalar).
//
// The scene already carries the height fog: every surface took it in its own shader and the sky
// pass drew the sky fill. The cloud stands in front of what the pixel shows, so it takes the fog
// in front of that (height_fog_pixel.glsl), and the fog's light in front of what the cloud now
// hides is taken back by the cloud's opacity: scene * T + cloud * T_fog + S_fog * (1 - T), which is
// (unfogged scene * T + cloud) fogged by (S_fog, T_fog), the result of fogging after the clouds.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler2D uClouds;
// The view's exposure state (ExposureHistory). The scene is stored at the frame's
// pre-exposure P; the cloud in-scatter is absolute and joins it multiplied by P (the
// transmittance in G is a ratio and takes none).
#define GE_EXPOSURE_HISTORY_BINDING 3
#include "../../Rendering/Shaders/Includes/exposure_history.glsl"

layout(set = 0, binding = 2, std140) uniform CloudCompositeParamsBlock
{
    float radius;
    float altitude;
    float thickness;
    int numStepsLight;
    float stepSize;
    float rayOffsetStrength;
    float cloudScale;
    float densityMultiplier;
    float densityOffset;
    float shapeOffsetX;
    float shapeOffsetY;
    float shapeOffsetZ;
    float shapeWeightR;
    float shapeWeightG;
    float shapeWeightB;
    float shapeWeightA;
    float detailNoiseScale;
    float detailNoiseWeight;
    float detailWeightR;
    float detailWeightG;
    float detailWeightB;
    float detailOffsetX;
    float detailOffsetY;
    float detailOffsetZ;
    float lightAbsorptionThroughCloud;
    float lightAbsorptionTowardSun;
    float darknessThreshold;
    float phaseForward;
    float phaseBack;
    float phaseBase;
    float phaseFactor;
    float timeScale;
    float baseSpeed;
    float detailSpeed;
    float sunDirX;
    float sunDirY;
    float sunDirZ;
    float sunColorR;
    float sunColorG;
    float sunColorB;
    float sunIntensity;
    float historyWeight;
} CloudParams;

// The height fog in front of what each pixel shows (height_fog_pixel.glsl): the view depth the fog's
// distance comes from, the view's projection, the fog's parameters and the sky backdrop its colour
// can come from.
layout(set = 0, binding = 4) uniform sampler2D uDepth;
layout(set = 0, binding = 5, std140) uniform ViewParamsBlock
{
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#define GE_VIEWPARAMS_CORE_ONLY 1
#include "../../Rendering/Shaders/Includes/view_params_fields.glsl"
#undef GE_VIEWPARAMS_CORE_ONLY
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;
layout(set = 0, binding = 6, std140) uniform HeightFogParamsBlock
{
#include "../../Rendering/Shaders/Includes/height_fog_params_fields.glsl"
} HeightFogParams;
layout(set = 0, binding = 7) uniform sampler2D uSkyBackdrop;
#define GE_HEIGHT_FOG_BACKDROP uSkyBackdrop
#include "../../Rendering/Shaders/Includes/height_fog_pixel.glsl"

void main()
{
    vec4 scene = texture(uSceneColor, vUV);
    vec4 clouds = texture(uClouds, vUV);
    vec3 sunColor = vec3(CloudParams.sunColorR, CloudParams.sunColorG, CloudParams.sunColorB) *
                    CloudParams.sunIntensity;
    float cloudTransmittance = clamp(clouds.g, 0.0, 1.0);
    vec3 cloudRadiance = clouds.r * sunColor;
    vec3 fogScattering = vec3(0.0);
    float fogTransmittance = 1.0;
    // A pixel without a cloud keeps the scene as it is, so only a cloud's pixels evaluate the fog.
    if (cloudTransmittance < 1.0 || clouds.r > 0.0)
    {
        ivec2 depthSize = textureSize(uDepth, 0);
        ivec2 pixel = ivec2(clamp(vUV * vec2(depthSize), vec2(0.0), vec2(depthSize) - vec2(1.0)));
        GE_HeightFogAtPixel(vUV, texelFetch(uDepth, pixel, 0).r, fogScattering, fogTransmittance);
    }
    vec3 rgb = scene.rgb * cloudTransmittance +
               (cloudRadiance * fogTransmittance + fogScattering * (1.0 - cloudTransmittance)) * GE_PreExposure();
    oColor = vec4(max(rgb, vec3(0.0)), scene.a);
}
