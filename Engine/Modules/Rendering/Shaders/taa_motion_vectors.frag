#version 450

// TAA movers motion-vector pass, fragment stage: viewport-UV-space motion
// delta for the visible mover surface, in the one payload encoding both
// producers share (Includes/motion_vector_payload.glsl). The target is cleared
// to the sentinel; any written texel means "an exact per-instance MV exists
// here".

#include "Includes/motion_vector_payload.glsl"

layout(location = 0) in vec4 vCurrClip;
layout(location = 1) in vec4 vPrevClip;

layout(location = 0) out vec4 outMV;

void main()
{
    // No usable current surface: retain the unwritten sentinel. The previous
    // endpoint's guard is the encode's own.
    if (vCurrClip.w <= 1e-6)
    {
        outMV = GE_MOTION_VECTOR_SENTINEL;
        return;
    }
    outMV = GE_MotionVectorPayloadFromUv(GE_YUpNdcToViewportUV(vCurrClip.xy / vCurrClip.w),
                                         vPrevClip);
}
