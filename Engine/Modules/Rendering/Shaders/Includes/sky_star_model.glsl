#ifndef GE_SKY_STAR_MODEL_GLSL
#define GE_SKY_STAR_MODEL_GLSL

#include "error_function.glsl"

// The night sky's star model: what one star of a given magnitude puts on screen. Shared by the star
// billboard (sky_stars_billboard.vert/.frag) and the test probe (sky_star_model_probe.comp), so the
// model a camera sees and the model the tests measure cannot drift apart.
//
// Units: radiance in cd/m2 (nits) until GE_StarShade returns, then scene-linear on the 203-nit
// anchor; illuminance in lux; distances on screen in pixels; angles in radians unless a name says
// degrees.
//
// The model: a star's light from its magnitude (Pogson), dimmed and reddened by the air along it; a
// pixel-sized Gaussian core, integrated over each pixel; for the eye observer, the eye's glare around
// it (CIE 146), taken from the core, and faint stars fading to white; visibility by the eye's threshold
// against the sky behind it (Schaefer); and the artistic dials on top: highlight compression, a glow
// that adds light, diffraction spikes that take theirs from the glow, the core's scale and the sparkle
// shape.

// Illuminance of a magnitude 0 star above the atmosphere (Allen's Astrophysical Quantities).
const float kStarZeroMagnitudeLux = 2.54e-6;
// Scene-linear 1.0 is 203 cd/m2.
const float kStarSceneLinearNits = 203.0;
// The magnitude the highlight compression pivots on: it changes the light of brighter stars only.
const float kStarReferenceMagnitude = 6.5;
// Every star's physical core radius in pixels (the Star core dial scales it, 1 = physical). The core
// is exp(-d^2 / r^2), a Gaussian of sigma r / sqrt(2): 1 px gives a sigma of 0.7 px, the display's
// smallest soft point. Each pixel shows the core's mean over its square (GE_StarCorePixelMean), so the
// core's scene-linear light summed over the pixels is the same wherever the star sits on the pixel
// grid, at any core width. On screen, after the tone curve, a core narrower than a pixel still changes
// its light as the camera turns (about 6 % at Core 0.6, 2.5 % at 0.75, 1.5 % at 0.85), and the value of
// its brightest pixel. It is the same for every star: a bright star is a brighter point and carries its
// glare and glow, not a wider disk.
const float kStarCoreRadiusPixels = 1.0;
// The quad's round cut is at least three core radii and at least this many pixels. It keeps 99.5 % of
// the core's light or more at any sub-pixel position over the whole Star core dial (0.5 to 3), and a
// faint star always rasterises.
const float kStarMinCutRadiusPixels = 1.8;
// The eye's threshold illuminance for a point source against a background of luminance B (Schaefer
// 1993, after Hecht and Blackwell): I = c (1 + sqrt(k B))^2 foot-candles with B in nanolamberts,
// c = 10^-9.8, k = 10^-1.9 below 1,500 nL (night vision) and c = 10^-8.35, k = 10^-5.9 above (day
// vision). It gives magnitude 6.6 under the natural moonless sky (1.7e-4 cd/m2), about 4 under a
// full-moon sky (18 mag/arcsec2, 6.8e-3 cd/m2) and about -4 in daylight (Venus). The limiting magnitude dial is the
// limit under the natural dark sky (kStarDarkSkyNits, the airglow the sky-view LUT carries at the zenith); a
// brighter sky lowers it by the threshold's change, a darker one does not raise it.
const float kStarLuxPerFootCandle = 10.764;
const float kStarDarkSkyNits = 1.7e-4;
// The limit fades out over half a magnitude, centred on the limiting magnitude. The limit and its
// lowering against a bright sky hold for both observers: the limit is the field's density, a dial.
const float kStarLimitFadeMagnitudes = 0.5;
// Scintillation reaches its authored amount at this airmass (about 5.7 degrees up).
const float kStarScintillationAirmass = 10.0;
// Glare (the eye's disability glare, CIE 146:2002, after Stiles and Holladay): around a source of
// illuminance E at the eye, the veiling luminance at an angle theta is L = k E / theta_deg^2, with
// k = 10 for a young observer (k = 10 (1 + (age / 70)^4) with age). The drawn halo stops where it
// falls below a tenth of the natural moonless sky (1.7e-4 cd/m2) and never reaches past this many
// pixels; inside the core it holds the core edge's value. It is the eye's: the eye observer draws it,
// the camera observer does not.
const float kStarGlareK = 10.0;
const float kStarGlareFloorNits = 1.7e-5;
const float kStarMaxGlareRadiusPixels = 64.0;
// The share of a star's light the glare and the glow together may take from its core.
const float kStarMaxHaloFraction = 0.8;
// Colour: the eye sees faint stars grey. For the eye observer a star's colour fades to white by its
// illuminance at the eye, only at the faint end: full down to magnitude 3.5 (1.0e-7 lx), none from
// magnitude 5.5 (1.6e-8 lx), so the bright stars, the ones a viewer sees colour in, keep the catalog's
// full spread of colours. The camera observer keeps every star's catalog colour.
const float kStarColourFullLux = 1.0e-7;
const float kStarColourNoneLux = 1.6e-8;
// The artistic glow (Star glow): a (1 + d^2 / R^2)^-2 lobe on the bright stars, R the angle of 7 px at
// 900 rows over a 60 degree field, drawn out to 4 R, which holds 16/17 of its energy.
// It applies in full to stars brighter than magnitude 1.5 and fades out to magnitude 3. It is artistic
// and adds light: its energy is Star glow times the star's light, around the core or the sparkle, which
// keep the light they have with no glow.
const float kStarGlowRadiusRadians = 7.0 * 1.1547005 / 900.0;
const float kStarGlowCutRadii = 4.0;
const float kStarGlowFullMagnitude = 1.5;
const float kStarGlowNoneMagnitude = 3.0;

