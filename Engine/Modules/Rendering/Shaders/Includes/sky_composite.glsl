#ifndef GE_SKY_COMPOSITE_GLSL
#define GE_SKY_COMPOSITE_GLSL

#include "compat_profile.glsl"

#include "sky_ray_sphere.glsl"
#include "sky_transmittance_lut.glsl"

// Shared sky composite: the ground / below-horizon blend and the night-sky
// gradient that sit on TOP of the atmospheric sky-view LUT sample. Factored out
// of sky_render.frag so the IBL capture (sky_capture_cube.frag) reproduces the
// EXACT same end-result sky the user sees on screen — minus the bright discs
// (sun / moon / stars), which the capture omits so the prefilter never fireflies.
//
// These functions are a verbatim extract of the inlined blocks that used to live
// in sky_render.frag's perspective path; the on-screen sky output is unchanged.
// All inputs are passed explicitly (no UBO dependency) so this include compiles
// against any UBO layout that supplies the fields.

#ifndef GE_SKY_PI
#define GE_SKY_PI 3.14159265359
#endif

// Sky-view LUT mapping: planet-local direction (east/up/north frame) <-> LUT UV.
// u is the full-circle azimuth (atan over east/north), so it wraps continuously
// across u=0/1 with a Repeat-u sampler — no seam anywhere on the visible sky.
// v is the elevation, warped by a signed-sqrt so texels concentrate at the
// horizon (v=0.5) where the atmospheric gradient changes fastest, and thin out
// toward the poles. This replaces the octahedral encode, whose diagonal fold
// produced a visible seam along the horizon on the directly-viewed sky.
//
// GE_SkyDirToViewLutUv and GE_SkyViewLutUvToDir are exact inverses; the compute
// pass decodes UV->dir to march, the render/capture passes encode dir->UV to read.

// Encode a planet-local direction (x=east, y=up, z=north) to the sky-view LUT UV.
vec2 GE_SkyDirToViewLutUv(vec3 dLocal)
{
    float u = atan(dLocal.z, dLocal.x) * (0.5 / GE_SKY_PI) + 0.5; // azimuth, wraps at u=0/1
    float elev = asin(clamp(dLocal.y, -1.0, 1.0));                // [-PI/2, PI/2]
    float e = elev / GE_SKY_PI + 0.5;                             // [0,1], 0.5 == horizon
    float s = e * 2.0 - 1.0;
    float v = 0.5 + 0.5 * sign(s) * sqrt(abs(s));                 // concentrate texels at horizon
    return vec2(u, v);
}

// Decode a sky-view LUT UV back to a planet-local direction (exact inverse of
// GE_SkyDirToViewLutUv). Returns x=east, y=up, z=north components.
vec3 GE_SkyViewLutUvToDir(vec2 uv)
{
    float az = (uv.x - 0.5) * (2.0 * GE_SKY_PI);
    float s = uv.y * 2.0 - 1.0;
    float e = 0.5 + 0.5 * sign(s) * (s * s);                      // invert the sqrt warp
    float elev = (e - 0.5) * GE_SKY_PI;
    float cosEl = cos(elev), sinEl = sin(elev);
    return vec3(cosEl * cos(az), sinEl, cosEl * sin(az));
}

float GE_SkyHash13(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p += dot(p, p.yzx + 19.19);
    return fract(p.x * p.y * p.z);
}

