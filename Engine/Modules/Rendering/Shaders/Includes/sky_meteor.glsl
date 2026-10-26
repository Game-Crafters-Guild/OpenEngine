#ifndef GE_SKY_METEOR_GLSL
#define GE_SKY_METEOR_GLSL

#include "error_function.glsl"
#include "integer_hash.glsl"
#include "sky_star_model.glsl"

// The falling stars (meteors): each is a star of the star model (sky_star_model.glsl) that moves.
// GE_StarShade gives it the stars' units, extinction, limit against the sky, observer, color and
// shape; this file decides where it is, how bright it is, and how its light spreads over its motion.
//
// The sky holds a fixed number of slots. Each slot flies one meteor in every window of
// slotCount * interval seconds, so one meteor starts every interval seconds on average, anywhere in
// the sky. A flight is drawn from a hash of its slot and its window: no two flights share a path.
//
// A flight: a start above kMeteorMinAltitude (uniform over that part of the sky), a heading within
// kMeteorMaxHeadingRadians of straight down, a great-circle arc at kMeteorMinSpeed to kMeteorMaxSpeed,
// lasting kMeteorMinDuration to kMeteorMaxDuration seconds. Its peak magnitude follows the meteors'
// population law: each magnitude brighter is kMeteorPopulationIndex times rarer, between
// kMeteorBrightestMagnitude and kMeteorFaintestMagnitude. The faint end is cut at +1, so at its peak
// every flight is as bright as the brighter stars. Its light rises and falls as sin^2 over the flight.
const float kMeteorMinAltitudeSine = 0.25881905; // sin(15 degrees)
const float kMeteorMaxHeadingRadians = 1.22173048; // 70 degrees
const float kMeteorMinSpeed = 0.13962634; // radians per second (8 degrees)
const float kMeteorMaxSpeed = 0.52359878; // radians per second (30 degrees)
const float kMeteorMinDuration = 0.3; // seconds
const float kMeteorMaxDuration = 1.0; // seconds
const float kMeteorPopulationIndex = 2.5;
const float kMeteorBrightestMagnitude = -3.0;
const float kMeteorFaintestMagnitude = 1.0;
// The eye integrates light over about 0.1 s: a meteor is seen as a streak along the path its head
// covered in that time, Seen by Eye or by Camera. The time is fixed, so the streak is the same at every
// frame rate.
const float kMeteorEyeIntegrationSeconds = 0.1;
// The light curve's floor, a share of the peak's light: a flight's first and last instants are
// drawn this faint (15 magnitudes below the peak) rather than at an infinite magnitude.
const float kMeteorLightCurveFloor = 1e-6;
const float kMeteorPi = 3.14159265;
// sqrt(pi) / 2: the integral of exp(-x^2) from 0 to infinity, which scales the streak's erf terms.
const float kMeteorHalfRootPi = 0.88622693;
// Below this streak length in pixels the Gaussian's mean over the segment is the Gaussian itself.
const float kMeteorPointStreakPixels = 0.05;
// The streak is cut this many core radii ahead of its head: there the core is exp(-16) of its
// peak, and the cut keeps exp(-a / tau) at most exp(4) (tau >= r), so its product with the erf pair
// never overflows.
const float kMeteorCoreLeadRadii = 4.0;

// A slot's meteor at one instant.
struct GE_Meteor
{
    bool Active;          // a flight is in the sky
    vec3 Start;           // the flight's start direction (world, unit)
    vec3 Head;            // the head's direction now (world, unit)
    vec3 Tail;            // the head's direction kMeteorEyeIntegrationSeconds ago, or at the start
    float PeakMagnitude;  // the flight's brightest V magnitude
    float Magnitude;      // the V magnitude now, on the light curve
    float Speed;          // radians per second
    float Duration;       // seconds
};

// Draw k (0 to 7) of flight number `flight`: every pair is its own key, so no two flights share a draw.
float GE_MeteorDraw(uint flight, uint k)
{
    return GE_HashUnit(flight * 8u + k);
}

