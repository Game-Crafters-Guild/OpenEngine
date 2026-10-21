#version 450

#include "Includes/tonemap_aces2.glsl"
#include "Includes/cube_lut.glsl"
#include "Includes/night_tint.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uHDRColor;

// Auto-exposure history (AutoExposureNode). exposureScale is the metered+adapted linear multiplier;
// read only when useAutoExposure != 0. preExposure is the scale P the input is stored at
// (pre_exposure_snapshot.comp), divided out of the applied exposure in every mode. Always bound
// (the node publishes it every frame) so the fullscreen stage never skips for a missing buffer.
#define GE_EXPOSURE_HISTORY_BINDING 1
#include "Includes/exposure_history.glsl"

// The meter's reading, for the night tint's adaptation gate (bound by the engine, FullscreenShaderNode).
#define GE_EXPOSURE_COMPENSATION_BINDING 7
#include "Includes/exposure_compensation.glsl"

// The volume's Cube LUT (set 0, bindings 3 and 4; identity textures when none is bound). At the
// Finish stage it runs here, on the look's display-referred output before the HDR highlight
// extension; at the Grade stage hdr_color_fx.frag samples it instead.
layout(set = 0, binding = 3) uniform sampler3D uLut3d;
layout(set = 0, binding = 4) uniform sampler2D uLut1d;

// The night tint's inputs (Includes/night_tint.glsl): the light the star pass added (SkyStarLight,
// luminance at the frame's pre-exposure; black when no stars are drawn) and the view's depth
// (reverse-Z, 0 = sky).
layout(set = 0, binding = 5) uniform sampler2D uStarLight;
layout(set = 0, binding = 6) uniform sampler2D uDepth;

layout(push_constant) uniform TonemapPC {
    float exposure;
    int look;         // 0=Neutral, 1=Filmic (ACES 2), 2=Photographic (AgX); 3=no look, the
                      // debug arm a view selects (Passes::kTonemapNoLookArm), never a volume value
    int ditherMode;   // 98-99=debug views. This pass owns no dither at all — the terminal
                      // FinalSRGBEncode does — so no value here selects one.
    int outEncoding;  // 1=SDR display-referred linear (undithered — the terminal FinalSRGBEncode
                      // owns the quantization step and both its filters), 4=HDR linear (headroom),
                      // 5=luminance heatmap (debug). The output transfer function
                      // (sRGB/PQ/HLG/scRGB) is applied solely by FinalSRGBEncode.
    float paperWhiteNits;
    float maxOutputNits;
    int preserveAlpha; // 0 = opaque; 1 = pass source .a; 2 = derive RGB coverage for UI
    int useAutoExposure; // 0 = use pc.exposure; 1 = read the metered exposure from uExposure
    float nightTintStrength; // the artistic night tint's strength (PostProcessVolume::NightTintStrength), 0 = none
    float nightTintR;        // its hue, linear Rec.709 at luminance 1
    float nightTintG;
    float nightTintB;
    GE_CUBE_LUT_PUSH_MEMBERS // the Cube LUT: intensity 0 = none; sampled here at the Finish stage
} pc;

// Canonical HDR paper-white floor (framebuffer 1.0 == 80 nits anchor). Mirrors
// Rendering::kHdrPaperWhiteFloorNits and the same const in encode_srgb.frag.
const float kHdrPaperWhiteFloorNits = 80.0;

// --- The looks ---

