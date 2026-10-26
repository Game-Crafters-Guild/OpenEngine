#ifndef GE_REVERSE_Z_GLSL
#define GE_REVERSE_Z_GLSL

// The shader side of the reverse-Z depth convention. Rendering/Core/ReverseZ.h owns the convention.
// A shader that tests for the background or linearizes depth uses these helpers.

// The depth a depth attachment clears to (ReverseZ::kClearFar).
const float kReverseZClearFar = 0.0;

// True where nothing was drawn: the depth is the cleared far plane. Sky never writes depth, the
// clear is exactly 0.0, and depth copies and resolves keep the value exactly, so no epsilon applies.
bool IsBackgroundDepth(float rawDepth)
{
    return rawDepth <= kReverseZClearFar;
}

// Raw reverse-Z perspective depth -> positive view-space distance in meters.
// nearFar is the view's (near, far) pair: pass ge_nearFar.xy from the ViewParams block the shader
// declares, named or anonymous.
float LinearizeReverseZ(float rawDepth, vec2 nearFar)
{
    float zNear = max(nearFar.x, 1e-5);
    float zFar = max(nearFar.y, zNear + 1.0);
    return (zNear * zFar) / max(rawDepth * (zFar - zNear) + zNear, 1e-6);
}

#endif