// The slot's flight now. timeSeconds: the sky's clock. intervalSeconds: the mean time between two
// meteors anywhere in the sky. up: the planet's up at the camera.
GE_Meteor GE_MeteorAt(uint slot, uint slotCount, float timeSeconds, float intervalSeconds, vec3 up)
{
    float window = float(max(slotCount, 1u)) * max(intervalSeconds, 0.0);
    float windowIndex = floor(max(timeSeconds, 0.0) / max(window, 1e-3));
    float local = max(timeSeconds, 0.0) - windowIndex * window;
    uint flight = uint(windowIndex) * max(slotCount, 1u) + slot;

    GE_Meteor m;
    m.Duration = mix(kMeteorMinDuration, kMeteorMaxDuration, GE_MeteorDraw(flight, 0u));
    m.Speed = mix(kMeteorMinSpeed, kMeteorMaxSpeed, GE_MeteorDraw(flight, 1u));
    float launch = GE_MeteorDraw(flight, 2u) * max(window - m.Duration, 0.0);
    float t = local - launch;
    m.Active = t >= 0.0 && t <= m.Duration;

    vec3 east;
    vec3 north;
    GE_StarSkyFrame(up, east, north);
    float sinAltitude = mix(kMeteorMinAltitudeSine, 1.0, GE_MeteorDraw(flight, 3u));
    float cosAltitude = sqrt(max(1.0 - sinAltitude * sinAltitude, 0.0));
    float azimuth = 2.0 * kMeteorPi * GE_MeteorDraw(flight, 4u);
    vec3 level = cos(azimuth) * east + sin(azimuth) * north;
    m.Start = cosAltitude * level + sinAltitude * up;
    // Straight down along the sky from the start (toward the horizon below it), and across.
    vec3 down = sinAltitude * level - cosAltitude * up;
    vec3 across = cross(m.Start, down);
    float heading = kMeteorMaxHeadingRadians * (2.0 * GE_MeteorDraw(flight, 5u) - 1.0);
    vec3 tangent = cos(heading) * down + sin(heading) * across;

    float flown = clamp(t, 0.0, m.Duration);
    float headAngle = m.Speed * flown;
    float tailAngle = m.Speed * max(flown - kMeteorEyeIntegrationSeconds, 0.0);
    m.Head = cos(headAngle) * m.Start + sin(headAngle) * tangent;
    m.Tail = cos(tailAngle) * m.Start + sin(tailAngle) * tangent;

    // The population law's inverse: N(< m) grows as r^m, so m = log_r(r^a + u (r^b - r^a)).
    float r = kMeteorPopulationIndex;
    float brightest = pow(r, kMeteorBrightestMagnitude);
    float faintest = pow(r, kMeteorFaintestMagnitude);
    m.PeakMagnitude = log(mix(brightest, faintest, GE_MeteorDraw(flight, 6u))) / log(r);
    float curve = sin(kMeteorPi * flown / m.Duration);
    m.Magnitude = m.PeakMagnitude - 2.5 * log(max(curve * curve, kMeteorLightCurveFloor)) / log(10.0);
    return m;
}

// A falling star's shape: the round point core and no spikes. Summed along the streak, the spikes would
// draw a faint box with hard edges around the flight; their share of the glow stays in the glow
// (GE_StarShade), so the star's light is unchanged.
GE_StarShapeDials GE_MeteorShape(GE_StarShapeDials shape)
{
    shape.Spikes = 0.0;
    shape.Shape = kStarShapePoint;
    return shape;
}

// The radius in pixels of the streak's core: the core's Gaussian widened by the pixel's own spread (a
// pixel's variance, 1/12 along each axis), and at least kMeteorMinStreakRadius. A Gaussian narrower than
// that rises and falls by more than 0.5 % on the pixel grid as it moves; a star's core takes the pixel's
// mean instead (GE_StarCorePixelMean), which a streak at any angle has no closed form for.
const float kMeteorMinStreakRadius = 0.85;

float GE_MeteorStreakRadius(float coreRadius)
{
    return max(sqrt(coreRadius * coreRadius + 1.0 / 6.0), kMeteorMinStreakRadius);
}

// The streak's reach in pixels around its segment: the star's cut, and at least three streak radii.
float GE_MeteorCutRadius(GE_StarDrawn star)
{
    return max(star.CutRadius, 3.0 * GE_MeteorStreakRadius(star.CoreRadius));
}

// The distance in pixels from o to the streak, the segment from the head (0) to streak.
float GE_MeteorStreakDistance(vec2 o, vec2 streak)
{
    float along = clamp(dot(o, streak) / max(dot(streak, streak), 1e-12), 0.0, 1.0);
    return length(o - along * streak);
}

// The along-streak weight's decay length, as a share of the streak: the eye's persistence fades, so
// the head is brightest and the light it left falls off as exp(-s / tau) behind it. Seen by Camera the
// streak fades the same way, so both read as a head with a trail.
const float kMeteorTrailDecayShare = 1.0 / 3.0;
// The streak's glare and glow are summed over taps this many pixels apart along the segment, at most
// kMeteorMaxTaps of them. Each tap's glare is held at half a step (GE_MeteorRadianceAt), so the spacing
// sets how close to the streak the glare rises.
const float kMeteorTapSpacingPixels = 0.5;
// The share of the core's light drawn as the still core at the head; the trail carries the rest, so the
// star's light is unchanged. Spread along the streak alone, the core's Gaussian is cut in half at the
// head and the head is no brighter than the trail a quarter of the way back: a still frame shows an even
// line. With a quarter at the head, the head is a point of about two pixels, 5 to 14 times the light a
// quarter of the way back, on a 30 to 100 pixel streak.
const float kMeteorHeadShare = 0.25;
const int kMeteorMaxTaps = 64;