// AgX display transform (Troy Sobotka's AgX, github.com/sobotka/AgX).
// sRGB linear → AgX working space, log2 encoding, polynomial sigmoid.
// Pipeline structure, EV range, and the sigmoid fit follow Benjamin Wrensch's "Minimal AgX
// Implementation": Copyright (c) 2024 Missing Deadlines (Benjamin Wrensch), MIT License.
// See ThirdParty/MinimalAgX/LICENSE.txt and UPSTREAM.md — including for how the inset/outset
// matrices below differ from Wrensch's printed values.
vec3 TonemapAgX(vec3 v)
{
    // sRGB-linear → AgX inset working space. The applied matrix, row-major;
    // each row sums to ~1 so the achromatic axis is preserved — a neutral grey
    // stays neutral:
    //   0.842479  0.078434  0.079224
    //   0.042374  0.878468  0.079224
    //   0.042342  0.078434  0.879143
    // Transcribed COLUMN-major for the GLSL mat3 constructor (which takes columns,
    // NOT rows — pasting the reference rows in directly transposes the matrix and
    // breaks the achromatic axis, tinting all AgX content warm).
    const mat3 agxInset = mat3(
        0.842479062253094,  0.0423738568909378, 0.0423421658632190,   // column 0
        0.0784335999206980, 0.878468162822090,  0.0784336006085089,   // column 1
        0.0792237451477643, 0.0792237451477643, 0.879142973793946     // column 2
    );
    vec3 agx = agxInset * max(v, vec3(1e-10));
    // Log2 encoding: map [minEV, maxEV] → [0, 1]
    const float minEV = -12.47393;
    const float maxEV = 4.026069;
    agx = clamp((log2(agx) - minEV) / (maxEV - minEV), 0.0, 1.0);
    // 6th order polynomial sigmoid approximation
    vec3 x2 = agx * agx;
    vec3 x4 = x2 * x2;
    vec3 sig = max(15.5 * x4 * x2 - 40.14 * x4 * agx + 31.96 * x4 - 6.868 * x2 * agx + 0.4298 * x2 + 0.1191 * agx - 0.00232, vec3(0.0));
    // EOTF: outset (inverse of the inset) restores chroma, then ^2.2 returns to scene-linear for the
    // downstream encode. Without the outset the result is the washed-out, desaturated "minimal" AgX.
    // Column-major for GLSL mat3; rows of the applied matrix sum to 1 (achromatic axis preserved).
    const mat3 agxOutset = mat3(
         1.1968800908, -0.0529602234, -0.0529204415,   // column 0
        -0.0980216014,  1.1519157412, -0.0980482752,   // column 1
        -0.0990233529, -0.0990320966,  1.1510759197);  // column 2
    return pow(max(agxOutset * sig, vec3(0.0)), vec3(2.2));
}

