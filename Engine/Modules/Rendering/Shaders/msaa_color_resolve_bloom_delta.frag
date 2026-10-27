#version 450

// Resolves the multisampled scene color like msaa_color_resolve.frag, and writes a second output:
// the plain sample mean minus the weighted mean. Bloom adds it back, so its input is the plain mean.

layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oBloomSampleDelta;
layout(set = 0, binding = 0) uniform sampler2DMS uSamples;

#include "Includes/msaa_weighted_resolve.glsl"

void main()
{
    vec4 plain;
    GE_MsaaResolveBoth(uSamples, ivec2(gl_FragCoord.xy), oColor, plain);
    oBloomSampleDelta = vec4(plain.rgb - oColor.rgb, 0.0);
}
