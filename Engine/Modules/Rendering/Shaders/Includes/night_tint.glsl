#ifndef GE_NIGHT_TINT_GLSL
#define GE_NIGHT_TINT_GLSL

// The artistic night tint (PostProcessVolume::NightTintStrength and NightTintColor): a hue,
// normalised to luminance 1, given to the dark field of a night image on the display, after the look.
// Shared by tonemap.frag and the test probe (night_tint_probe.comp), so the rule the image follows and
// the rule the tests measure cannot drift apart.
//
// The rule:
// - Adaptation: the tint acts in full when the scene the view adapts to meters below 0.005 cd/m2 and
//   is gone above 0.05 cd/m2: a moonlit night (a full moon meters about 0.009) takes it, a lamp-lit
//   room (a 60 W lamp meters about 0.3), a dusk and a day do not. The reading is the meter's, before the
//   clamps and the exposure compensation curve, so neither the floor nor the curve changes it.
// - The dark field only: it fades out between 0.5 and 2 cd/m2 of scene luminance, so a moonlit surface
//   (under 0.1 cd/m2) takes it and the moon and a lit window keep their colour. The
//   window is scene-referred, before the exposure: the exposure compensation curve and the Auto Min EV
//   floor move a night's display brightness and must not choose its hue.
// - The stars keep their colour: the star pass records the light it adds (SkyStarLight). A sky pixel
//   brighter than the same pixel without the star light by 1.1 times is partly spared and by 1.5 times
//   fully spared, so a star's core and its glow keep the star's colour. The sky beside geometry, a lit
//   edge and a bright cloud have no star light and take the tint. Geometry in front of a star hides
//   its light: only sky pixels (depth 0, reverse-Z) count it.
const float kNightTintScotopicNits = 0.005;
const float kNightTintPhotopicNits = 0.05;
const float kNightTintFieldNits = 0.5;
const float kNightTintSpareNits = 2.0;
const float kNightTintFieldContrast = 1.1;
const float kNightTintSpareContrast = 1.5;

// A pixel's scene luminance in cd/m2: storedY is its luminance as stored, scene-linear at the frame's
// pre-exposure (1.0 = 203 cd/m2 once the pre-exposure is divided out).
float GE_NightTintSceneNits(float storedY, float preExposure)
{
    return storedY / max(preExposure, 1e-12) * 203.0;
}

// The adaptation the gate reads: the meter's reading when the view meters (meteredNits >= 0), else the
// luminance the exposure maps to mid-grey (0.18); appliedExposure is the scale from absolute
// scene-linear to display (the exposure times the pre-exposure).
float GE_NightTintAdaptationNits(float meteredNits, float appliedExposure)
{
    return meteredNits >= 0.0 ? meteredNits : 0.18 * 203.0 / max(appliedExposure, 1e-12);
}

// How far the star light lifts a pixel: its stored luminance over the luminance without the star
// light. 1 on the bare sky and on geometry. storedY and starY are at the same pre-exposure;
// depth is the pixel's reverse-Z depth (0 = sky).
float GE_NightTintStarContrast(float storedY, float starY, float depth)
{
    if (depth > 0.0)
        return 1.0;
    return storedY / max(storedY - starY, 1e-12);
}

// How much of the tint a pixel takes (0 to 1). strength: the volume's strength. adaptationNits: the
// scene's metered luminance, the average the meter keys to mid-grey, before the clamps and the curve
// (the luminance a Manual or Fixed exposure maps to mid-grey when no meter runs). sceneNits: the
// pixel's scene luminance in cd/m2, before the exposure. contrast: GE_NightTintStarContrast.
float GE_NightTintAmount(float strength, float adaptationNits, float sceneNits, float contrast)
{
    if (strength <= 0.0)
        return 0.0;
    float t = smoothstep(log(kNightTintScotopicNits), log(kNightTintPhotopicNits), log(max(adaptationNits, 1e-12)));
    float amount = clamp(strength, 0.0, 1.0) * (1.0 - t) *
                   (1.0 - smoothstep(kNightTintFieldNits, kNightTintSpareNits, sceneNits));
    return amount * (1.0 - smoothstep(kNightTintFieldContrast, kNightTintSpareContrast, contrast));
}

// The tinted display colour: lin re-hued toward tint (luminance 1) at its own luminance, by amount.
// The hue is desaturated toward gray where it would put a channel past the pixel's display range (1,
// or the pixel's own brightest channel when it is above 1): a bright twilight sky in the dark field
// keeps its luminance and takes as much of the hue as fits, rather than clipping its blue.
vec3 GE_NightTintApply(vec3 lin, vec3 tint, float amount)
{
    float tintY = dot(tint, vec3(0.2126, 0.7152, 0.0722));
    if (amount <= 0.0 || tintY <= 0.0)
        return lin;
    float displayY = dot(lin, vec3(0.2126, 0.7152, 0.0722));
    vec3 hued = displayY * tint / tintY;
    float ceiling = max(1.0, max(lin.r, max(lin.g, lin.b)));
    float peak = max(hued.r, max(hued.g, hued.b));
    if (peak > ceiling)
        hued = vec3(displayY) + (hued - vec3(displayY)) * max(ceiling - displayY, 0.0) / (peak - displayY);
    return mix(lin, hued, amount);
}

#endif // GE_NIGHT_TINT_GLSL