// Layers the ground / below-horizon blend and the night sky's own glow over the
// atmospheric sky color, for the visible sky and the IBL capture alike. Returns the
// composited sky color (still pre-exposure).
//
//   skyColor       - the sky-view LUT sample (scene-linear, pre-composite)
//   dirWS          - world-space view direction (for the night noise term)
//   viewZenithCos  - dot(dirWS, planet-local up)
//   nightAmount    - clamp(nightSkyBlend, 0, 1)
//   applyGround    - when false, skip the below-horizon ground/dark block so the
//                    raw atmospheric sky shows below the horizon (open sky). The
//                    night glow (above the horizon) is unaffected either way.
//   groundAlbedo / groundBrightness / groundNightColor / belowHorizon* /
//   nightSkyHorizonColor - the AtmosphereUBO fields the blocks consume.
vec3 GE_ApplySkyGroundAndNight(vec3 skyColor,
                               vec3 dirWS,
                               float viewZenithCos,
                               float nightAmount,
                               bool applyGround,
                               vec3 groundAlbedo,
                               float groundBrightness,
                               vec3 groundNightColor,
                               float belowHorizonDarkness,
                               vec3 belowHorizonDarkColor,
                               float belowHorizonBlendSharpness,
                               vec3 nightSkyHorizonColor)
{
    // ---- Ground color blend ----
    vec3 dayGroundColor = groundAlbedo * max(groundBrightness, 0.0);
    vec3 groundColor = mix(dayGroundColor, groundNightColor, clamp(nightAmount, 0.0, 1.0));
    if (applyGround && viewZenithCos < 0.0)
    {
        float darkness = clamp(belowHorizonDarkness, 0.0, 1.0);
        skyColor = mix(groundColor, belowHorizonDarkColor, darkness);
        float k = max(belowHorizonBlendSharpness, 0.001);
        float t = clamp(-viewZenithCos * k, 0.0, 1.0);
        skyColor = mix(skyColor, groundColor, t);
    }

    // ---- Night sky glow ----
    // The natural night sky's own light, added to the LUT's sky, which carries the twilight and the
    // moonlit sky itself: airglow and integrated starlight, about 1.7e-4 cd/m2 at the zenith (V 22
    // mag per square arcsecond, Leinert et al. 1998), brightening toward the horizon (van Rhijn's
    // longer path through the glowing layer) to nightSkyHorizonColor. Desaturated blue-grey.
    // When the stylized ground is painted (applyGround), the glow stays strictly above the horizon
    // so it never tints that ground; the physical modes (ContinueHorizon / PlanetGround) mirror it
    // below the horizon, |viewZenithCos| driving the gradient (PlanetGround then composites its lit
    // floor on top).
    float nightFloorCos = applyGround ? 0.0 : -1.0;
    if (nightAmount > 0.0 && viewZenithCos > nightFloorCos)
    {
        float elevCos = applyGround ? max(viewZenithCos, 0.0) : abs(viewZenithCos);
        // Scene-linear on the 203-nit anchor.
        const vec3 kNightZenithCol = vec3(6.98e-7, 8.50e-7, 1.163e-6);
        vec3 glow = mix(nightSkyHorizonColor, kNightZenithCol, smoothstep(0.0, 0.85, elevCos));

        float noise = GE_SkyHash13(normalize(dirWS) * 37.0);
        glow *= 1.0 + (noise - 0.5) * 0.05;

        skyColor += glow * clamp(nightAmount, 0.0, 1.0);
    }

    return skyColor;
}

// Aerial perspective: the floor sits behind the in-scattered air light (the horizon
// haze the LUT stored for this direction). A single grey view-ray transmittance keyed
// off distance fades the floor into that haze with no per-pixel march.
vec3 GE_AerialPerspectiveComposite(vec3 hazeBackdrop, vec3 floorColor, float distToFloor, float extinction)
{
    float viewTrans = exp(-distToFloor * extinction);
    return mix(hazeBackdrop, floorColor, viewTrans);
}

// Number of sky-view LUT taps used for the skylight the ground receives. The taps are
// drawn from the cosine-weighted density itself (below), so every tap carries weight
// 1/N and the estimator is exact for a uniform sky at any N; N buys accuracy on a sky
// that varies, which is the low-sun case.
//
// Cost, stated without flattery: glslang leaves this loop ROLLED (one OpLoopMerge in the
// emitted SPIR-V), so it is N sequential iterations of a CDF inverse, a sincos and a
// texture fetch -- not N unrolled ALU ops. The tap UVs are identical for every
// invocation, which is the one thing in its favour (the fetches hit the same texels
// frame-wide), but this is a real per-invocation cost wherever it runs per pixel.
#ifndef GE_SKY_AMBIENT_SAMPLE_COUNT
#define GE_SKY_AMBIENT_SAMPLE_COUNT 16
#endif

