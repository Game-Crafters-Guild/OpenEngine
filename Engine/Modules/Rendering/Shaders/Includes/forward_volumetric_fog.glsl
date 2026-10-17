// Volumetric fog for what a forward pass draws over the already fogged scene (GE_FOG): a fragment
// fogged by its own distance from the camera, read from the froxel volume the VolumetricFog
// composite fogs the opaque scene from, so the two agree where they meet.
//
// The composite reads the volume at the opaque surface's distance, so a transparent fragment drawn
// before it is fogged as far away as what lies behind it, and over the sky as far as the fog goes.
// Transparent draws that fog themselves therefore draw after it (the PostFogTransparent node).
//
// The fog's parameters are the VolumetricFogParams buffer and its volume the VolumetricFog texture
// the VolumetricFog node publishes for the view (bound by name; a view without the fog binds a
// zero block, which fogs nothing). Past the volume's far plane the fog continues analytically, as
// the composite's does (volumetric_fog_far.glsl), lit by the IBL irradiance cube.
#ifndef GE_FORWARD_VOLUMETRIC_FOG_GLSL
#define GE_FORWARD_VOLUMETRIC_FOG_GLSL

#include "compat_profile.glsl"
#include "view_params.glsl"

layout(set = 0, binding = 54, std140) uniform VolumetricFogParamsBlock
{
#include "volumetric_fog_params_fields.glsl"
} VolumetricFogParams;

layout(set = 0, binding = 55) uniform sampler3D ge_volumetricFog;
layout(set = 0, binding = 56) uniform samplerCube ge_volumetricFogIrradiance;

#define GE_VOLUMETRIC_FOG VolumetricFogParams
#define GE_VOLUMETRIC_FOG_IRRADIANCE ge_volumetricFogIrradiance
// Explicit-level reads throughout: the fog applies after a surface may have discarded, where an
// implicit derivative is undefined. Both textures have one level.
#define GE_VOLUMETRIC_FOG_IRRADIANCE_TAP(s, coords) textureLod(s, coords, 0.0)
#include "volumetric_fog_view.glsl"
#include "volumetric_fog_phase.glsl"
#include "volumetric_fog_far.glsl"

// The volume at a fragment's slice: the composite's five-tap filter (volumetric_fog_composite.frag,
// FogBilateralIntegrated) as it reads a surface that stands at the same distance at every tap.
vec4 GE_VolumetricFogAt(vec2 uv, float slice01)
{
    vec2 fogTexel = 1.0 / max(VolumetricFogParams.uGridParams.xy, vec2(1.0));
    vec2 offsets[5] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
    vec4 sum = vec4(0.0);
    float weightSum = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        vec2 sampleUv = clamp(uv + offsets[i] * fogTexel, vec2(0.0), vec2(1.0));
        float w = (i == 0) ? 1.0 : 0.55;
        sum += textureLod(ge_volumetricFog, vec3(sampleUv, slice01), 0.0) * w;
        weightSum += w;
    }
    return sum / max(weightSum, 1.0e-4);
}

// color seen through the volumetric fog between the camera and positionWS: dimmed to the fog's
// transmittance, with the light the fog scatters in added as far as the fragment hides what is
// behind it, by scatterWeight (as GE_ApplyHeightFog's).
vec3 GE_ApplyVolumetricFog(vec3 color, vec3 positionWS, float scatterWeight)
{
    if (VolumetricFogParams.uVolumeParams.x <= 0.0)
        return color;

    vec2 uv = clamp(gl_FragCoord.xy * ge_screenSize.zw, vec2(0.0), vec2(1.0));
    float distanceToCamera = max(FogViewDistanceFromWorld(uv, positionWS) + VolumetricFogParams.uFogExtraParams.x, 0.0);
    vec4 fogSample = GE_VolumetricFogAt(uv, clamp(FogDistanceToSlice01(distanceToCamera), 0.0, 1.0));
    if (VolumetricFogParams.uHeightParams.w > 0.5 && distanceToCamera > VolumetricFogParams.uVolumeParams.x)
        fogSample = FogExtendBeyondGrid(fogSample, uv, distanceToCamera);
    return color * fogSample.a + fogSample.rgb * scatterWeight;
}

#endif // GE_FORWARD_VOLUMETRIC_FOG_GLSL
