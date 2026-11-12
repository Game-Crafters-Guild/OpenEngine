// The sun's cascade shadow under water: the god rays and the caustics on submerged receivers read it
// (ocean_underwater.frag). Declares the cascade block at set 0 binding 8 and the cascade array at binding 9.
// Needs ViewParams (view_params.glsl) in scope: the cascade matrices take positions relative to the view's origin.

#ifndef OCEAN_GOD_RAY_SHADOW_GLSL
#define OCEAN_GOD_RAY_SHADOW_GLSL

#include "Includes/shadow_cascade_blend.glsl"
#include "Includes/view_origin.glsl"

layout(set = 0, binding = 8, std140) uniform ShadowData
{
    mat4 ge_shadowVP[4];
    vec4 ge_shadowSplits;
    vec4 ge_shadowParams; // x=depthBias, y=normalBias, z=numCascades, w=maxShadowDistance
    vec4 ge_shadowDebug;
    vec4 ge_shadowPcss;
    vec4 ge_shadowPcssCascades[4];
    vec4 ge_shadowPcssPyramid[4];
};

layout(set = 0, binding = 9) uniform sampler2DArrayShadow ge_shadowMapArray;

bool GodRayShadowUVInBounds(vec2 uv)
{
    return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

float GodRayShadowCompareTap(vec2 uv, float layer, float refDepth)
{
    if (!GodRayShadowUVInBounds(uv))
        return 1.0;
    return texture(ge_shadowMapArray, vec4(uv, layer, refDepth));
}

float SampleGodRayCascade(vec3 posWS, int cascadeIdx)
{
    vec4 shadowClip = ge_shadowVP[cascadeIdx] * vec4(posWS - GE_ViewOrigin(), 1.0);
    vec3 shadowNDC = shadowClip.xyz / max(abs(shadowClip.w), 1.0e-5);
    vec2 shadowUV = shadowNDC.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;

    float refDepthSpan = max(ge_shadowPcssCascades[0].y, 1.0e-3);
    float thisDepthSpan = max(ge_shadowPcssCascades[cascadeIdx].y, 1.0e-3);
    float biasScale = refDepthSpan / thisDepthSpan;
    float refDepth = shadowNDC.z + ge_shadowParams.x * biasScale;

    if (!GodRayShadowUVInBounds(shadowUV) || refDepth < 0.0 || refDepth > 1.0)
        return 1.0;

    float texelSize = 1.0 / float(textureSize(ge_shadowMapArray, 0).x);
    float layer = float(cascadeIdx);
    float shadow = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            shadow += GodRayShadowCompareTap(shadowUV + offset, layer, refDepth);
        }
    }
    return shadow * (1.0 / 9.0);
}

float SampleGodRayShadow(vec3 posWS, float linearDepth)
{
    int numCascades = int(ge_shadowParams.z + 0.5);
    if (numCascades <= 0 || linearDepth > ge_shadowParams.w)
        return 1.0;

    numCascades = clamp(numCascades, 1, 4);
    int cascadeIdx = numCascades - 1;
    for (int i = 0; i < numCascades; ++i)
    {
        if (linearDepth < ge_shadowSplits[i])
        {
            cascadeIdx = i;
            break;
        }
    }

    if (cascadeIdx < numCascades - 1)
    {
        float splitDist = ge_shadowSplits[cascadeIdx];
        float cascadeStart = (cascadeIdx == 0) ? 0.0 : ge_shadowSplits[cascadeIdx - 1];
        float cascadeRange = max(splitDist - cascadeStart, 1.0e-3);
        float blendBand = GE_CascadeBlendBand(cascadeRange);
        float distToSplit = splitDist - linearDepth;
        if (distToSplit < blendBand)
        {
            float t = smoothstep(0.0, 1.0, distToSplit / blendBand);
            float currentShadow = SampleGodRayCascade(posWS, cascadeIdx);
            float nextShadow = SampleGodRayCascade(posWS, cascadeIdx + 1);
            return mix(nextShadow, currentShadow, t);
        }
    }

    return SampleGodRayCascade(posWS, cascadeIdx);
}

#endif // OCEAN_GOD_RAY_SHADOW_GLSL
