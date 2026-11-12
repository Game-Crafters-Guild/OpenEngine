// Frustum tests over the six planes of Rendering::ExtractFrustumPlanes (left, right, bottom, top, near, far).
// A plane is (unit inward normal, d); p is inside when dot(n, p) + d >= 0. Reverse-Z: near = row3 - row2, far = row2.
// A test returns true only when the bound is wholly outside one plane.
#ifndef GE_FRUSTUM_TESTS_GLSL
#define GE_FRUSTUM_TESTS_GLSL

// The host tests compile this block as C++ (GlslShim.h): literals take `f` and no swizzles.
// GE_SHARED_FRUSTUM_TESTS_BEGIN
// The negation of Rendering::TestSphereFrustum (Rendering/Common/Frustum.h) without its 1.5x
// radius inflation: a kernel scales the radius by its own margin before the call.
bool SphereOutsideFrustum(vec3 center, float radius, vec4 planes[6])
{
    for (int i = 0; i < 6; ++i)
    {
        vec4 plane = planes[i];
        if (dot(center, vec3(plane.x, plane.y, plane.z)) + plane.w < -radius)
            return true;
    }
    return false;
}

// The negation of Rendering::TestAabbFrustum. The box corner farthest along each normal decides:
// when that corner is outside, the whole box is.
bool AabbOutsideFrustum(vec3 boxMin, vec3 boxMax, vec4 planes[6])
{
    for (int i = 0; i < 6; ++i)
    {
        vec4 plane = planes[i];
        float x = plane.x >= 0.0f ? boxMax.x : boxMin.x;
        float y = plane.y >= 0.0f ? boxMax.y : boxMin.y;
        float z = plane.z >= 0.0f ? boxMax.z : boxMin.z;
        if (dot(vec3(plane.x, plane.y, plane.z), vec3(x, y, z)) + plane.w < 0.0f)
            return true;
    }
    return false;
}
// GE_SHARED_FRUSTUM_TESTS_END

#endif
