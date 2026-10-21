// The volumetric fog's view geometry: where a screen position and a distance put a
// point, and which froxel slice a distance falls in. Shared by the VolumetricFog
// module's passes (volumetric_fog_common.glsl) and the forward passes that fog what
// they draw (volumetric_fog_forward.glsl). The includer declares the parameter block
// and names its instance GE_VOLUMETRIC_FOG.
#ifndef GE_VOLUMETRIC_FOG_VIEW_GLSL
#define GE_VOLUMETRIC_FOG_VIEW_GLSL

vec2 FogUvToNdc(vec2 uv)
{
    return vec2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
}

vec2 FogNdcToUv(vec2 ndc)
{
    return vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
}

float FogSliceDistance(float slice01)
{
    return GE_VOLUMETRIC_FOG.uVolumeParams.x * pow(clamp(slice01, 0.0, 1.0), max(GE_VOLUMETRIC_FOG.uVolumeParams.y, 0.05));
}

float FogDistanceToSlice01(float distanceToCamera)
{
    return pow(clamp(distanceToCamera / max(GE_VOLUMETRIC_FOG.uVolumeParams.x, 0.001), 0.0, 1.0), 1.0 / max(GE_VOLUMETRIC_FOG.uVolumeParams.y, 0.05));
}

vec3 FogWorldRayFromUv(vec2 uv)
{
    vec4 nearPoint = GE_VOLUMETRIC_FOG.uInvViewProj * vec4(FogUvToNdc(uv), 1.0, 1.0);
    vec4 farPoint = GE_VOLUMETRIC_FOG.uInvViewProj * vec4(FogUvToNdc(uv), 0.0, 1.0);
    nearPoint.xyz /= max(nearPoint.w, 1.0e-6);
    farPoint.xyz /= max(farPoint.w, 1.0e-6);
    if (GE_VOLUMETRIC_FOG.uDensityParams.z > 0.5)
        return normalize(farPoint.xyz - nearPoint.xyz);
    return normalize(farPoint.xyz - GE_VOLUMETRIC_FOG.uCameraPos.xyz);
}

vec3 FogRayOriginFromUv(vec2 uv)
{
    if (GE_VOLUMETRIC_FOG.uDensityParams.z <= 0.5)
        return GE_VOLUMETRIC_FOG.uCameraPos.xyz;

    vec4 nearPoint = GE_VOLUMETRIC_FOG.uInvViewProj * vec4(FogUvToNdc(uv), 1.0, 1.0);
    return nearPoint.xyz / max(nearPoint.w, 1.0e-6);
}

vec3 FogWorldFromUvDistance(vec2 uv, float distanceToCamera)
{
    return FogRayOriginFromUv(uv) + FogWorldRayFromUv(uv) * distanceToCamera;
}

vec3 FogWorldFromDepth(vec2 uv, float rawDepth)
{
    vec4 p = GE_VOLUMETRIC_FOG.uInvViewProj * vec4(FogUvToNdc(uv), rawDepth, 1.0);
    return p.xyz / max(p.w, 1.0e-6);
}

float FogViewDistanceFromWorld(vec2 uv, vec3 worldPos)
{
    if (GE_VOLUMETRIC_FOG.uDensityParams.z > 0.5)
        return max(dot(worldPos - FogRayOriginFromUv(uv), FogWorldRayFromUv(uv)), 0.0);
    return length(worldPos - GE_VOLUMETRIC_FOG.uCameraPos.xyz);
}

#endif // GE_VOLUMETRIC_FOG_VIEW_GLSL