// Cosine-weighted mean sky radiance over the upper hemisphere, as seen from the ground:
//
//     E    = integral of L(w) (n.w) dw over the upper hemisphere
//     Lbar = E / PI            (the integral of (n.w) dw over that hemisphere is PI)
//
// so a Lambertian floor of albedo p leaves radiance p*E/PI == p*Lbar. Lbar is what this
// returns, and GE_GroundBounceRadiance multiplies it by the albedo and nothing else.
//
// The taps are importance-sampled from the cosine density: its CDF in the zenith angle
// is sin^2(theta), so theta_i = asin(sqrt(u_i)) over stratified u_i puts the taps where
// the weight is and the estimate is the plain MEAN of the taps.
//
// The shape of that density is worth stating, because BOTH ways of picking one
// representative direction are wrong. The weight per unit zenith angle is
// cos(theta)sin(theta): ZERO at the zenith (vanishing solid angle), ZERO at the horizon
// (vanishing cosine), peaking at 45 degrees, with mean exactly 45 degrees. A zenith tap
// understates a sky that brightens toward the horizon; a horizon tap overstates it by
// weighting the directions the cosine is busy suppressing.
//
// Azimuth turns by the golden angle per tap rather than sitting on a ring, so a low-sun
// sky -- bright and warm toward the sun, dim and blue away from it -- is sampled all the
// way round instead of being read off one side.
vec3 GE_SkyDiffuseMeanRadiance(sampler2D skyViewLut, float lutHeight)
{
    const float kGoldenAngleRadians = 2.39996323;
    float vMin = 0.5 / lutHeight;
    float vMax = 1.0 - vMin;

    vec3 sum = vec3(0.0);
    for (int i = 0; i < GE_SKY_AMBIENT_SAMPLE_COUNT; ++i)
    {
        float u    = (float(i) + 0.5) / float(GE_SKY_AMBIENT_SAMPLE_COUNT);
        float sinT = sqrt(u);                   // sin(theta) = sqrt(u) inverts the CDF
        float cosT = sqrt(max(1.0 - u, 0.0));
        float phi  = float(i) * kGoldenAngleRadians;

        // Planet-local: x east, y up, z north -- the frame GE_SkyDirToViewLutUv encodes.
        vec3 dir = vec3(sinT * cos(phi), cosT, sinT * sin(phi));
        vec2 uv  = GE_SkyDirToViewLutUv(dir);
        uv.y     = clamp(uv.y, vMin, vMax);
        // Compat needs an explicit lod: this mean is sampled from a per-fragment
        // PlanetGround branch, and WGSL forbids implicit lod in non-uniform flow.
        sum += GE_TAP_LOD0(skyViewLut, uv).rgb;
    }
    return sum / float(GE_SKY_AMBIENT_SAMPLE_COUNT);
}

// Where a horizon colour comes from (GE_SkyHorizonColor); HeightFogColorSource mirrors the values.
#define GE_SKY_HORIZON_SOURCE_VIEW_LUT 1
#define GE_SKY_HORIZON_SOURCE_CONSTANT 2
#define GE_SKY_HORIZON_SOURCE_HDRI 3

// Compass resolution of an HDRI's horizon colour: the row is averaged over 1/64 of a turn
// (5.6 degrees) around each direction, so its detail (a fence, a tree line, the sun) washes out
// instead of drawing one column of colour per texel.
#define GE_SKY_HORIZON_HDRI_COLUMNS 64.0
#define GE_SKY_HORIZON_HDRI_TAPS 4

