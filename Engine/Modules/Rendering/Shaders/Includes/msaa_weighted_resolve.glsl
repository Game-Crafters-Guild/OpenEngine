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

// Writes the weighted mean to `weighted` and the plain mean of the same samples to `plain`.
// GE_MsaaWeightedResolve keeps its own loop, so its compiled code does not depend on this function.
void GE_MsaaResolveBoth(sampler2DMS samples, ivec2 pixel, out vec4 weighted, out vec4 plain)
{
    int count = textureSamples(samples);
    vec4 sum = vec4(0.0);
    vec4 plainSum = vec4(0.0);
    float weightSum = 0.0;
    for (int i = 0; i < count; ++i)
    {
        vec4 sampleColor = texelFetch(samples, pixel, i);
        sampleColor.rgb = Sanitize(sampleColor.rgb);
        float sampleWeight = GE_KarisWeight(sampleColor.rgb, 1.0);
        sum += sampleColor * sampleWeight;
        plainSum += sampleColor;
        weightSum += sampleWeight;
    }
    weighted = sum / weightSum;
    plain = plainSum / float(count);
}

#endif // GE_MSAA_WEIGHTED_RESOLVE_GLSL
