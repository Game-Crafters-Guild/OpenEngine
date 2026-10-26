#ifndef GE_MOON_DISC_GLSL
#define GE_MOON_DISC_GLSL

// The visible moon disc: its frame on the sky, the lit shape the sun gives it, the radiance of its
// surface and the sky drawn under it. Shared by the sky pass (sky_render.frag, both projections) and
// the night probe (Engine/Tests/Shaders/sky_night_probe.comp, SkyNightProbeTests), so the test
// measures what ships.
//
// Moon UV: the drawn disc's square, x along the disc's right axis, y down it (the texture's rows);
// p = moonUv * 2 - 1 is the position in drawn radii.

#ifndef GE_SKY_PI
#define GE_SKY_PI 3.14159265359
#endif

// The moon's color at unit luminance (SkySystemConfig::moonColor, the same values): sunlight off a
// surface redder than grey. sky_view_lut.comp lights the sky with the same tint.
const vec3 GE_MOON_TINT = vec3(1.2142, 0.9614, 0.7521);

// The real moon's angular radius, radians (0.2595 degrees at its mean distance). The disc may be
// drawn larger (SkyEnvironment::MoonSize); a magnified image keeps its radiance, so the surface is as
// bright as the real moon's at any drawn size, and the art's moon drawn at this radius carries the
// real moon's light.
const float GE_MOON_ANGULAR_RADIUS = 0.004529;

// The mean of moon_full.png's linear luminance over the art's moon (alpha-weighted): the surface's
// albedo pattern is about this mean, so the art's moon has the real moon's mean radiance, and at a
// drawn radius of GE_MOON_ANGULAR_RADIUS / GE_MOON_TEXTURE_RADIUS it delivers the moonlight.
// SkyNightProbeTests measures the result against the staged texture.
const float GE_MOON_TEXTURE_MEAN = 0.3531;

// The radius of the moon in moon_full.png, in the drawn disc's radii (where the art's alpha falls to
// 0.5): the sphere the sun lights. A phase mask on the whole drawn disc puts a crescent's lit sliver
// outside the art, so a two-day moon would draw as earthshine alone.
const float GE_MOON_TEXTURE_RADIUS = 0.889;

// Earthshine: the moon's night side lit by the sunlit Earth. The Earth's phase seen from the moon is
// the complement of the moon's, so earthshine is brightest at a new moon and gone at a full moon. At a
// thin crescent the dark side's radiance is about 1e-4 of the lit side's (Goode et al. 2001,
// Geophys. Res. Lett. 28:1671, the Big Bear earthshine record; Danjon's ashen light), scaled here by
// the Earth's lit fraction: half of that at a quarter moon.
const float GE_MOON_EARTHSHINE_AT_FULL_EARTH = 1.1e-4;
// The color of the moon's surface under earthlight, at unit luminance: earthlight is bluer than
// sunlight (Rayleigh scattering and the oceans), more than the moon's red reflectance warms it, so
// the ashen light is neutral to bluish, never warmer than the lit side (GE_MOON_TINT).
const vec3 GE_MOON_EARTHSHINE_TINT = vec3(0.937, 1.008, 1.109);

// The disc's edge profile at `t`, the distance from the disc's centre over its drawn radius.
float GE_MoonDiscProfile(float t)
{
    return smoothstep(1.0, 0.92, t);
}

