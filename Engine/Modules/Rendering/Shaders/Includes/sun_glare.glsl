#ifndef GE_SUN_GLARE_GLSL
#define GE_SUN_GLARE_GLSL

// Sun glare: the veiling light the optics scatter out of the sun's beam, spread over the
// image by a point-spread function centred on the sun.
//
// The kernel and its normalisation live together because they are one quantity — the glare
// is a FRACTION of the sun's irradiance redistributed by a kernel of unit integral, so a
// change to one that misses the other is an energy error. Shared with the probe that
// integrates the kernel numerically (sun_glare_probe.comp), so the glare a camera sees and
// the glare the test measures cannot drift apart.
//
// All radiances are on the engine's unitless scene-linear scale (nits / 203, see
// LightPhotometry.h) — the same scale SceneColor and the sun disc use. Irradiance in means
// radiance out; the caller supplies the sun's above-atmosphere irradiance and multiplies the
// result by the transmittance along the ray to the sun.

#ifndef GE_GLARE_PI
#define GE_GLARE_PI 3.14159265359
#endif
#define GE_GLARE_DEG_PER_RAD 57.29577951308232

// ---------------------------------------------------------------------------------------
// The kernel: the CIE general disability-glare equation (CIE 146:2002; Vos & van den Berg),
//
//     PSF = 10/theta^3 + [5/theta^2 + 0.1 p/theta] [1 + (Age/62.5)^4] + 0.0025 p
//
// with theta the visual angle in DEGREES, valid 0.1 to 100 degrees. Printed in that form in
// Labuz, "Introduction to straylight" (repub.eur.nl/pub/102424), which is where these
// coefficients were checked rather than recalled.
//
// p = 0 here, which removes the last two terms: they model light scattered through the IRIS
// and SCLERA of an eye, and a lens has neither. Age 25 makes the wing's bracket 1.0256, so
// the two coefficients below are 10 and 5.128.
//
// Consistency check on the pair, kept because it is the reason to trust them: at theta = 2
// degrees the terms are 10/8 and 5/4, which sum to exactly the classic Stiles-Holladay
// 10/theta^2 = 2.5. The general equation is built to reduce to the classic one there, and no
// other coefficient pair does.
//
// The 1/theta^3 core carries 54.6% of the energy and the 1/theta^2 wing 45.4%, so roughly
// half the glare sits inside 2 degrees. Against the wing alone this halves the far veil:
// 19.6% of the energy lies beyond 10 degrees instead of 40.2%.
#define GE_SUN_GLARE_CORE_COEFF 10.0
#define GE_SUN_GLARE_WING_COEFF 5.128

// Fraction of the sun's irradiance that reaches the sensor as veiling glare rather than as
// the direct disc.
//
// Integrating the equation above over the sphere from the sun's own angular radius gives the
// EYE's straylight fraction, 0.131 — consistent with the ~10% ocular straylight figure the
// same literature reports. A camera is not an eye; this default sits an order of magnitude
// below it, inside the 0.5-3% veiling-glare index range of coated photographic lenses
// (ISO 9358), because a default that is wrong in the bright direction is the one that ruins
// a frame. It is the ONLY glare dial: everything else about the term is derived.
#define GE_SUN_GLARE_FRACTION 0.01

// SceneColor is RGBA16F. Anything above 65504 stores as +Inf, and an Inf reaches NaN
// downstream: bloom's Karis prefilter weights a tap by 1/(1 + brightness), so Inf * 0 = NaN
// (bloom_threshold.frag), and TAA's neighbourhood clamp does the same.
//
// The glare is ADDED to a target that already holds the sun disc, and the disc's own storage
// bound is half the format. So this is half of what the disc leaves: a quarter of the format
// maximum, which keeps disc + glare a full stop below +Inf with room for the fog in-scatter
// and the TAA blend that composite over both later.
//
// Where it binds: the halo's centre reaches ~21,900 for a 100 klx sun at noon transmittance,
// so the bound clips the innermost ~0.15 degrees — INSIDE the sun's own 0.27 degree disc,
// which is drawn over it. Beyond the disc's edge the kernel is unclipped at every hour.
#define GE_SUN_GLARE_MAX_STORED_RADIANCE 16376.0

