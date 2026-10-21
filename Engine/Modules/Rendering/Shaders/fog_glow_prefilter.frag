#version 450


layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;
// The view's exposure state: scene colour is stored at the frame's pre-exposure P, while the
// fog glow threshold, knee and contribution cap are absolute scene-linear values, so the
// brightness they are compared with is divided by P. The extracted glow stays in stored units.
#define GE_EXPOSURE_HISTORY_BINDING 2
#include "Includes/exposure_history.glsl"
layout(set = 0, binding = 3, std140) uniform ViewParamsBlock
{
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#define GE_VIEWPARAMS_CORE_ONLY 1
#include "Includes/view_params_fields.glsl"
#undef GE_VIEWPARAMS_CORE_ONLY
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;
layout(set = 0, binding = 4, std140) uniform HeightFogParamsBlock
{
#include "Includes/height_fog_params_fields.glsl"
} HeightFogParams;
// What the sky pass draws its backdrop from; read only when the fog's colour comes from the sky.
layout(set = 0, binding = 5) uniform sampler2D uSkyBackdrop;
#include "Includes/fog_glow_layer.glsl"

layout(push_constant) uniform FogGlowPrefilterPC
{
    float fogGlowThreshold;
    float fogGlowKnee;
    float fogGlowFadeStart;
    float fogGlowFadeEnd;
    float fogGlowTintR;
    float fogGlowTintG;
    float fogGlowTintB;
    int fogGlowAntiFlicker;
} pc;

// Cross-median stabilization is adapted from KinoBloom v2 by Keijiro
// Takahashi. Copyright (c) 2015-2017, MIT License.
// See ThirdParty/KinoBloom/LICENSE.md and UPSTREAM.md.
vec3 Median3(vec3 a, vec3 b, vec3 c)
{
    return a + b + c - min(min(a, b), c) - max(max(a, b), c);
}

// The fog's own light (GE_FogGlowLayer), faded in by its opacity, so opaque scene edges never seed
// the multiscale veil.
vec3 FogVeil(vec2 uv)
{
    vec4 layer = GE_FogGlowLayer(uv);
    float fade = smoothstep(pc.fogGlowFadeStart,
                            max(pc.fogGlowFadeEnd, pc.fogGlowFadeStart + 1e-4),
                            layer.a);
    return min(max(layer.rgb, vec3(0.0)), vec3(65000.0)) * fade;
}

void main()
{
    vec3 source;
    if (pc.fogGlowAntiFlicker != 0)
    {
        vec2 texel = 1.0 / vec2(textureSize(uDepth, 0));
        vec3 c = FogVeil(vUV);
        vec3 l = FogVeil(vUV - vec2(texel.x, 0.0));
        vec3 r = FogVeil(vUV + vec2(texel.x, 0.0));
        vec3 d = FogVeil(vUV - vec2(0.0, texel.y));
        vec3 u = FogVeil(vUV + vec2(0.0, texel.y));
        source = Median3(Median3(c, l, r), d, u);
    }
    else
    {
        source = FogVeil(vUV);
    }

    float brightness = max(max(source.r, source.g), source.b) / GE_PreExposure();
    float knee = max(pc.fogGlowKnee, 1e-5);
    float soft = clamp((brightness - pc.fogGlowThreshold + knee) / (2.0 * knee), 0.0, 1.0);
    soft = soft * soft * knee;
    float contribution = min(max(brightness - pc.fogGlowThreshold, soft), 100.0);
    vec3 tint = max(vec3(pc.fogGlowTintR, pc.fogGlowTintG, pc.fogGlowTintB), vec3(0.0));
    oColor = vec4(source * (contribution / max(brightness, 1e-6)) * tint, 1.0);
}