const float kStarDegreesPerRadian = 57.29577951;

// Illuminance above the atmosphere of a star of the given V magnitude, in lux.
float GE_StarIlluminance(float magnitude)
{
    return kStarZeroMagnitudeLux * exp2(-0.4 * 3.32192809 * magnitude);
}

// A star's light relative to a reference star (magnitude 6.5), after the highlight compression:
// (10^(0.4 (6.5 - m)))^compression. Compression 1 is physical; lower values compress the bright end.
float GE_StarEnergy(float magnitude, float compression)
{
    return exp2(0.4 * 3.32192809 * (kStarReferenceMagnitude - magnitude) * clamp(compression, 0.1, 1.0));
}

// Relative airmass toward altitude sinAlt (a Kasten-like fit, finite at the horizon).
float GE_StarAirmass(float sinAlt)
{
    float s = max(sinAlt, 0.0);
    return 1.0 / (s + 0.025 * exp(-11.0 * s));
}

// Scintillation amplitude at altitude sinAlt: zero at the zenith, growing as airmass^1.75 (Dravins et
// al. 1997) to amountAtAirmass10 at airmass 10, and capped there.
float GE_StarScintillationAmplitude(float sinAlt, float amountAtAirmass10)
{
    float x = pow(GE_StarAirmass(sinAlt), 1.75);
    float x10 = pow(kStarScintillationAirmass, 1.75);
    return amountAtAirmass10 * clamp((x - 1.0) / (x10 - 1.0), 0.0, 1.0);
}

// The faintest magnitude the eye sees against a background of the given luminance in nits (the
// threshold above), as a V magnitude.
float GE_StarThresholdMagnitude(float backgroundNits)
{
    float b = max(backgroundNits, 0.0) * 1.0e9 / 3183.0988;
    bool night = b < 1500.0;
    float c = night ? 1.58489319e-10 : 4.46683592e-9;
    float k = night ? 1.25892541e-2 : 1.25892541e-6;
    float footCandles = c * (1.0 + sqrt(k * b)) * (1.0 + sqrt(k * b));
    return -2.5 * log(footCandles * kStarLuxPerFootCandle / kStarZeroMagnitudeLux) / log(10.0);
}

// The limiting magnitude against the sky behind a star: the dial's limit under the natural dark sky,
// lowered by how much the threshold rises against skyNits (the sky-view LUT behind the star, which
// carries the airglow, twilight and moonlight).
float GE_StarLimitAgainstSky(float limitingMagnitude, float skyNits)
{
    return limitingMagnitude + GE_StarThresholdMagnitude(max(skyNits, kStarDarkSkyNits)) -
           GE_StarThresholdMagnitude(kStarDarkSkyNits);
}

