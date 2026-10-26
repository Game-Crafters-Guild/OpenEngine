#version 450

// Copies the multisampled scene color to a single-sample target with the Karis-weighted mean
// (Includes/msaa_weighted_resolve.glsl). The ocean grab and overlay passes read this copy and see
// the same edges as the scene color resolve. Pairs with ocean_fullscreen.vert.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2DMS uSrc;

#include "Includes/msaa_weighted_resolve.glsl"

void main()
{
    outColor = GE_MsaaWeightedResolve(uSrc, ivec2(gl_FragCoord.xy));
}
