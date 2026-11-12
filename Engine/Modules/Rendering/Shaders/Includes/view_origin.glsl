// The world position of the view's render origin, the origin of vPosRel, the LightBuffer and the local-light
// shadow matrices. It is a whole sector times a power of two, so it is exact in fp32; in sector (0,0,0) it is zero.
// Needs ge_renderOrigin (view_params.glsl) in scope.

#ifndef GE_VIEW_ORIGIN_GLSL
#define GE_VIEW_ORIGIN_GLSL

// The world position of the view's render origin, in meters.
vec3 GE_ViewOrigin()
{
    return ge_renderOrigin.xyz * ge_renderOrigin.w;
}

// The world position of a point relative to the view's render origin. Only for a reader that still
// works in world space; its precision is that of an fp32 world position.
vec3 GE_ViewAbsolutePoint(vec3 relativePoint)
{
    return relativePoint + GE_ViewOrigin();
}

#endif // GE_VIEW_ORIGIN_GLSL