// Visibility against the limiting magnitude, by the star's magnitude after extinction.
float GE_StarLimitVisibility(float apparentMagnitude, float limitingMagnitude)
{
    return clamp((limitingMagnitude + 0.5 * kStarLimitFadeMagnitudes - apparentMagnitude) / kStarLimitFadeMagnitudes,
                 0.0, 1.0);
}

// The drawn glare's energy around a star, in nits x pixels^2, for a glare scale G (the glare's
// luminance at one pixel from the star times one pixel^2), a core radius r and a cut radius c
// (c >= 2 r): G / max(d^2, r^2) inside the cut, faded by 1 - smoothstep(0.5, 1, d / c). Its integral
// is PI G (1 + 2 (ln(c / r) - 0.2988)).
float GE_StarGlareEnergy(float glareScale, float coreRadius, float cutRadius)
{
    return 3.14159265 * glareScale * (1.0 + 2.0 * (log(cutRadius / coreRadius) - 0.2988));
}

// The artistic glow's light as a multiple of the star's: the Star glow amount on the brightest stars,
// fading to none from magnitude 1.5 to 3.
float GE_StarGlowFraction(float magnitude, float glow)
{
    return clamp(glow, 0.0, 4.0) * (1.0 - smoothstep(kStarGlowFullMagnitude, kStarGlowNoneMagnitude, magnitude));
}

// How much of a star's colour shows, from its illuminance at the eye in lux: 0 (white) for faint stars,
// 1 from magnitude 3.5.
float GE_StarColourSaturation(float lux)
{
    return smoothstep(log(kStarColourNoneLux), log(kStarColourFullLux), log(max(lux, 1e-20)));
}


// Where a star sits for the sky: its direction in the camera's planet-local frame (x east, y up,
// z north: how the sky-view LUT is addressed, as sky_render.frag reads it), the sine of its altitude
// (the transmittance LUT's input: extinction along the star, not along the camera), and one pixel's
// solid angle. The pixel is the one on the view axis wherever the star sits on screen: the real
// off-axis pixel is smaller (cos^3), which would make a star 2.5 times brighter at the edge of a
// 60 degree view than at its centre and brighten it as the camera pans.
struct GE_StarViewGeometry
{
    vec3 LutDirection;
    float SinAltitude;
    float PixelSolidAngle;
};

// The sky's frame at the camera: east and north, perpendicular to the planet's up.
void GE_StarSkyFrame(vec3 up, out vec3 east, out vec3 north)
{
    vec3 northRef = abs(dot(vec3(0.0, 0.0, 1.0), up)) > 0.98 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0);
    east = normalize(cross(up, northRef));
    north = normalize(cross(east, up));
}

GE_StarViewGeometry GE_StarGeometry(vec3 starDir, vec3 up, float tanHalfFovY, float viewportRows)
{
    vec3 east;
    vec3 north;
    GE_StarSkyFrame(up, east, north);
    GE_StarViewGeometry g;
    g.LutDirection = vec3(dot(starDir, east), dot(starDir, up), dot(starDir, north));
    g.SinAltitude = clamp(dot(starDir, up), -1.0, 1.0);
    float pixelAngle = 2.0 * tanHalfFovY / max(viewportRows, 1.0);
    g.PixelSolidAngle = pixelAngle * pixelAngle;
    return g;
}

// The star's artistic shape (StarShape): a round core (the physical shape, its size the Star core
// dial) or the four-pointed sparkle (Classic) on every star. The glow and the spikes are drawn on either.
const float kStarShapePoint = 0.0;
const float kStarShapeSparkle = 1.0;
// Diffraction spikes (Star spikes): four arms along the screen's axes on the stars that carry the glow,
// each exp(-a / L) along the arm and exp(-c^2 / w^2) across it, L the Spike length times the glow radius
// (a falloff length: an arm fades to a third over L) and w this width. Their light is the given share of
// the glow's, so the star's total does not change.
const float kStarSpikeWidthPixels = 0.6;
const float kStarSpikeCount = 4.0;
// The sparkle (Classic): its reach S grows with the star's energy from this many pixels, as
// S = kStarSparkleReachPixels * energy^0.2, at most 4 glow radii. Inside it, a round core of
// kStarSparkleCoreRadius S under four points with the metric (sqrt|x| + sqrt|y|)^2, both falling off as
// (1 - t)^kStarSparkleFalloff past kStarSparkleFlatSize; the profile integrates to kStarSparkleIntegral S^2
// (checked by the probe's integration test).
const float kStarSparkleReachPixels = 3.0;
const float kStarSparkleCoreRadius = 0.4;
const float kStarSparkleFlatSize = 0.05;
const float kStarSparkleFalloff = 2.5;
const float kStarSparkleIntegral = 0.101648;