// The kernel's argument is the SQUARED CHORD between the view ray and the direction to the
// sun, |dirWS - sunDirWS|^2 for unit vectors, which equals 2 (1 - cos theta) exactly.
//
// Chord rather than angle for two reasons. It is the variable the normalisation is closed
// form in (dOmega = 2 pi sin(theta) dtheta = pi d(chordSq)), and it is numerically stable at
// the centre: 1 - dot(a, b) cancels catastrophically for nearly-parallel unit vectors, while
// dot(a - b, a - b) does not.
//
// Chord and angle agree to 1.1% out to 30 degrees, which covers the range the CIE equation is
// validated over. Beyond that the chord compresses — 180 degrees maps to an effective 114.6 —
// which makes the far tail marginally brighter than the true angle would. That tail is a
// tenth of the energy spread over the whole far hemisphere.
float GE_SunGlareChordSq(vec3 viewDirWS, vec3 sunDirWS)
{
    vec3 d = viewDirWS - sunDirWS;
    return dot(d, d);
}

// theta^2 in DEGREES^2, regularised at the source's own angular radius. That floor is what
// keeps the halo from re-drawing the disc: scattered light carries no structure finer than
// the thing that scattered it, so the kernel is flat across the sun rather than adding a
// second, sharper spike on top of it.
float GE_SunGlareThetaSqDeg(float chordSq, float angularRadius)
{
    float rDeg = angularRadius * GE_GLARE_DEG_PER_RAD;
    return chordSq * (GE_GLARE_DEG_PER_RAD * GE_GLARE_DEG_PER_RAD) + max(rDeg * rDeg, 1e-12);
}

// Antiderivative of the unnormalised kernel against du, used only by the normalisation:
//   integral(10 u^-3/2 + C2 u^-1) du = -20/sqrt(u) + C2 ln(u)
float GE_SunGlareAntiderivative(float u)
{
    return -2.0 * GE_SUN_GLARE_CORE_COEFF * inversesqrt(u) + GE_SUN_GLARE_WING_COEFF * log(u);
}

// Solid-angle normalisation: the integral of the unnormalised kernel over the whole sphere.
// dOmega = pi d(chordSq) = pi (pi/180)^2 du, and both terms integrate in closed form, so this
// is exact rather than a small-angle approximation and the kernel integrates to 1 over the
// SPHERE — which is what the device probe checks against the true measure.
float GE_SunGlareNormalisation(float angularRadius)
{
    float rDeg = angularRadius * GE_GLARE_DEG_PER_RAD;
    float u0 = max(rDeg * rDeg, 1e-12);
    float u1 = 4.0 * (GE_GLARE_DEG_PER_RAD * GE_GLARE_DEG_PER_RAD) + u0;
    float c = GE_GLARE_PI * (GE_GLARE_PI / 180.0) * (GE_GLARE_PI / 180.0);
    return c * (GE_SunGlareAntiderivative(u1) - GE_SunGlareAntiderivative(u0));
}

// The CIE equation itself, unnormalised: 10/theta^3 + 5.128/theta^2 with theta in degrees,
// regularised at the source's radius. Exposed separately from the PSF so a test can check the
// two coefficients against the published cross-check rather than against a second copy of
// them — at 2 degrees this must equal the classic Stiles-Holladay 10/theta^2 = 2.5.
float GE_SunGlareKernel(float chordSq, float angularRadius)
{
    float u = GE_SunGlareThetaSqDeg(chordSq, angularRadius);
    return GE_SUN_GLARE_CORE_COEFF * inversesqrt(u) / u + GE_SUN_GLARE_WING_COEFF / u;
}