// The visible sky's radiance at the horizon in the azimuth of dirWS: scene-linear, before the
// sky's exposure trim, which sky_render.frag multiplies in last and a caller multiplies in the same
// way. This is the colour of the far air a ground-hugging haze stands in front of: the backdrop's
// horizon row (v = 0.5) under the shared night composite, evaluated at viewZenithCos == 0, where
// the composite's ground block does not apply. Below-horizon view directions read the horizon of
// their own azimuth, so a haze over the ground meets the sky without a seam.
//
// skyBackdrop is the texture the sky pass draws its backdrop from: the sky-view LUT, or an HDRI.
// At the horizon the two encodings coincide (the LUT's u is the same atan(z, x) azimuth as the
// HDRI's lat-long u, and both put the horizon at v = 0.5). The LUT is smooth along its horizon
// row and is tapped once at lod 0. An HDRI is averaged (averageAlongRow): from the mip whose width
// is GE_SKY_HORIZON_HDRI_COLUMNS where its chain reaches that far, with GE_SKY_HORIZON_HDRI_TAPS
// taps across the window for what the chain does not cover (an HDRI cooked without mips).
// azimuthOffsetRadians is the HDRI's rotation about the zenith (0 for the LUT); nightAmount is 0
// for an HDRI, which the sky draws without the night composite.
//
// The planet-local frame is taken as the world axes (east +X, up +Y, north +Z): the sky pass's frame
// for a camera above the world origin. Away from the origin the sky's up tilts by distance over the
// planet radius, about 0.01 degrees per kilometre at Earth scale.
//
// Explicit lods: callers evaluate this per fragment inside non-uniform branches, where WGSL
// refuses an implicit one.
vec3 GE_SkyHorizonRadiance(sampler2D skyBackdrop,
                           vec3 dirWS,
                           float azimuthOffsetRadians,
                           float nightAmount,
                           vec3 nightSkyHorizonColor,
                           bool averageAlongRow)
{
    // A view straight up or down has no azimuth; any horizon direction is as good as another.
    vec3 flatDir = vec3(dirWS.x, 0.0, dirWS.z);
    vec3 horizonDir = dot(flatDir, flatDir) > 1e-12 ? normalize(flatDir) : vec3(1.0, 0.0, 0.0);
    // The HDRI's rotation turns the tap about the zenith. At offset 0 the tap direction equals
    // horizonDir bit for bit, so a LUT tap lands exactly where the sky pass's own tap does.
    float cosOffset = cos(azimuthOffsetRadians);
    float sinOffset = sin(azimuthOffsetRadians);
    vec3 tapDir = vec3(cosOffset * horizonDir.x - sinOffset * horizonDir.z, 0.0,
                       sinOffset * horizonDir.x + cosOffset * horizonDir.z);
    vec2 tapUv = GE_SkyDirToViewLutUv(tapDir);

    vec3 skyColor;
    if (averageAlongRow)
    {
        float width = float(textureSize(skyBackdrop, 0).x);
        float lod = clamp(log2(width / GE_SKY_HORIZON_HDRI_COLUMNS), 0.0,
                          float(textureQueryLevels(skyBackdrop) - 1));
        skyColor = vec3(0.0);
        for (int i = 0; i < GE_SKY_HORIZON_HDRI_TAPS; ++i)
        {
            float offset = ((float(i) + 0.5) / float(GE_SKY_HORIZON_HDRI_TAPS) - 0.5) /
                           GE_SKY_HORIZON_HDRI_COLUMNS;
            skyColor += textureLod(skyBackdrop, vec2(tapUv.x + offset, tapUv.y), lod).rgb;
        }
        skyColor /= float(GE_SKY_HORIZON_HDRI_TAPS);
    }
    else
    {
        skyColor = textureLod(skyBackdrop, tapUv, 0.0).rgb;
    }
    // The night composite's per-direction dither hashes horizonDir, the direction of the sky pixel
    // the haze stands in front of.
    return GE_ApplySkyGroundAndNight(skyColor, horizonDir, 0.0, nightAmount,
                                     false, vec3(0.0), 0.0, vec3(0.0), 0.0, vec3(0.0), 1.0,
                                     nightSkyHorizonColor);
}

// A haze's colour from the sky, in the units the sky draws in. source is one of the
// GE_SKY_HORIZON_SOURCE_* values. A constant source (the gradient sky's horizon, the stylized
// ground's rim) is horizonColor itself; the backdrop sources read GE_SkyHorizonRadiance with
// horizonColor as the night sky's horizon colour. scale is the sky's exposure trim, times the
// HDRI's intensity for an HDRI.
vec3 GE_SkyHorizonColor(sampler2D skyBackdrop,
                        int source,
                        vec3 dirWS,
                        float azimuthOffsetRadians,
                        float nightAmount,
                        vec3 horizonColor,
                        float scale)
{
    if (source == GE_SKY_HORIZON_SOURCE_CONSTANT)
        return horizonColor * scale;
    return GE_SkyHorizonRadiance(skyBackdrop, dirWS, azimuthOffsetRadians, nightAmount, horizonColor,
                                 source == GE_SKY_HORIZON_SOURCE_HDRI) * scale;
}

