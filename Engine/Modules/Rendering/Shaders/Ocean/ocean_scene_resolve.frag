#version 450

// Resolves the opaque color of the depth resolve's sample zero (depth_resolve.comp): only samples at
// its depth count, each with the Karis weight, so a partly covered object does not glow through the
// water's fog.
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2DMS uSrc;
layout(set = 0, binding = 1) uniform sampler2DMS uDepth;
layout(std140, set = 0, binding = 2) uniform ResolveParams { uvec4 options; };

#include "Includes/hdr_sanitize.glsl"
#include "Includes/karis_weight.glsl"

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int count = textureSamples(uSrc);
    float reference = options.x != 0u ? texelFetch(uDepth, p, 0).r : 0.0;
    float tolerance = max(reference * 0.005, 1e-6);
    vec4 color = vec4(0.0);
    float weight = 0.0;
    for (int i = 0; i < count; ++i)
    {
        float accept = options.x == 0u || abs(reference - texelFetch(uDepth, p, i).r) <= tolerance ? 1.0 : 0.0;
        vec4 sampleColor = texelFetch(uSrc, p, i);
        // An infinite sample would get weight 0 and give Inf * 0 = NaN.
        sampleColor.rgb = Sanitize(sampleColor.rgb);
        float sampleWeight = accept * GE_KarisWeight(sampleColor.rgb, 1.0);
        color += sampleColor * sampleWeight;
        weight += sampleWeight;
    }
    // Sample zero always matches its own depth, so weight is positive.
    outColor = color / weight;
}