// The point-spread function, steradians^-1, integrating to exactly 1 over the sphere.
float GE_SunGlarePsf(float chordSq, float angularRadius)
{
    return GE_SunGlareKernel(chordSq, angularRadius) / GE_SunGlareNormalisation(angularRadius);
}

// Scalar glare radiance before the sun's colour and the ray's transmittance are applied.
// The disc is the direct term and this is the scattered one, so the two are disjoint by
// construction: the disc carries (1 - fraction) E and the halo carries fraction * E.
float GE_SunGlareRadiance(float irradiance, float chordSq, float angularRadius, float fraction)
{
    return fraction * irradiance * GE_SunGlarePsf(chordSq, angularRadius);
}


// ---------------------------------------------------------------------------------------
// Visibility: is the direct beam reaching the optics at all?
//
// An occluder blocks the beam BEFORE it enters the lens, so visibility scales the WHOLE
// halo rather than the pixels behind the occluder. Each term is the fraction of the sun's disc
// that reaches the eye, sampled over the same disc, and each sees a different set of
// occluders:
//
//   * the shadow cascades see anything that CASTS a shadow, on screen or off, including a sun
//     behind the camera -- but the CBT terrain is excluded from the shadow passes
//     (CBTRenderNode.h), so it is invisible to them;
//   * the terrain skyline, marched on the CPU, sees the terrain and nothing else;
//   * the screen probe sees anything in the depth buffer, casters and terrain alike, because
//     CBT writes the shared scene depth in the World pass -- but only while the sun is on
//     screen.
//
// The cascades and the skyline see disjoint occluders, so their fractions multiply. The probe
// sees both sets, so multiplying it in would count an occluder twice: a caster on screen that
// covers half the disc reads 0.5 in the probe AND 0.5 in the cascades, and the product 0.25 is
// half the true 0.5. Visibility is therefore the MINIMUM of the probe and that product. Each
// side is an estimate of the same fraction that can only miss occluders, never invent them,
// so the smaller one is the one that saw more. Off screen the probe reads 1 and the minimum is
// the product alone.
//
// What remains open, and is stated rather than hidden: an occluder that neither casts a shadow
// nor is terrain nor is on screen.

// Fraction of a small disc around the sun's projected position that is not covered by scene
// geometry. Reverse-Z: the sky is depth 0.0 and any drawn geometry is greater.
//
// The depth read is the scene depth ATTACHMENT the world pass wrote, fetched by texel. Sixteen
// texels do not justify a resolve pass of their own, and a resolve is a pass that has to be
// scheduled and paid for every frame whether the sun is up or not.
//
// The ring radius is given PER AXIS in UV so the taps form a circle in PIXELS; one scalar
// applied to both axes stretches the ring by the viewport aspect, which on a 3.4:1 view puts
// the horizontal taps three times further from the sun than the vertical ones and gives any
// occluder narrower than that a visibility floor it can never get below.
//
// Golden-angle taps with a smooth radial weight rather than a uniform binary count: taps
// enter and leave the sum with small weights, so a silhouette sweeping across the sun ramps
// the halo down continuously instead of stepping through the tap count.
#define GE_SUN_GLARE_PROBE_TAPS 16
#define GE_SUN_GLARE_GOLDEN_ANGLE 2.39996323