// What a star puts on screen.
struct GE_StarDrawn
{
    vec3 CorePeak;        // the core's (or the sparkle's) peak radiance, scene-linear
    float CoreRadius;     // pixels: the core's radius, or the sparkle's reach S
    vec3 GlareScale;      // the glare's radiance at one pixel, scene-linear (G above)
    float CutRadius;      // pixels: the quad's half-size
    vec3 GlowPeak;        // the glow lobe's peak radiance, scene-linear
    float GlowRadius;     // pixels (R above)
    vec3 SpikePeak;       // each spike's radiance at the star's centre, scene-linear
    float SpikeLength;    // pixels: the arms' falloff length (L above)
    float Shape;          // kStarShapePoint or kStarShapeSparkle
    float GlareFraction;  // the share of the star's light in the glare
    float GlowFraction;   // the glow's light as a multiple of the star's (spikes excluded)
    float SpikeFraction;  // the spikes' light as a multiple of the star's
    float TotalEnergy;    // nits x pixels^2: the star's light on screen plus the glow's and the spikes'
    float Visibility;     // 0 hides the star
};

// The dials of a star's shape: Star core (the core's scale, 1 = physical), Star spikes (the share of
// the glow's light in the spikes, 0 = none), Spike length (the spikes' falloff, in glow radii), the
// shape, and the observer (1 = the eye: its glare, faint stars fading to white as rod vision sees them;
// 0 = a camera: the same stars without the glare, each in its catalog colour).
struct GE_StarShapeDials
{
    float CoreScale;
    float Spikes;
    float SpikeLength;
    float Shape;
    float Eye;
};

// The sparkle's falloff of a normalised distance in [0, 1]: flat to kStarSparkleFlatSize, then
// (1 - t)^kStarSparkleFalloff to exactly 0 at 1.
float GE_StarSparkleFalloff(float d)
{
    float n = clamp(d, 0.0, 1.0);
    float t = n <= kStarSparkleFlatSize ? 0.0 : (n - kStarSparkleFlatSize) / (1.0 - kStarSparkleFlatSize);
    return pow(1.0 - t, kStarSparkleFalloff);
}

// The sparkle's profile at q (pixels over the reach S): the round core under the four points.
float GE_StarSparkle(vec2 q)
{
    vec2 s = sqrt(abs(q));
    float points = (s.x + s.y) * (s.x + s.y);
    return max(GE_StarSparkleFalloff(length(q) / kStarSparkleCoreRadius), GE_StarSparkleFalloff(points));
}