// The along-streak weight's decay length in pixels for a streak of streakLength pixels, at least the
// core's streak radius: a shorter decay is a point to the eye and would overflow the closed form.
float GE_MeteorTrailDecayPixels(float streakLength, float radius)
{
    return max(kMeteorTrailDecayShare * streakLength, radius);
}

// The weight's integral over the streak, the normalizer that keeps the star's energy:
// tau (1 - exp(-L / tau)).
float GE_MeteorWeightIntegral(float streakLength, float tau)
{
    return tau * (1.0 - exp(-streakLength / tau));
}

// The core's Gaussian exp(-(a - s)^2 / r^2) averaged over the streak s in [0, L] under the weight
// exp(-s / tau) from the head. a: pixels along the streak from the head. The closed form completes the
// square: (sqrt(PI) r / 2) (erf(a' / r) - erf((a' - L) / r)) at a' = a - r^2 / (2 tau), times
// exp(r^2 / (4 tau^2) - a / tau), over the weight's integral. Pixels more than kMeteorCoreLeadRadii core
// radii ahead of the head get none.
float GE_MeteorCoreAlong(float a, float radius, float streakLength, float tau)
{
    if (streakLength < kMeteorPointStreakPixels)
        return exp(-a * a / (radius * radius));
    if (a < -kMeteorCoreLeadRadii * radius)
        return 0.0;
    float shifted = a - radius * radius / (2.0 * tau);
    float scale = exp(radius * radius / (4.0 * tau * tau) - a / tau);
    float pair = GE_Erf(shifted / radius) - GE_Erf((shifted - streakLength) / radius);
    return kMeteorHalfRootPi * radius * scale * pair / GE_MeteorWeightIntegral(streakLength, tau);
}

// The radiance a drawn meteor puts on the pixel centered at offset o pixels from its head
// (scene-linear, before visibility). streak: pixels from the head to the tail. The whole star is
// integrated along the streak under the head-weighted trail's weight (Seen by Eye or by Camera), so the light on the pixel grid is the star's energy wherever the streak
// lies: the core as a Gaussian of GE_MeteorStreakRadius in closed form (two erf calls); the glare and
// the glow as weighted taps along the segment. kMeteorHeadShare of the core stays at the head as a still
// point. star is shaded with GE_MeteorShape: it has no spikes.
vec3 GE_MeteorRadianceAt(vec2 o, GE_StarDrawn star, vec2 streak)
{
    vec3 corePeak = star.CorePeak;
    float coreRadius = star.CoreRadius;
    star.CorePeak = vec3(0.0);

    float radius = GE_MeteorStreakRadius(star.CoreRadius);
    float radius2 = radius * radius;
    float streakLength = sqrt(dot(streak, streak));
    vec2 axis = streakLength > 0.0 ? streak / streakLength : vec2(1.0, 0.0);
    float tau = GE_MeteorTrailDecayPixels(streakLength, radius);

    // The taps sit at the midpoints of equal steps; each tap's glare is held below at half a step, so
    // the 1 / d^2 peak of one tap does not stand out between two taps.
    int taps = clamp(int(ceil(streakLength / kMeteorTapSpacingPixels)), 1, kMeteorMaxTaps);
    float step = streakLength / float(taps);
    star.CoreRadius = max(star.CoreRadius, 0.5 * step);
    vec3 rest = vec3(0.0);
    float weightSum = 0.0;
    for (int i = 0; i < taps; ++i)
    {
        float s = (float(i) + 0.5) * step;
        float weight = exp(-s / tau);
        rest += weight * GE_StarRadianceAt(o - s * axis, star);
        weightSum += weight;
    }
    rest /= weightSum;

    float a = dot(o, axis);
    vec2 across = o - a * axis;
    float trail = max(GE_MeteorCoreAlong(a, radius, streakLength, tau), 0.0);
    float along = mix(trail, exp(-a * a / radius2), kMeteorHeadShare);
    float core = coreRadius * coreRadius / radius2 * exp(-dot(across, across) / radius2) * along;
    return rest + corePeak * core;
}

#endif // GE_SKY_METEOR_GLSL