// The visibility disc's tap pattern, shared by the screen probe and the cascade term so the
// two sample the same disc: tap `i` as a position inside the unit disc and its weight.
void GE_SunGlareDiscTap(int i, out vec2 offset, out float weight)
{
    // ONE CENTRE TAP plus three stratified shells of five, rather than a golden-angle spiral.
    //
    // Two things the spiral got wrong at sixteen taps. Its angles bunch, so a horizontal
    // silhouette crossing the sun took the ring through 20-50% steps because several taps
    // shared a row. And it has no tap at r = 0, so an occluder covering the sun's own pixel and
    // nothing else went unseen -- which is the single most informative sample there is.
    const int kShells = 3;
    const int kPerShell = (GE_SUN_GLARE_PROBE_TAPS - 1) / kShells;
    float r;
    float a;
    if (i == 0)
    {
        r = 0.0;                                                       // the sun's own texel
        a = 0.0;
    }
    else
    {
        int shell = (i - 1) / kPerShell;
        int slot = (i - 1) - shell * kPerShell;
        r = sqrt((float(shell) + 0.5) / float(kShells));               // area-uniform in radius
        a = (float(slot) + 0.5) * (6.28318530718 / float(kPerShell))
            + float(shell) * GE_SUN_GLARE_GOLDEN_ANGLE;                // de-align the shells
    }
    offset = vec2(cos(a), sin(a)) * r;
    weight = 1.0 - r * r;                                              // smooth, centre-weighted
}

// Tap geometry of the screen probe, shared by both depth-source variants below so the ring
// cannot drift between them: the same texel and the same weight whatever the depth texture's
// sample count is.
void GE_SunGlareProbeTap(int i, ivec2 size, vec2 sunUV, vec2 probeRadiusUV,
                         out ivec2 texel, out float weight)
{
    vec2 offset;
    GE_SunGlareDiscTap(i, offset, weight);
    vec2 uv = sunUV + offset * probeRadiusUV;
    texel = clamp(ivec2(uv * vec2(size)), ivec2(0), size - ivec2(1));
}

float GE_SunGlareScreenVisibility(sampler2D sceneDepth, vec2 sunUV, vec2 probeRadiusUV)
{
    ivec2 size = textureSize(sceneDepth, 0);
    if (size.x <= 0 || size.y <= 0)
        return 1.0;

    float visible = 0.0;
    float total = 0.0;
    for (int i = 0; i < GE_SUN_GLARE_PROBE_TAPS; ++i)
    {
        ivec2 texel;
        float w;
        GE_SunGlareProbeTap(i, size, sunUV, probeRadiusUV, texel, w);
        visible += (texelFetch(sceneDepth, texel, 0).r > 0.0 ? 0.0 : 1.0) * w;
        total += w;
    }
    return visible / max(total, 1e-6);
}

// Multisampled scene depth. Each of the ring's texels holds `samples` depth values and EVERY
// one of them is averaged into its tap rather than one being picked: the probe estimates a
// COVERAGE FRACTION, so the unbiased estimate for a partly-covered texel is the fraction of
// its samples that are sky. That makes this path and the single-sample path above the same
// measurement at two resolutions rather than two behaviours that merely coexist, and it
// multiplies the fade's level count by the sample count -- which is the continuity a
// silhouette crossing the sun needs.
//
// Both alternatives are worse, for the same reason. The MINIMUM over samples ("is any sample
// sky") counts a texel with one sky sample as fully visible, biasing the halo bright at every
// silhouette edge and making MSAA-on leakier than MSAA-off. Sample 0 alone reproduces exactly
// what a depth resolve would have handed over, but discards samples already in memory and
// gives up a step of the fade for nothing.
float GE_SunGlareScreenVisibilityMS(sampler2DMS sceneDepth, int samples, vec2 sunUV,
                                    vec2 probeRadiusUV)
{
    ivec2 size = textureSize(sceneDepth);
    if (size.x <= 0 || size.y <= 0 || samples <= 0)
        return 1.0;

    float visible = 0.0;
    float total = 0.0;
    for (int i = 0; i < GE_SUN_GLARE_PROBE_TAPS; ++i)
    {
        ivec2 texel;
        float w;
        GE_SunGlareProbeTap(i, size, sunUV, probeRadiusUV, texel, w);
        float sky = 0.0;
        for (int s = 0; s < samples; ++s)
            sky += texelFetch(sceneDepth, texel, s).r > 0.0 ? 0.0 : 1.0;
        visible += (sky / float(samples)) * w;
        total += w;
    }
    return visible / max(total, 1e-6);
}

