#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;

layout(set = 0, binding = 2, std140) uniform ViewParamsBlock
{
    // 160 B core prefix of the shared ViewParams list. HeightFog lives in its
    // own module, so the field list is reached by relative path into Rendering;
    // glslc resolves #include against the source file's directory.
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#define GE_VIEWPARAMS_CORE_ONLY 1
#include "../../Rendering/Shaders/Includes/view_params_fields.glsl"
#undef GE_VIEWPARAMS_CORE_ONLY
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;

layout(set = 0, binding = 3, std140) uniform HeightFogParamsBlock
{
    // Shared with the forward passes that fog what they draw; reached by relative path into
    // Rendering like the ViewParams field list above.
#include "../../Rendering/Shaders/Includes/height_fog_params_fields.glsl"
} HeightFogParams;

// What the sky pass draws its backdrop from (the sky-view LUT or the HDRI), published by the
// SkyRender node. Read only when colorSource names a backdrop.
layout(set = 0, binding = 4) uniform sampler2D uSkyBackdrop;

// The fog's sum, shared with the forward passes (height_fog_common.glsl), reading the backdrop above.
#define GE_HEIGHT_FOG_SKY_BACKDROP uSkyBackdrop
#include "../../Rendering/Shaders/Includes/height_fog_common.glsl"

void main()
{
    vec4 scene = texture(uSceneColor, vUV);
    float fogStrength = clamp(HeightFogParams.intensity, 0.0, 1.0);
    if (fogStrength <= 1e-5 || HeightFogParams.density <= 1e-5)
    {
        oColor = scene;
        return;
    }

    ivec2 depthSize = textureSize(uDepth, 0);
    ivec2 pix = ivec2(clamp(gl_FragCoord.xy, vec2(0.0), vec2(depthSize) - vec2(1.0)));
    float rawDepth = texelFetch(uDepth, pix, 0).r;
    bool sky = rawDepth <= 0.000001;

    float depthForRay = sky ? 0.0 : rawDepth;
    vec2 ndc = ViewportUvToNdc(vUV);
    vec4 pV = ViewParams.ge_invProj * vec4(ndc, depthForRay, 1.0);
    pV.xyz /= max(pV.w, 1e-6);
    float viewLength = sky ? max(ViewParams.ge_nearFar.y, HeightFogParams.maxDistance) : length(pV.xyz);
    vec3 viewDirV = normalize(pV.xyz);
    vec3 viewDirWS = normalize(transpose(mat3(ViewParams.ge_view)) * viewDirV);
    vec3 originWS = ViewParams.ge_cameraPosWS.xyz;

    vec3 scattering;
    float transmittance;
    ComputeHeightFog(originWS, viewDirWS, viewLength, vUV, scattering, transmittance);

    if (sky && (HeightFogParams.flags & kFlagSkyEnabled) != 0)
    {
        vec3 axis = ResolveHeightAxis();
        float axisDot = dot(viewDirWS, axis);
        float s = max(0.0, axisDot + HeightFogParams.skyHorizonOffset);
        s = HeightFogSaturate(HeightFogUnlerp(HeightFogParams.skyFillStart, HeightFogParams.skyFillEnd, s));
        s = smoothstep(1.0, 0.0, pow(s, max(HeightFogParams.skyPower, 0.001)));
        float bottom = pow(HeightFogSaturate(-axisDot), max(HeightFogParams.skyPower, 0.001)) *
                       clamp(HeightFogParams.skyBottomStrength, 0.0, 1.0);
        s = max(s, bottom);
        vec3 fogColor = HeightFogColor(originWS, viewDirWS, viewLength, vUV,
                                       FogExtinctionPerWorldUnit(HeightFogParams.density));
        float skyAmount = s * (1.0 - exp(-MaxFogOpticalDepth(HeightFogParams.density)));
        skyAmount = min(skyAmount, clamp(HeightFogParams.maxOpacity, 0.0, 1.0));
        scattering = fogColor * skyAmount;
        transmittance = 1.0 - skyAmount;
    }
    else if (sky)
    {
        oColor = scene;
        return;
    }

    // The output alpha (scene.a * effectiveTransmittance) is load-bearing, not a
    // leftover. The shared fog-glow pyramid samples this pass (HDRHeightFog) as its
    // uFogged input and reconstructs per-pixel fog transmittance as
    // fogged.a / scene.a (see fog_glow_fast.frag / fog_glow_prefilter.frag), which
    // isolates the additive fog radiance for the glow veil. Gating this write on
    // "fog glow off" would break that reconstruction, so it stays unconditional.
    //
    // It is safe for the presented image only because the fog-glow passes run
    // immediately after this pass and BEFORE the alpha is overwritten to 1.0
    // downstream (taa_resolve.comp writes vec4(color, 1.0); tonemap.frag forces
    // outA = 1.0 under the default preserveAlpha == 0). If TAA/Tonemap are ever
    // reordered ahead of the fog-glow pyramid, or a preserveAlpha tonemap path is
    // placed downstream of height fog, foggy regions would present semi-transparent
    // — revisit this alpha then.
    float effectiveTransmittance = mix(1.0, clamp(transmittance, 0.0, 1.0), fogStrength);
    vec3 fogged = scene.rgb * clamp(transmittance, 0.0, 1.0) + scattering;
    oColor = vec4(mix(scene.rgb, fogged, fogStrength), scene.a * effectiveTransmittance);
}
