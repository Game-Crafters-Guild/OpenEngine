#version 450

// The backdrop of a view whose sky draws nothing: its clear colour, stored at the frame's
// pre-exposure P like every other radiance written into scene colour (ClearColorBackdrop.cpp).
// A clear value is a constant the CPU cannot scale, because P lives on the GPU.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0, std140) uniform ClearColorBlock
{
    vec4 clearColor; // the view's clear colour, scene-linear (1.0 = 203 nits)
} uClear;

// The view's exposure state (ExposureHistory).
#define GE_EXPOSURE_HISTORY_BINDING 1
#include "Includes/exposure_history.glsl"

void main()
{
    oColor = vec4(uClear.clearColor.rgb * GE_PreExposure(), uClear.clearColor.a);
}