// One comparison lookup in a shadow cascade, at a point given by its CLIP-space position: 1 when
// nothing this cascade stores lies between the point and the light. Mirrors the conventions in
// shadow_sampling.glsl exactly: the comparison sampler is GreaterOrEqual ("lit if refDepth >=
// stored"), reverse-Z stores occluders with LARGER NDC depth, the bias is therefore ADDED to
// push the point toward the light, and the cascades rasterize with the negative viewport so the
// shadow UV's Y is flipped. A point outside the map reads lit, because the cascade knows
// nothing there, and so does a point nearer the light than the map's near plane, where nothing
// can be stored nearer still (casters beyond that plane are clamped onto it, at depth 1).
//
// Branch-free: the lookup is always issued and the verdict selected after it, so a tap's
// lookups carry no control flow between them. The gradient form is an explicit level-0
// lookup, which a compute stage needs (sampler2DArrayShadow has no textureLod).
float GE_SunGlareCascadeLit(sampler2DArrayShadow shadowArray, int cascadeIdx, vec4 clip,
                            float depthBias)
{
    vec3 ndc = clip.xyz / max(clip.w, 1e-6);
    vec2 uv = ndc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    float refDepth = ndc.z + depthBias;
    bool inside = clip.w > 0.0 && uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0 &&
                  refDepth < 1.0;
    float lit = textureGrad(shadowArray, vec4(clamp(uv, 0.0, 1.0), float(cascadeIdx),
                                              clamp(refDepth, 0.0, 1.0)),
                            vec2(0.0), vec2(0.0));
    return inside ? lit : 1.0;
}

// Distance bins along the beam, in meters from the eye toward the sun: edges 0, 0.25, 1, 4,
// 16, 64 and 256, and a last bin that runs to the cascade's near plane. Four to one per bin,
// so the lateral distance a bin assumes is within a factor of two of any occluder in it.
#define GE_SUN_GLARE_CASCADE_BINS 7
#define GE_SUN_GLARE_CASCADE_FIRST_EDGE 0.25
#define GE_SUN_GLARE_CASCADE_BIN_RATIO 4.0

// What every tap of one cascade shares: the eye and the beam's directions in the cascade's
// clip space (the projection is linear in the world position, so each lookup position is a
// sum of these rather than a matrix product), and whether a caster within the first bin hides
// the whole disc.
struct GE_SunGlareCascadeFrame
{
    vec4 eyeClip;
    vec4 alongClip;   // clip-space change per meter along the beam, toward the sun
    vec4 acrossXClip; // per meter along the beam, per unit of disc offset, on either axis
    vec4 acrossYClip;
    float eyeColumn;  // 1 - the caster fraction within the first bin of the eye's own column
    bool covers;      // the eye is inside this cascade's depth range
};