// Radiance leaving the Lambertian planet floor: sunlight attenuated by the atmosphere on
// the way down, plus the skylight the floor actually receives, both reflected by the
// ground albedo.
//
// Both terms are irradiance through the same p/PI Lambertian BRDF:
//   direct    E_sun = (n.l) * transmittance * sunColor * sunIntensity  ->  p/PI * E_sun
//   skylight  E_sky = PI * Lbar (definition above)                     ->  p * Lbar
// which is why the skylight term carries neither a 1/PI nor a tuning fraction: for a
// uniform sky of radiance L a Lambertian ground of albedo p leaves exactly p*L, so any
// constant in front of Lbar is an energy error rather than a dial.
//
// Energy-bounded by construction -- the albedo multiplies an irradiance the surface
// actually receives, so the result cannot exceed albedo x incident.
//
// At night the primary source is the moon, so the direct term is the moonlit floor and the
// skylight term the moonlit sky's. The night sky's own glow (airglow and starlight, which
// GE_ApplySkyGroundAndNight adds over the LUT) is not in the LUT the skylight integrates,
// so its share of the floor is added on the same nightAmount: groundNightColor, the floor's
// radiance under that glow alone. It is a radiance in its own right (the stylized path
// writes it straight into skyColor), so like there it is NOT scaled by groundBrightness.
vec3 GE_GroundBounceRadiance(vec3 groundAlbedo,
                             float groundBrightness,
                             vec3 surfaceUp,
                             vec3 sunDir,
                             vec3 transToSun,
                             vec3 sunColor,
                             float sunIntensity,
                             vec3 skyDiffuseMeanRadiance,
                             vec3 groundNightColor,
                             float nightAmount)
{
    float NdotL   = max(dot(surfaceUp, sunDir), 0.0);
    vec3  direct  = groundAlbedo * (1.0 / GE_SKY_PI) * NdotL * transToSun * sunColor * sunIntensity;
    vec3  ambient = groundAlbedo * skyDiffuseMeanRadiance;
    vec3  lit     = (direct + ambient) * max(groundBrightness, 0.0);
    return lit + groundNightColor * clamp(nightAmount, 0.0, 1.0);
}

