#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

layout(push_constant) uniform LdrFxPC {
    float casStrength;
    int casStackOrder;
    float vignetteIntensity;
    float vignetteSmoothness;
    int vignetteRounded;
    float vignetteColorR;
    float vignetteColorG;
    float vignetteColorB;
    int vignetteStackOrder;
    float shaderAnimationTime;
    float pad0;
} pc;

float Luma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 ApplyCas(vec3 c, vec2 texel)
{
    float w = clamp(pc.casStrength, 0.0, 1.0);
    if (w <= 1e-5)
        return c;

    vec3 n = texture(uSceneColor, vUV + vec2(0.0, -texel.y)).rgb;
    vec3 s = texture(uSceneColor, vUV + vec2(0.0,  texel.y)).rgb;
    vec3 e = texture(uSceneColor, vUV + vec2( texel.x, 0.0)).rgb;
    vec3 wv = texture(uSceneColor, vUV + vec2(-texel.x, 0.0)).rgb;
    vec3 ne = texture(uSceneColor, vUV + vec2( texel.x, -texel.y)).rgb;
    vec3 nw = texture(uSceneColor, vUV + vec2(-texel.x, -texel.y)).rgb;
    vec3 se = texture(uSceneColor, vUV + vec2( texel.x,  texel.y)).rgb;
    vec3 sw = texture(uSceneColor, vUV + vec2(-texel.x,  texel.y)).rgb;

    vec3 avgCardinal = (n + s + e + wv) * 0.25;
    vec3 avgDiagonal = (ne + nw + se + sw) * 0.25;
    vec3 avg = mix(avgCardinal, avgDiagonal, 0.35);
    vec3 detail = c - avg;

    float lC = Luma(c);
    float lN = Luma(n);
    float lS = Luma(s);
    float lE = Luma(e);
    float lW = Luma(wv);
    float lNE = Luma(ne);
    float lNW = Luma(nw);
    float lSE = Luma(se);
    float lSW = Luma(sw);
    float lMin = min(min(lC, min(min(lN, lS), min(lE, lW))), min(min(lNE, lNW), min(lSE, lSW)));
    float lMax = max(max(lC, max(max(lN, lS), max(lE, lW))), max(max(lNE, lNW), max(lSE, lSW)));
    float contrast = max(lMax - lMin, 1e-4);

    float adaptive = 1.0 - smoothstep(0.08, 0.40, contrast);
    float gain = w * mix(2.8, 0.75, 1.0 - adaptive);
    return max(c + detail * gain, vec3(0.0));
}

// URP-exact procedural vignette (UberPost.hlsl), applied on display-referred LDR color.
vec3 ApplyVignette(vec3 c)
{
    float w = clamp(pc.vignetteIntensity, 0.0, 1.0);
    if (w <= 1e-5)
        return c;

    vec2 sz = vec2(textureSize(uSceneColor, 0));
    float aspect = sz.x / max(sz.y, 1.0);
    vec2 d = abs(vUV - vec2(0.5)) * (w * 3.0);
    d.x *= (pc.vignetteRounded != 0) ? aspect : 1.0;
    float vf = pow(clamp(1.0 - dot(d, d), 0.0, 1.0), clamp(pc.vignetteSmoothness, 0.0, 1.0) * 5.0);
    vec3 vigColor = vec3(pc.vignetteColorR, pc.vignetteColorG, pc.vignetteColorB);
    return c * mix(vigColor, vec3(1.0), vf);
}

void main()
{
    vec4 src = texture(uSceneColor, vUV);
    vec2 texel = 1.0 / vec2(textureSize(uSceneColor, 0));

    float casW = clamp(pc.casStrength, 0.0, 1.0);
    float vigW = clamp(pc.vignetteIntensity, 0.0, 1.0);

    // CAS and the vignette in stack order, a tie running CAS first (at the defaults, casStackOrder 1
    // and vignetteStackOrder 0, the vignette runs before the sharpen). The LUT is not here: it runs
    // inside the Tonemap pass, before every display-referred effect.
    bool vignetteFirst = pc.vignetteStackOrder < pc.casStackOrder;
    vec3 rgb = src.rgb;
    if (vignetteFirst && vigW > 1e-5)
        rgb = ApplyVignette(rgb);
    if (casW > 1e-5)
        rgb = ApplyCas(rgb, texel);
    if (!vignetteFirst && vigW > 1e-5)
        rgb = ApplyVignette(rgb);

    oColor = vec4(rgb, src.a);
}
