// The volumetric fog in front of a surface, by the surface's own distance: read from the froxel
// volume the VolumetricFog composite fogs the opaque scene from, at the surface's own slice, and
// continued past the volume as the composite continues it, so the two agree where they meet.
// forward_fog.glsl includes it for a shading variant compiled with the VolumetricFog pass keyword.
//
// The fog's parameters are the VolumetricFogParams block and its volume the VolumetricFog texture
// that the VolumetricFog node publishes for the view; both are bound by name. In-scatter is
// absolute scene-linear radiance, like the height fog term's.
#ifndef GE_VOLUMETRIC_FOG_FORWARD_GLSL
#define GE_VOLUMETRIC_FOG_FORWARD_GLSL

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

/// The volumetric fog between the camera and a surface.
/// @param uv         the surface's screen position in [0, 1].
/// @param positionWS the surface's world position.
/// @return rgb: the in-scattered radiance, absolute scene-linear; a: the transmittance in [0, 1].
///         A view whose fog has no volume (maxDistance 0) returns (0, 0, 0, 1).
vec4 GE_VolumetricFogForward(vec2 uv, vec3 positionWS)
{
    if (VolumetricFogParams.uVolumeParams.x <= 0.0)
        return vec4(0.0, 0.0, 0.0, 1.0);
    float distanceToCamera =
        max(FogViewDistanceFromWorld(uv, positionWS) + VolumetricFogParams.uFogExtraParams.x, 0.0);
    // One trilinear tap at the surface's own slice. The composite filters five taps around a pixel
    // because its depth changes from tap to tap; a surface's own distance is one distance.
    vec4 fogSample = textureLod(ge_volumetricFog, vec3(uv, clamp(FogDistanceToSlice01(distanceToCamera), 0.0, 1.0)), 0.0);
    if (VolumetricFogParams.uHeightParams.w > 0.5 && distanceToCamera > VolumetricFogParams.uVolumeParams.x)
        fogSample = FogExtendBeyondGrid(fogSample, uv, distanceToCamera);
    return fogSample;
}

#endif // GE_VOLUMETRIC_FOG_FORWARD_GLSL