GE_SunGlareCascadeFrame GE_SunGlareCascadeFrameAt(sampler2DArrayShadow shadowArray,
                                                  mat4 cascadeVP, int cascadeIdx, vec3 eyeWS,
                                                  vec3 sunDirWS, float discRadius,
                                                  float depthBias)
{
    GE_SunGlareCascadeFrame f;
    f.eyeClip = cascadeVP * vec4(eyeWS, 1.0);
    float eyeDepth = f.eyeClip.z / max(f.eyeClip.w, 1e-6) + depthBias;
    f.covers = f.eyeClip.w > 0.0 && eyeDepth >= 0.0 && eyeDepth <= 1.0;

    // Two axes across the beam. Any pair perpendicular to it serves; the pattern has no
    // preferred orientation.
    vec3 helper = abs(sunDirWS.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 acrossX = normalize(cross(helper, sunDirWS));
    vec3 acrossY = cross(sunDirWS, acrossX);
    f.alongClip = cascadeVP * vec4(sunDirWS, 0.0);
    f.acrossXClip = cascadeVP * vec4(acrossX * discRadius, 0.0);
    f.acrossYClip = cascadeVP * vec4(acrossY * discRadius, 0.0);

    float firstFar = GE_SunGlareCascadeLit(
        shadowArray, cascadeIdx, f.eyeClip + f.alongClip * GE_SUN_GLARE_CASCADE_FIRST_EDGE,
        depthBias);
    float firstNear = GE_SunGlareCascadeLit(shadowArray, cascadeIdx, f.eyeClip, depthBias);
    f.eyeColumn = 1.0 - max(firstFar - firstNear, 0.0);
    return f;
}

// Whether the beam from the eye to one point of the sun's disc reaches it, according to one
// shadow cascade: 1 clear, 0 hidden, fractional across a texel. `tap` is the point's offset
// inside the unit disc (GE_SunGlareDiscTap), scaled by the disc's angular radius the frame was
// built with -- the same disc the screen probe samples, through the same taps.
//
// A shadow map stores, per column along the light, only how near the light its nearest caster
// is. The ray from the eye to a point of the disc at a small angle `offset` from the sun's
// center is, at distance t along the light, the column `eyeWS + offset * t`. So an occluder at
// distance t hides that point of the disc when the column at `offset * t` holds a caster whose
// light-facing surface lies near t. Every distance bin is tested: the column at the bin's own
// lateral distance, and a caster surface between the bin's two edges, which is the difference
// of two comparisons, lit at the far edge minus lit at the near edge. That difference is the
// filtered fraction of the column's texels whose stored depth falls inside the bin, so it
// fades across a texel like any other comparison lookup.
//
// The occluder's distance therefore sets how wide a band of the map the disc covers: d times
// the disc's diameter for an occluder at distance d. As the sun moves, an occluder's shadow
// edge sweeps across the eye at d times the sun's angular speed, so the edge crosses the band
// in the time the sun takes to move one disc diameter, whatever the distance. That is the time
// the screen probe takes too, so the two terms fade together for an occluder both of them see.
//
// The first bin is the eye's own column for every tap (the frame's eyeColumn): across 0.25 m
// the taps sit within a millimeter of the eye, finer than any cascade texel. An eye outside
// the cascade's depth range reads clear, because the cascade knows nothing about it.
float GE_SunGlareCascadeTapBeam(sampler2DArrayShadow shadowArray, int cascadeIdx,
                                GE_SunGlareCascadeFrame f, vec2 tap, float depthBias)
{
    if (!f.covers)
        return 1.0;
    // Clip-space change per meter along the beam of this disc point's sideways offset.
    vec4 lateralClip = f.acrossXClip * tap.x + f.acrossYClip * tap.y;
    float beam = f.eyeColumn;
    float nearEdge = GE_SUN_GLARE_CASCADE_FIRST_EDGE;
    for (int bin = 1; bin < GE_SUN_GLARE_CASCADE_BINS; ++bin)
    {
        float farEdge = nearEdge * GE_SUN_GLARE_CASCADE_BIN_RATIO;
        // The geometric middle of the bin; the last bin, which has no far edge, takes the
        // middle it would have at the same ratio.
        vec4 column = f.eyeClip + lateralClip * (nearEdge * 2.0);
        float litNear = GE_SunGlareCascadeLit(shadowArray, cascadeIdx,
                                              column + f.alongClip * nearEdge, depthBias);
        float litFar = bin == GE_SUN_GLARE_CASCADE_BINS - 1
                           ? 1.0
                           : GE_SunGlareCascadeLit(shadowArray, cascadeIdx,
                                                   column + f.alongClip * farEdge, depthBias);
        beam *= 1.0 - max(litFar - litNear, 0.0);
        nearEdge = farEdge;
    }
    return beam;
}

#endif // GE_SUN_GLARE_GLSL
