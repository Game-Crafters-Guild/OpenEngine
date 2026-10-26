// The Karis-weighted mean of a pixel's samples (karis_weight.glsl). A bright surface that covers
// one sample in eight gets one eighth of its tonemapped brightness. The result stays in the HDR
// space of the samples.
#ifndef GE_MSAA_WEIGHTED_RESOLVE_GLSL
#define GE_MSAA_WEIGHTED_RESOLVE_GLSL

#include "hdr_sanitize.glsl"
#include "karis_weight.glsl"

vec4 GE_MsaaWeightedResolve(sampler2DMS samples, ivec2 pixel)
{
    int count = textureSamples(samples);
    vec4 sum = vec4(0.0);
    float weightSum = 0.0;
    for (int i = 0; i < count; ++i)
    {
        vec4 sampleColor = texelFetch(samples, pixel, i);
        // An infinite sample would get weight 0 and give Inf * 0 = NaN.
        sampleColor.rgb = Sanitize(sampleColor.rgb);
        float sampleWeight = GE_KarisWeight(sampleColor.rgb, 1.0);
        sum += sampleColor * sampleWeight;
        weightSum += sampleWeight;
    }
    return sum / weightSum;
}

#endif // GE_MSAA_WEIGHTED_RESOLVE_GLSL
