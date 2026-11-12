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

// Raw reverse-Z depth -> positive view-space distance in meters, exact for a perspective
// (finite or infinite) and an orthographic projection. depthTerms is ge_depthTerms, the
// projection's (m22, m32, m23, m33). Branch on IsBackgroundDepth first where the sky needs a value.
float LinearizeReverseZ(float rawDepth, vec4 depthTerms)
{
    return (depthTerms.y - rawDepth * depthTerms.w) / (rawDepth * depthTerms.z - depthTerms.x);
}

#endif