// The disc's right and up axes on the sky for the moon at `moonDir` (world space, unit): right is
// horizontal, up completes the frame toward the zenith.
void GE_MoonDiscFrame(vec3 moonDir, out vec3 discRight, out vec3 discUp)
{
    vec3 upFallback = abs(moonDir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    discRight = normalize(cross(upFallback, moonDir));
    discUp = cross(moonDir, discRight);
}

// The direction toward the sun in the disc's own frame, matching moon UV: x along discRight, y down
// discUp, z toward the observer. The sun is far enough that the direction from the moon is the
// direction from the observer, so z is the cosine of the phase angle: 1 full, -1 new.
vec3 GE_MoonSunInDiscFrame(vec3 sunDir, vec3 moonDir, vec3 discRight, vec3 discUp)
{
    return vec3(dot(sunDir, discRight), -dot(sunDir, discUp), -dot(sunDir, moonDir));
}

// The share of the moon's visible face the sun lights, (1 + cos a) / 2 for the phase angle a. Kept
// above 0 so a new moon's earthshine stays finite.
float GE_MoonLitFraction(vec3 sunInDisc)
{
    return max(0.5 * (1.0 + sunInDisc.z), 1e-3);
}

// 1 on the sunlit half of the art's moon at moon UV `moonUv`, 0 on the dark half and outside it, with
// a narrow soft terminator: the hemisphere facing `sunInDisc` (GE_MoonSunInDiscFrame).
float GE_MoonPhaseMask(vec2 moonUv, vec3 sunInDisc)
{
    vec2 p = (moonUv * 2.0 - 1.0) / GE_MOON_TEXTURE_RADIUS;
    float r2 = dot(p, p);
    if (r2 > 1.0)
        return 0.0;

    float z = sqrt(max(0.0, 1.0 - r2));
    float lit = dot(vec3(p.x, p.y, z), normalize(sunInDisc));
    return smoothstep(-0.04, 0.04, lit);
}

// The dark side's radiance as a fraction of the lit side's: GE_MOON_EARTHSHINE_AT_FULL_EARTH times
// the Earth's lit fraction seen from the moon, (1 - cos a) / 2 for the moon's phase angle a.
float GE_MoonEarthshineFraction(vec3 sunInDisc)
{
    return GE_MOON_EARTHSHINE_AT_FULL_EARTH * 0.5 * (1.0 - sunInDisc.z);
}

// Radiance of the lunar surface at one disc sample, scene-linear on the sky's unitless scale: the
// moon's illuminance (with the phase already in it) over the real moon's solid angle and the lit
// share of its face, shaped by the texture's albedo (`albedo`, linear); the sunlit part in the moon's
// color, the dark part in earthshine's; then the stylistic trim in stops
// (SkyEnvironment::MoonExposureEV, 0 by default).
vec3 GE_MoonSurfaceRadiance(vec3 albedo, float phaseMask, float illuminance, vec3 sunInDisc, float trimEv)
{
    float realSolidAngle = GE_SKY_PI * GE_MOON_ANGULAR_RADIUS * GE_MOON_ANGULAR_RADIUS;
    float albedoPattern = dot(albedo, vec3(0.2126, 0.7152, 0.0722)) / GE_MOON_TEXTURE_MEAN;
    float lit = illuminance / (realSolidAngle * GE_MoonLitFraction(sunInDisc));
    vec3 color = mix(GE_MOON_EARTHSHINE_TINT * GE_MoonEarthshineFraction(sunInDisc), GE_MOON_TINT, phaseMask);
    return color * (lit * albedoPattern * exp2(trimEv));
}

// What the disc draws at moon UV `moonUv`, `t` drawn radii from its centre, from the art's tap
// `albedo`: the surface's radiance times the disc profile and the art's alpha. The caller applies the
// view ray's transmittance (perspective) or its horizon masks (the orthographic backdrop).
vec3 GE_MoonDiscRadiance(vec4 albedo, vec2 moonUv, float t, vec3 sunInDisc, float illuminance, float trimEv)
{
    return GE_MoonSurfaceRadiance(albedo.rgb, GE_MoonPhaseMask(moonUv, sunInDisc), illuminance, sunInDisc,
                                  trimEv) *
           GE_MoonDiscProfile(t) * albedo.a;
}

// How much of what lies behind the moon the disc hides at `t` drawn radii from its centre, from the
// art's alpha: the stars, which are beyond it. A falling star is in front of the moon and draws over
// the disk (GE_StarMoonCover).
float GE_MoonDiscCover(float t, float albedoAlpha)
{
    return GE_MoonDiscProfile(t) * albedoAlpha;
}

// One of the four directions on the drawn disc's rim, `k` in 0..3: right, up, left, down of the
// moon by `drawnRadius`.
vec3 GE_MoonRimDirection(vec3 moonDir, vec3 discRight, vec3 discUp, float drawnRadius, int k)
{
    float angle = float(k) * (0.5 * GE_SKY_PI);
    vec3 radial = discRight * cos(angle) + discUp * sin(angle);
    return moonDir * cos(drawnRadius) + radial * sin(drawnRadius);
}

// The sky drawn under the disc. The air's in-scatter and the airglow lie in front of the moon, so the
// disc does not hide them; but the moon's forward scatter peaks at its centre, and under a disc drawn
// larger than the real moon that peak would show through the dark limb, brighter and (near the
// horizon) warmer than the sky around the disc. So where the disc draws, the sky is the sky around
// it, `rimSky` (the mean of the GE_MoonRimDirection taps: one value over the whole disc, no point at
// its centre), weighted by the disc's cover and the view ray's transmittance, so a hidden or faint
// moon leaves `directSky` as it is.
vec3 GE_MoonSkyUnderDisc(vec3 directSky, vec3 rimSky, float cover, vec3 transmittance)
{
    float weight = clamp(cover * dot(transmittance, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    return mix(directSky, rimSky, weight);
}

#endif