// Composites the lit planet floor below the horizon over `skyColor`, hazed into the
// horizon backdrop by aerial perspective.
//
// ONE implementation of the ground-bounce MATH, shared by the VISIBLE sky
// (sky_render.frag) and the IBL capture (sky_capture_cube.frag), so the two cannot drift
// apart in what a lit floor is worth. WHETHER a call site composites a floor is the call
// site's decision and the two deliberately differ: the capture bakes the bounce for every
// physical below-horizon mode, while the visible sky paints a floor only in PlanetGround
// and otherwise folds the horizon haze down. No scene in the repo pins PlanetGround, so
// in practice the shipped configuration is exactly that split -- baked bounce, folded
// visible backdrop -- and it is intended, not a parity bug.
//
// Returns `skyColor` untouched when the ray misses the planet.
vec3 GE_CompositeGroundFloor(vec3 skyColor,
                             sampler2D transLUT,
                             vec3 skyDiffuseMeanRadiance,
                             vec3 camPos,
                             vec3 dirWS,
                             vec3 planetCenter,
                             float planetRadius,
                             float atmosphereRadius,
                             float camR,
                             float viewZenithCos,
                             vec3 groundAlbedo,
                             float groundBrightness,
                             vec3 sunDir,
                             vec3 sunColor,
                             float sunIntensity,
                             vec3 betaRayleigh,
                             float rayleighScaleHeight,
                             float groundHazeStrength,
                             vec3 groundNightColor,
                             float nightAmount)
{
    vec3  upAtHit;
    float distToFloor;

    // On the sphere the ray-sphere solve is DEGENERATE and must not be used. For a camera
    // at camR == planetRadius, every below-horizon direction gives
    //   tca = R*sin(theta),  thc = sqrt(R^2 - R^2*cos^2(theta)) = R*sin(theta)
    //   t0  = tca - thc = 0  exactly
    // so a `t0 > 0` guard rejects the whole lower hemisphere and float32 ULP noise (~0.5
    // at 6.4e6) is left deciding which directions composite a floor at all. The IBL
    // capture renders from the world origin, which is exactly on the sphere, so this is
    // its normal case and not an edge case. Handle it analytically: on the surface every
    // below-horizon ray meets the ground at distance ~0, the surface normal is the
    // camera's own up, and there is no haze column to cross.
    // The tolerance is RELATIVE to the planet radius, because what it has to survive is
    // float32 resolution at that radius, not a distance in metres. camR reaches this
    // function through a length(), and SPIR-V permits 3 ULP in sqrt: at R = 6.36e6 one
    // ULP is ~0.5 m, so a fixed 1 m band is ~2 ULP and a conformant driver may land camR
    // outside it while the camera is exactly on the surface -- silently restoring the
    // degenerate intersection this branch exists to avoid. 8e-7 * R is ~5.1 m at Earth
    // scale, a bit over 10 ULP.
    //
    // Cost of the wider band, stated so it is not discovered later: inside it the floor
    // composites with distToFloor = 0, i.e. no aerial perspective. A ray straight down
    // from 5 m travels 5 m and loses nothing; the worst case is a grazing ray, which at
    // that altitude reaches the ground ~8 km away, where the measured extinction
    // (~1.35e-5 /m) would have mixed in ~10% haze. The 1 m band already made the same
    // trade at ~5%, and horizonBlend fades exactly that grazing band in.
    const float kGroundFloorSurfaceToleranceRelative = 8e-7;
    float surfaceTolerance = planetRadius * kGroundFloorSurfaceToleranceRelative;

    // At or BELOW the surface takes the analytic branch deliberately: underground there
    // is no sky below the horizon to show, and the capture -- which is the reason this
    // path exists -- renders from a camera the fill code places exactly on the sphere.
    // The one degenerate input left is a camera at the planet centre, where the up
    // direction is undefined; pick world up rather than emit normalize(0).
    if (camR - planetRadius <= surfaceTolerance)
    {
        vec3  toCam    = camPos - planetCenter;
        float toCamLen = length(toCam);
        upAtHit     = toCamLen > 0.0 ? toCam / toCamLen : vec3(0.0, 1.0, 0.0);
        distToFloor = 0.0;
    }
    else
    {
        float tg0, tg1;
        if (!GE_RaySphereIntersect(camPos, dirWS, planetCenter, planetRadius, tg0, tg1) || tg0 <= 0.0)
            return skyColor;
        upAtHit     = normalize((camPos + dirWS * tg0) - planetCenter);
        distToFloor = tg0;
    }

    // Sun shadowing from the transmittance LUT at the surface (the hit point is on the
    // sphere), 0 once the sun is below the hit point's horizon.
    vec3  transToSun = GE_SkyTransmittanceToTop(transLUT, planetRadius, dot(upAtHit, sunDir),
                                                planetRadius, atmosphereRadius);

    vec3 floorColor = GE_GroundBounceRadiance(groundAlbedo, groundBrightness, upAtHit, sunDir,
                                              transToSun, sunColor, sunIntensity, skyDiffuseMeanRadiance,
                                              groundNightColor, nightAmount);

    // Grey view-ray extinction derived from the configured atmosphere at the camera
    // altitude, so distance haze scales with the planet rather than a fixed constant.
    // At distToFloor == 0 this is exp(0) == 1 -- the floor outright -- so the analytic
    // branch needs no separate compositing path.
    float camDensity       = exp(-(camR - planetRadius) / rayleighScaleHeight);
    float aerialExtinction = dot(betaRayleigh, vec3(0.299, 0.587, 0.114)) *
                             camDensity * max(groundHazeStrength, 0.0);
    vec3  lit = GE_AerialPerspectiveComposite(skyColor, floorColor, distToFloor, aerialExtinction);

    // Fade the floor in across the geometric horizon so it never pops as a hard line,
    // independent of camera altitude.
    float horizonBlend = smoothstep(-0.004, 0.0, -viewZenithCos);
    return mix(skyColor, lit, horizonBlend);
}

#endif
