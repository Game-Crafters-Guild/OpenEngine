#ifndef GE_REVERSE_Z_GLSL
#define GE_REVERSE_Z_GLSL

// The engine's depth convention, shader side: reverse-Z. NDC depth is [0, 1], the near plane maps
// to 1.0, the far plane to 0.0, and the nearer surface has the larger depth. The C++ twin is
// Rendering/Core/ReverseZ.h. A shader that rebuilds position through an inverse projection needs
// none of this; one that tests for the background or linearizes depth uses these.

// The depth a depth attachment clears to (ReverseZ::kClearFar).
const float kReverseZClearFar = 0.0;

// True where nothing was drawn: the depth is the cleared far plane. Sky never writes depth, the
// clear is exactly 0.0, and depth copies and resolves keep the value exactly, so no epsilon applies.
bool IsBackgroundDepth(float rawDepth)
{
    return rawDepth <= kReverseZClearFar;
}

// Reverse-Z perspective depth (near = 1.0, far = 0.0) -> positive view-space distance in meters.
// nearFar is the view's (near, far) pair: pass ge_nearFar.xy from the ViewParams block the shader
// declares, named or anonymous.
float LinearizeReverseZ(float rawDepth, vec2 nearFar)
{
    float zNear = max(nearFar.x, 1e-5);
    float zFar = max(nearFar.y, zNear + 1.0);
    return (zNear * zFar) / max(rawDepth * (zFar - zNear) + zNear, 1e-6);
}

#endif
