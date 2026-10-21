#version 450

#include "Includes/compat_profile.glsl"

#include "volumetric_fog_common.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler3D uIntegrated;
layout(set = 0, binding = 1) uniform sampler2D uDepth;
// Same engine-shared irradiance cube the lighting pass uses, so the analytic
// far-fog in-scatter color matches the near (froxel) fog. Sampled only on the
// far-extension path, gated by fog.uDensityParams.w (iblEnabled).
layout(set = 0, binding = 3) uniform samplerCube uFogIrradianceCube;
// The view's exposure state (ExposureHistory). The froxels hold absolute in-scatter; the scene
// colour this pass blends onto (One, SrcAlpha) is stored at the frame's pre-exposure P, so the
// in-scatter is multiplied by P here (the transmittance in alpha is a ratio and takes none).
#define GE_EXPOSURE_HISTORY_BINDING 4
#include "Includes/exposure_history.glsl"

#define GE_VOLUMETRIC_FOG_IRRADIANCE uFogIrradianceCube
#include "Includes/volumetric_fog_far.glsl"

float FogSceneDistance(vec2 uv, ivec2 depthSize)
{
    ivec2 px = ivec2(clamp(uv * vec2(depthSize), vec2(0.0), vec2(depthSize - ivec2(1))));
    float rawDepth = texelFetch(uDepth, px, 0).r;
    vec3 worldPos = FogWorldFromDepth(uv, rawDepth);
    return max(FogViewDistanceFromWorld(uv, worldPos) + fog.uFogExtraParams.x, 0.0);
}

vec4 FogBilateralIntegrated(vec2 uv, float centerSlice01, float centerDistance, ivec2 depthSize)
{
    vec2 fogTexel = 1.0 / max(fog.uGridParams.xy, vec2(1.0));
    vec2 offsets[5] = vec2[](
        vec2(0.0, 0.0),
        vec2(1.0, 0.0),
        vec2(-1.0, 0.0),
        vec2(0.0, 1.0),
        vec2(0.0, -1.0));

    vec4 sum = vec4(0.0);
    float weightSum = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        vec2 sampleUv = clamp(uv + offsets[i] * fogTexel, vec2(0.0), vec2(1.0));
        float sampleDistance = FogSceneDistance(sampleUv, depthSize);
        float sampleSlice01 = FogDistanceToSlice01(sampleDistance);
        float depthWeight = exp(-abs(sampleSlice01 - centerSlice01) * max(fog.uGridParams.z * 0.65, 1.0));
        float screenWeight = (i == 0) ? 1.0 : 0.55;
        float distanceRatio = abs(sampleDistance - centerDistance) / max(centerDistance, 1.0);
        float edgeWeight = exp(-distanceRatio * 24.0);
        float w = screenWeight * max(depthWeight, edgeWeight * 0.35);
        sum += texture(uIntegrated, vec3(sampleUv, clamp(sampleSlice01, 0.0, 1.0))) * w;
        weightSum += w;
    }
    return sum / max(weightSum, 1.0e-4);
}

void main()
{
    ivec2 depthSize = textureSize(uDepth, 0);
    vec2 uv = clamp(vUV, vec2(0.0), vec2(1.0));
    float distanceToCamera = FogSceneDistance(uv, depthSize);
    float slice01 = FogDistanceToSlice01(distanceToCamera);

    vec4 fogSample = FogBilateralIntegrated(uv, clamp(slice01, 0.0, 1.0), distanceToCamera, depthSize);

    // Global fog only: extend analytically past the grid so distant geometry/sky
    // (beyond maxDistance, e.g. when the camera is zoomed out) stays fogged.
    if (fog.uHeightParams.w > 0.5 && distanceToCamera > fog.uVolumeParams.x)
        fogSample = FogExtendBeyondGrid(fogSample, uv, distanceToCamera);

    outColor = vec4(fogSample.rgb * GE_PreExposure(), fogSample.a);
}
