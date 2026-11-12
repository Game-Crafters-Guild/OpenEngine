// The linear depth in the view of ViewParams of a position relative to the render origin of the pass camera
// (Cam), the frame of vPosRel. Needs Cam (camera_relative.glsl) and ViewParams (view_params.glsl) in scope.

#ifndef GE_VIEW_LINEAR_DEPTH_GLSL
#define GE_VIEW_LINEAR_DEPTH_GLSL

// A shadow pass draws from the light's sector; the whole-sector delta, exact in fp32, moves the position into
// the view's frame. In a pass of the view itself the delta is zero and the position is unchanged.
float GE_ViewLinearDepth(vec3 passPositionRel)
{
    vec3 sectorDelta = vec3(Cam.uRenderOriginSector.xyz - ivec3(ge_renderOrigin.xyz)) * GE_SECTOR_SIZE;
    return (ge_viewRel * vec4(passPositionRel + sectorDelta, 1.0)).z;
}

#endif // GE_VIEW_LINEAR_DEPTH_GLSL
