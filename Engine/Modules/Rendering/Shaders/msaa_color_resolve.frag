#version 450

// Resolves the multisampled scene color into its single-sample target with the
// Karis-weighted mean (Includes/msaa_weighted_resolve.glsl). MsaaColorResolvePass.cpp
// declares it after every pass that draws into the multisampled scene color.

layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2DMS uSamples;

#include "Includes/msaa_weighted_resolve.glsl"

void main()
{
    oColor = GE_MsaaWeightedResolve(uSamples, ivec2(gl_FragCoord.xy));
}