// magnitude: the star's V magnitude. color: linear Rec.709, luminance 1. transmittance: the
// atmosphere's transmittance along the star (extinction). skyNits: the sky's luminance behind it.
// flicker: the twinkle's multiplier this frame. pixelSolidAngle: one pixel's solid angle on the view
// axis (GE_StarGeometry).
GE_StarDrawn GE_StarShade(float magnitude, vec3 color, vec3 transmittance, float skyNits, float limitingMagnitude,
                          float compression, float glow, GE_StarShapeDials shape, float flicker, float pixelSolidAngle)
{
    GE_StarDrawn star;
    float transmittanceY = dot(transmittance, vec3(0.2126, 0.7152, 0.0722));
    float energy = GE_StarEnergy(magnitude, compression);
    float lux = GE_StarIlluminance(kStarReferenceMagnitude) * energy * transmittanceY * max(flicker, 0.0);

    // The star's light over the screen, in nits x pixels^2: its illuminance over one pixel's solid angle.
    float total = lux / pixelSolidAngle;
    float coreRadius = kStarCoreRadiusPixels * clamp(shape.CoreScale, 0.5, 3.0);
    bool eye = shape.Eye >= 0.5;

    // The eye's glare at one pixel: k E / (degrees per pixel)^2, and the radius where it reaches the
    // floor. A camera draws none.
    float pixelDegrees = sqrt(pixelSolidAngle) * kStarDegreesPerRadian;
    float glareScale = eye ? kStarGlareK * lux / (pixelDegrees * pixelDegrees) : 0.0;
    float minCutRadius = max(3.0 * coreRadius, kStarMinCutRadiusPixels);
    float cutRadius = clamp(sqrt(glareScale / kStarGlareFloorNits), minCutRadius,
                            max(kStarMaxGlareRadiusPixels, minCutRadius));
    float glareEnergy = GE_StarGlareEnergy(glareScale, coreRadius, cutRadius);

    // Energy is conserved for the glare: its share comes out of the core.
    glareEnergy = min(glareEnergy, kStarMaxHaloFraction * total);

    // The artistic glow adds light in a wide lobe, on top of the core; the spikes take their share of it.
    float glowRadius = kStarGlowRadiusRadians / sqrt(pixelSolidAngle);
    float addedEnergy = GE_StarGlowFraction(magnitude, glow) * total;
    float spikeShare = clamp(shape.Spikes, 0.0, 1.0);
    float spikeEnergy = addedEnergy * spikeShare;
    float glowEnergy = addedEnergy - spikeEnergy;
    float spikeLength = glowRadius * clamp(shape.SpikeLength, 0.5, 4.0);
    if (glowEnergy > 0.0)
        cutRadius = max(cutRadius, kStarGlowCutRadii * glowRadius);
    if (spikeEnergy > 0.0)
        cutRadius = max(cutRadius, 4.0 * spikeLength);
    glareScale = glareEnergy / max(GE_StarGlareEnergy(1.0, coreRadius, cutRadius), 1e-12);
    float glowPeakNits = glowEnergy / (3.14159265 * glowRadius * glowRadius *
                                       (1.0 - 1.0 / (1.0 + kStarGlowCutRadii * kStarGlowCutRadii)));
    // Each arm, cut at the quad's edge, holds L (1 - exp(-cut / L)) w sqrt(pi) times its centre value.
    float spikeArm = spikeLength * (1.0 - exp(-cutRadius / spikeLength)) * kStarSpikeWidthPixels * 1.77245385;
    float spikePeakNits = spikeEnergy / (kStarSpikeCount * spikeArm);

    // The core's light, on the round core or spread over the sparkle; the glow is its own lobe under either.
    float coreEnergy = total - glareEnergy;
    float corePeakNits;
    if (shape.Shape >= 0.5)
    {
        float reach = min(kStarSparkleReachPixels * pow(max(energy, 1e-6), 0.2), kStarGlowCutRadii * glowRadius);
        corePeakNits = coreEnergy / (kStarSparkleIntegral * reach * reach);
        coreRadius = reach;
        cutRadius = max(cutRadius, reach);
        glareScale = glareEnergy / max(GE_StarGlareEnergy(1.0, kStarCoreRadiusPixels, cutRadius), 1e-12);
    }
    else
    {
        corePeakNits = coreEnergy / (3.14159265 * coreRadius * coreRadius);
    }

    float apparentMagnitude = magnitude - 2.5 * log(max(transmittanceY, 1e-12)) / log(10.0);
    star.Visibility = GE_StarLimitVisibility(apparentMagnitude, GE_StarLimitAgainstSky(limitingMagnitude, skyNits));

    // The colour has luminance 1. The transmittance, normalised, reddens it: the colour a camera records.
    // For the eye, a star too faint for the cones is seen by the rods, which see no colour, so it fades
    // to the working space's white (R = G = B), not to the air's reddening. The physical (uncompressed)
    // illuminance decides.
    float physicalLux = GE_StarIlluminance(magnitude) * transmittanceY;
    vec3 reddened = color * transmittance / max(transmittanceY, 1e-12);
    float saturation = eye ? GE_StarColourSaturation(physicalLux) : 1.0;
    vec3 tint = mix(vec3(1.0), reddened, saturation);
    star.CorePeak = tint * (corePeakNits / kStarSceneLinearNits);
    star.CoreRadius = coreRadius;
    star.GlareScale = tint * (glareScale / kStarSceneLinearNits);
    star.CutRadius = cutRadius;
    star.GlowPeak = tint * (glowPeakNits / kStarSceneLinearNits);
    star.GlowRadius = glowRadius;
    star.SpikePeak = tint * (spikePeakNits / kStarSceneLinearNits);
    star.SpikeLength = spikeLength;
    star.Shape = shape.Shape >= 0.5 ? kStarShapeSparkle : kStarShapePoint;
    star.GlareFraction = total > 0.0 ? glareEnergy / total : 0.0;
    star.GlowFraction = total > 0.0 ? glowEnergy / total : 0.0;
    star.SpikeFraction = total > 0.0 ? spikeEnergy / total : 0.0;
    star.TotalEnergy = total + addedEnergy;
    return star;
}