// Khronos PBR Neutral display mapping: compresses highlights toward white while preserving hue
// and near-white saturation (a gentle, non-filmic curve that keeps material albedo faithful).
// PBRNeutralToneMapping from KhronosGroup/ToneMapping PBR_Neutral/pbrNeutral.glsl:
// Copyright 2024 The Khronos Group, Inc., Apache-2.0.
// See ThirdParty/KhronosPbrNeutral/LICENSE.txt and UPSTREAM.md.
vec3 TonemapNeutral(vec3 v)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(v.r, min(v.g, v.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    v -= offset;
    float peak = max(v.r, max(v.g, v.b));
    if (peak < startCompression)
        return max(v, vec3(0.0));
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    v *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(v, newPeak * vec3(1.0), g);
}

// The look values that select an arm by name (Components::Look; Neutral is the default arm).
const int kLookFilmic = 1;

// The no-look debug arm: identity after exposure, so values above 1.0 survive to the
// output-range stage (differs from Neutral, which compresses highlights toward white).
const int kNoLookArm = 3;

vec3 NoLook(vec3 v)
{
    return max(v, vec3(0.0));
}

float HdrOutputMaxLinearValue()
{
    float paperWhiteNits = max(pc.paperWhiteNits, kHdrPaperWhiteFloorNits);
    float maxOutputNits = max(pc.maxOutputNits, paperWhiteNits);
    return max(maxOutputNits / paperWhiteNits, 1.0);
}

// ACES 2 tier headroom: the display's paper-white-relative peak under an HDR
// encode, exactly 1.0 (the 100-nit tier, t = 0) under SDR encodes.
float Aces2TargetOutputMax()
{
    return (pc.outEncoding >= 2 && pc.outEncoding <= 4) ? HdrOutputMaxLinearValue() : 1.0;
}

vec3 ApplyTonemap(vec3 hdr)
{
    switch (pc.look)
    {
        case kLookFilmic: return TonemapACES2(hdr, Aces2TargetOutputMax());
        case 2:           return TonemapAgX(hdr);
        case kNoLookArm:  return NoLook(hdr);
        default:          return TonemapNeutral(hdr);
    }
}

// The output transfer functions (sRGB / PQ / HLG / scRGB) live solely in the
// terminal FinalSRGBEncode pass (encode_srgb.frag). This pass only tonemaps to
// display-referred LINEAR (SDR clamped, or HDR headroom) and never encodes.

vec3 ApplyHdrDisplayShoulder(vec3 sceneLinear)
{
    vec3 x = max(sceneLinear, vec3(0.0));
    const float shoulderStart = 1.0;
    float paperWhiteNits = max(pc.paperWhiteNits, kHdrPaperWhiteFloorNits);
    float maxOutputNits = max(pc.maxOutputNits, paperWhiteNits);
    float peakSceneWhite = max(maxOutputNits / paperWhiteNits, shoulderStart + 0.001);
    vec3 above = max(x - vec3(shoulderStart), vec3(0.0));
    vec3 rolled = vec3(shoulderStart) + above / (vec3(1.0) + above / (peakSceneWhite - shoulderStart));
    return min(mix(x, rolled, step(vec3(shoulderStart), x)), vec3(peakSceneWhite));
}

// Extends an operator's OWN display-referred output into the display's headroom.
//
// The parameter is what the selected operator produced, never the scene. A tone
// mapper does two jobs — it compresses brightness AND it renders colour
// (desaturates highlights, holds hue, compresses out-of-gamut) — and only the
// operator knows what colour a highlight should be. This applies one positive
// scalar to its result, so chromaticity is preserved exactly and the colour
// rendering cannot be discarded. `sceneLinear` is deliberately NOT a parameter:
// the way to lose that guarantee is to reach for the scene value here.
//
// Below the knee nothing moves. Above it the excess is expanded by the exact
// inverse of a Reinhard shoulder: C1-continuous at the knee (slope 1, no visible
// kink) and mapping operator 1.0 onto `outputMax` exactly. An operator that
// clamped at 1.0 would land on the panel's peak luminance; the two extended
// looks only approach it (Neutral asymptotically, and AgX, whose polynomial
// sigmoid tops out at 0.9971), so both land short. That is each curve's own
// answer, and the right one. Note the slope near the top is
// ((outputMax - knee) / (1 - knee))^2, so a small difference in an operator's
// ceiling is a large difference in peak nits: AgX's 0.3% shortfall costs it 14%.
//
// The knee is the lowest 0.05-grid value that keeps every operator's rendering
// of a neutral scene 1.0 at or below paper white, so diffuse white stays
// anchored near BT.2408 reference white and the headroom goes to speculars and
// emitters. Neutral binds it: it renders scene 1.0 at 0.869, landing at 0.966
// here (196 nits at a 203-nit paper white); a 0.70 knee would put that at 214.
vec3 ApplyHdrHighlightExtension(vec3 displayReferred, float outputMax)
{
    const float kExtensionKnee = 0.75;
    const float kOperatorHeadroom = 1.0 - kExtensionKnee;

    vec3 v = max(displayReferred, vec3(0.0));
    float peak = max(max(v.r, v.g), v.b);
    if (peak <= kExtensionKnee)
        return v;

    // c maps an excess of (1.0 - knee) onto (outputMax - knee). Clamping the
    // excess keeps the denominator positive for any input, including an operator
    // that overshoots 1.0.
    float excess = min(peak - kExtensionKnee, kOperatorHeadroom);
    float c = 1.0 / kOperatorHeadroom - 1.0 / (outputMax - kExtensionKnee);
    float extendedPeak = kExtensionKnee + excess / (1.0 - c * excess);
    return v * (extendedPeak / peak);
}

vec3 ApplyOutputRangeTonemap(vec3 sceneLinear, vec3 sdrTonemapped)
{
    if (pc.outEncoding < 2 || pc.outEncoding > 4)
        return sdrTonemapped;

    // No look has no colour rendering to preserve — it applies no curve at all —
    // so it rolls the raw scene value into the headroom instead.
    if (pc.look == kNoLookArm)
        return ApplyHdrDisplayShoulder(sceneLinear);

    float outputMax = HdrOutputMaxLinearValue();
    if (outputMax <= 1.001)
        return clamp(sdrTonemapped, vec3(0.0), vec3(1.0));

    // Filmic (ACES 2) is peak-parameterised: ApplyTonemap already produced
    // display-referred output spanning [0, outputMax] from its tier ladder, so
    // like no look it bypasses the highlight extension — extending it would
    // apply headroom twice. Clamp only.
    if (pc.look == kLookFilmic)
        return min(sdrTonemapped, vec3(outputMax));

    // Every other look, present and future: extend what IT produced. A new
    // look needs no edit here, which is why this is not a switch.
    return min(ApplyHdrHighlightExtension(sdrTonemapped, outputMax), vec3(outputMax));
}

// The Cube LUT on the look's display-referred output, before the HDR extension. A LUT is authored
// for display values in [0, 1], so each pixel is looked up at the value it shows: a pixel inside
// [0, 1] as it is, under SDR and HDR alike (midtones grade as in SDR, at any panel peak), and a
// pixel above 1.0 (Filmic's HDR highlights) divided by its own largest channel and scaled back, so
// above 1.0 the LUT grades a highlight's color, not its tone. A grade of the HDR range that does not
// depend on the display is the Grade stage, sampled before the look (hdr_color_fx.frag). The no-look
// view shows the image without any grade, and a slot with no usable LUT (no tables flagged)
// samples nothing.
vec3 ApplyCubeLutFinish(vec3 displayReferred)
{
    if (pc.look == kNoLookArm || pc.lutStage != kCubeLutStageFinish || pc.lutIntensity <= 1e-5 ||
        pc.lutFlags == 0)
        return displayReferred;
    GE_CubeLutParams lut = GE_CUBE_LUT_PARAMS(pc);
    float scale = max(max(max(displayReferred.r, displayReferred.g), displayReferred.b), 1.0);
    return GE_ApplyCubeLut(displayReferred / scale, lut, uLut3d, uLut1d) * scale;
}

vec3 LuminanceHeatmap(vec3 sceneLinear)
{
    const vec3 lumaWeights = vec3(0.2126, 0.7152, 0.0722);
    float nits = dot(max(sceneLinear, vec3(0.0)), lumaWeights) * max(pc.paperWhiteNits, kHdrPaperWhiteFloorNits);
    float t = clamp(log2(max(nits, 0.01) / 100.0) / 6.0 + 0.5, 0.0, 1.0);
    return mix(vec3(0.0, 0.1, 0.8), mix(vec3(0.0, 0.9, 0.2), vec3(1.0, 0.1, 0.0), smoothstep(0.45, 1.0, t)), smoothstep(0.0, 0.55, t));
}

// The artistic night tint (Includes/night_tint.glsl). Its two reads (an explicit-LOD sample and a texel
// fetch) are legal after the per-pixel early-outs: WGSL refuses implicit-LOD sampling in non-uniform
// flow. The depth is fetched, not filtered, so WebGPU binds the depth texture as unfilterable.
vec3 ApplyNightTint(vec3 lin, float adaptationNits, vec2 uv, vec3 storedColor)
{
    const vec3 kY = vec3(0.2126, 0.7152, 0.0722);
    vec3 tint = max(vec3(pc.nightTintR, pc.nightTintG, pc.nightTintB), vec3(0.0));
    if (pc.nightTintStrength <= 0.0 || dot(tint, kY) <= 0.0)
        return lin;
    float sceneNits = GE_NightTintSceneNits(dot(storedColor, kY), GE_PreExposure());
    // Skip the fetches where the rest of the rule already spares the pixel.
    if (GE_NightTintAmount(pc.nightTintStrength, adaptationNits, sceneNits, 0.0) <= 0.0)
        return lin;
    ivec2 depthTexel = min(ivec2(uv * vec2(textureSize(uDepth, 0))), textureSize(uDepth, 0) - 1);
    float contrast = GE_NightTintStarContrast(dot(storedColor, kY), textureLod(uStarLight, uv, 0.0).r,
                                              texelFetch(uDepth, depthTexel, 0).r);
    return GE_NightTintApply(lin, tint, GE_NightTintAmount(pc.nightTintStrength, adaptationNits, sceneNits, contrast));
}

void main()
{
    vec2 uv = vec2(vUV.x, vUV.y);

    // Auto-exposure (eye adaptation) overrides the static exposure when active; otherwise the
    // Fixed/Manual/Physical scalar from the push constant is used (byte-identical when off).
    // Guarded like bloom_threshold: metering can decline (shaderpkg race, missing HDR source),
    // leaving the history buffer unwritten — a non-positive/NaN/absurd scale falls back to the
    // static exposure instead of a persistent black or blown-out frame.
    float autoExposure = uExposure.exposureScale;
    float exposure = (pc.useAutoExposure != 0 && autoExposure > 0.0 && autoExposure < 1e9)
                         ? autoExposure
                         : pc.exposure;
    // The input already carries P, so the exposure left to apply is exposure / P (near 1).
    exposure /= GE_PreExposure();

    // Debug modes
    if (pc.ditherMode == 99) { oColor = vec4(1.0, 0.0, 1.0, 1.0); return; }
    if (pc.ditherMode == 98) { vec3 raw = texture(uHDRColor, uv).rgb * exposure; oColor = vec4(raw, 1.0); return; }

    vec3 stored = texture(uHDRColor, uv).rgb;
    vec3 hdr = max(stored * exposure, vec3(0.0));
    vec3 lin = ApplyTonemap(hdr);
    lin = ApplyCubeLutFinish(lin);
    // The gate's adaptation: the meter's reading while Auto exposure runs (before the clamps and the
    // curve), else what the exposure maps to mid-grey. exposure x P is the scale from absolute
    // scene-linear to display.
    float meteredNits = pc.useAutoExposure != 0 ? GE_MeteredNits() : -1.0;
    float adaptationNits = GE_NightTintAdaptationNits(meteredNits, exposure * GE_PreExposure());
    lin = ApplyNightTint(lin, adaptationNits, uv, stored);
    lin = ApplyOutputRangeTonemap(hdr, lin);
    // Default to opaque (1.0) so the main view is unchanged. Lens-flare thumbnails
    // use mode 2 because their RGB atlas is luminous data on black rather than a
    // conventional alpha texture. Normalize the straight RGB representation before
    // the HDR UI pass premultiplies it, preserving flare color and brightness while
    // still exposing a useful coverage alpha.
    float outA = 1.0;
    if (pc.preserveAlpha == 2)
    {
        outA = clamp(max(lin.r, max(lin.g, lin.b)), 0.0, 1.0);
        lin = outA > 1e-5 ? lin / outA : vec3(0.0);
    }
    else if (pc.preserveAlpha != 0)
    {
        outA = texture(uHDRColor, uv).a;
    }
    // HDR: emit display-referred LINEAR (headroom-tonemapped); FinalSRGBEncode
    // applies PQ/HLG/scRGB.
    if (pc.outEncoding == 4) { oColor = vec4(lin, outA); return; }
    if (pc.outEncoding == 5) { oColor = vec4(LuminanceHeatmap(hdr), 1.0); return; }

    // SDR: display-referred LINEAR, UNDITHERED, whatever the swapchain is. The
    // terminal FinalSRGBEncode owns the quantization step and therefore owns
    // both filters, sized to the real destination — a step this pass cannot see.
    // Unconditional on purpose: a blueprint still carrying the retired
    // outEncoding 0 lands here rather than on a hole.
    oColor = vec4(lin, outA);
}