// The mean of the core exp(-d^2 / r^2) over the pixel centred at o pixels from the star: the Gaussian's
// integral over the pixel's square, (PI r^2 / 4) (erf((x + 1/2) / r) - erf((x - 1/2) / r)) times the
// same along y. Summed over the pixel grid it is PI r^2, the core's integral, wherever the star sits.
float GE_StarCorePixelMean(vec2 o, float r)
{
    vec2 high = (o + 0.5) / r;
    vec2 low = (o - 0.5) / r;
    float alongX = max(GE_Erf(high.x) - GE_Erf(low.x), 0.0);
    float alongY = max(GE_Erf(high.y) - GE_Erf(low.y), 0.0);
    return 0.25 * 3.14159265 * r * r * alongX * alongY;
}

// Four taps of a rotated grid over one pixel, in pixels from its centre (they sum to zero), for the
// sparkle's sub-pixel points.
const vec2 kStarPixelTaps[4] = vec2[4](vec2(0.125, 0.375), vec2(0.375, -0.125), vec2(-0.125, -0.375),
                                       vec2(-0.375, 0.125));

// The radiance a drawn star puts on the pixel centred at offset o pixels from its centre (scene-linear),
// before visibility: the round core is the pixel's mean (GE_StarCorePixelMean); the sparkle, the glare,
// the glow and the spikes are taken at o. Every term is non-negative: a star only adds light.
vec3 GE_StarRadianceAt(vec2 o, GE_StarDrawn star)
{
    float d2 = dot(o, o);
    float d = sqrt(d2);
    float innerRadius = star.Shape >= 0.5 ? kStarCoreRadiusPixels : star.CoreRadius;
    vec3 body = star.Shape >= 0.5 ? star.CorePeak * GE_StarSparkle(o / star.CoreRadius)
                                  : star.CorePeak * GE_StarCorePixelMean(o, star.CoreRadius);
    vec3 halo = star.GlareScale / max(d2, innerRadius * innerRadius) * (1.0 - smoothstep(0.5, 1.0, d / star.CutRadius));
    float x = d2 / max(star.GlowRadius * star.GlowRadius, 1e-6);
    vec3 glow = d <= kStarGlowCutRadii * star.GlowRadius ? star.GlowPeak / ((1.0 + x) * (1.0 + x)) : vec3(0.0);
    vec2 a = abs(o);
    float w2 = kStarSpikeWidthPixels * kStarSpikeWidthPixels;
    float spikes = exp(-a.x / star.SpikeLength) * exp(-o.y * o.y / w2) + exp(-a.y / star.SpikeLength) * exp(-o.x * o.x / w2);
    return body + halo + glow + star.SpikePeak * spikes;
}

// How much of a star the moon's disk hides (0 to 1), for unit directions to the star and the moon and
// the disk's drawn angular radius in radians. A star is beyond the moon and fades out over the disk's
// edge, from 1.25 to 0.95 drawn radii. A falling star burns in the upper air, about 100 km up, far in
// front of the moon: the disk never hides it.
float GE_StarMoonCover(vec3 starDir, vec3 moonDir, float moonAngularRadius, bool fallingStar)
{
    if (fallingStar)
        return 0.0;
    float radius = max(moonAngularRadius, 0.0);
    return smoothstep(cos(radius * 1.25), cos(radius * 0.95), dot(starDir, moonDir));
}

#endif // GE_SKY_STAR_MODEL_GLSL
