// Terrain shadows from the sun-space clearance map (issue #2699).
//
// The map is a grid laid along the sun's horizontal direction: axis u points toward the sun, axis
// v across it, one line per v. Each sample stores its clearance: the height by which the terrain
// upstream (larger u) rises above the sun's ray through the sample, measured from the ground
// there. Positive clearance is shadow up to that height above the ground; negative clearance is
// the margin by which the ground clears the sun, so lit ground needs no self-shadow bias. Beside
// it each sample stores its occluder's distance: the horizontal distance (meters along u) to the
// terrain that sizes its penumbra (GE_TerrainShadowSampleValue: in shadow the caster of the shadow
// volume's top; lit, the caster that blocks the sun disc's lower edge most, whose margin then
// replaces the clearance, both negative).
//
// Along one line the top of the shadow volume is a suffix maximum:
//   top(k) = max over j > k of (h(j) - (j - k) * texel * tan(elevation))
// so the bake (terrain_shadow_bake.comp) is one scan per line, linear in the samples and
// independent of the sun's elevation, and a receiver needs one bilinear read and a compare. The scan
// carries a second maximum for the ray through the sun disc's lower edge, which finds the occluder
// of a lit receiver's penumbra: the largest margin under the center ray there is usually the ground
// just upwind, which says nothing about the ridge whose penumbra the receiver stands in.
//
// The penumbra is the uniform disc of the sun (angular radius a) cut by the edge ray over the
// occluder, the model the mesh shadows use (the cascades' PCSS disc, the ray-traced mask's cone).
// For a receiver whose margin under the shadow volume's top is m meters (vertically) and whose
// occluder is D meters away horizontally, the edge ray passes m * cos(e) from the receiver across
// the ray, and the sun's disc there has the radius r = (D / cos(e)) * tan(a): the receiver is fully
// shadowed at m * cos(e) >= r and fully lit at m * cos(e) <= -r. Across the penumbra the margin
// changes by 2 * D * tan(a) / cos^2(e) meters, and on level ground the penumbra is
// 2 * r / sin(e) meters wide.
//
// The block between the markers is compiled by the shaders and, through GlslShim, by
// CBTTerrainTests, so it uses only what the shim provides: no out parameters, no samplers.
// The includer defines, before this file:
//   vec2 GE_TerrainShadowReadClearance(vec2 gridCoord)   bilinear read of the map at a grid
//                                                         position in samples (clamped in range):
//                                                         x = margin, y = occluder distance
//   float GE_TerrainShadowReadGround(float x, float z)    the terrain's height at x, z relative to
//                                                         its base, clamped to the terrain's edge,
//                                                         the surface the terrain renders
// All positions share one frame (the grid's center and the terrain's corner are given in it);
// heights are meters above the terrain's base height.

// GE_SHARED_TERRAIN_SHADOW_BEGIN
struct GE_TerrainShadowGrid
{
    // The grid's center and the terrain's corner and extent, in the receivers' frame (x, z).
    float CenterX;
    float CenterZ;
    float TerrainX;
    float TerrainZ;
    float TerrainSizeX;
    float TerrainSizeZ;
    // The sun's horizontal direction (unit length): the u axis. v = (-SunZ, SunX).
    float SunX;
    float SunZ;
    // tan of the sun's elevation, meters of rise per meter along u.
    float TanElevation;
    // Meters between samples, and the first sample's u and v.
    float Texel;
    float UMin;
    float VMin;
    // Samples per line and lines in use.
    float SamplesU;
    float SamplesV;
};

// A sample with no caster (outside the terrain), and the magnitude the map can hold (RG16F; the
// limit bounds both the clearance and the occluder distance, in meters).
const float kGE_TerrainShadowNoCaster = -1.0e30f;
const float kGE_TerrainShadowClearanceLimit = 60000.0f;
// The smallest penumbra radius (meters across the ray), so an occluder at the receiver (a facet's
// own terminator) gives a hard edge without dividing by zero.
const float kGE_TerrainShadowMinPenumbraRadius = 1.0e-4f;
// The penumbra radius bound for a caller whose meshes bound theirs by nothing (the ray-traced mask).
const float kGE_TerrainShadowUncappedPenumbra = 1.0e30f;
const float kGE_TerrainShadowPi = 3.14159265f;
const float kGE_TerrainShadowLn2 = 0.693147181f;
// The two grazing floors below are equal on purpose: both set the view's lowest elevation above the
// mean surface, about 0.9 degrees, one as a tangent and one as the cosine against the surface normal
// (the sine of the elevation). At that angle the two differ by about 0.01 %, so the footprint stops
// widening and the seen share stops counting at the same view.
//
// The lowest view elevation the seen share counts, as a tangent; its inverse caps the tangent of a
// view straight down.
const float kGE_TerrainShadowMinViewTan = 1.0f / 64.0f;
// The lowest view cosine the pixel footprint widens for, so a grazing pixel covers at most 64 times
// its head-on footprint: the terrain surface's floor on the grazing cosine (cbt_surface.glsl
// kCBTMinGrazingCos).
const float kGE_TerrainShadowMinViewCos = 1.0f / 64.0f;
// How fast the view's and the sun's masking stop selecting the same ground as their azimuths part, per
// radian: the fit of Ginneken, Stavridi and Koenderink (1998) used in the height-direction correlated
// masking-shadowing form (GE_TerrainShadowSeenShare).
const float kGE_TerrainShadowAzimuthCorrelation = 4.41f;

// The grid position, in samples, of the point (x, z).
vec2 GE_TerrainShadowGridCoord(GE_TerrainShadowGrid g, float x, float z)
{
    float dx = x - g.CenterX;
    float dz = z - g.CenterZ;
    float u = dx * g.SunX + dz * g.SunZ;
    float v = dz * g.SunX - dx * g.SunZ;
    return vec2((u - g.UMin) / g.Texel, (v - g.VMin) / g.Texel);
}

// The point (x, z) of the grid position `gridCoord` (in samples).
vec2 GE_TerrainShadowGridPoint(GE_TerrainShadowGrid g, vec2 gridCoord)
{
    float u = g.UMin + gridCoord.x * g.Texel;
    float v = g.VMin + gridCoord.y * g.Texel;
    return vec2(g.CenterX + u * g.SunX - v * g.SunZ, g.CenterZ + u * g.SunZ + v * g.SunX);
}

// 1 when (x, z) lies on the terrain (its casters and its ground), else 0.
float GE_TerrainShadowOnTerrain(GE_TerrainShadowGrid g, float x, float z)
{
    float tx = (x - g.TerrainX) / g.TerrainSizeX;
    float tz = (z - g.TerrainZ) / g.TerrainSizeZ;
    return (tx >= 0.0f && tx <= 1.0f && tz >= 0.0f && tz <= 1.0f) ? 1.0f : 0.0f;
}

// Sample k of `line`: x = the height that blocks the sun there (kGE_TerrainShadowNoCaster off the
// terrain or past the line's last sample), y = the ground its clearance is measured from (the
// terrain's height there, clamped to its edge off it).
vec2 GE_TerrainShadowCasterAndGround(GE_TerrainShadowGrid g, float k, float line)
{
    vec2 p = GE_TerrainShadowGridPoint(g, vec2(k, line));
    float ground = GE_TerrainShadowReadGround(p.x, p.y);
    float casts = (k <= g.SamplesU - 1.0f && GE_TerrainShadowOnTerrain(g, p.x, p.y) > 0.5f) ? 1.0f : 0.0f;
    return vec2(casts > 0.5f ? ground : kGE_TerrainShadowNoCaster, ground);
}

// A carry of the scan, seen from one sample: x = the shadow volume's top for the ray through the sun's
// center, y = the horizontal distance to the caster that sets it; z, w = the same for the ray through
// the sun disc's lower edge (slope `tanLower`). Of two carries seen from the same sample the higher top
// wins, ray by ray (the carries' maximum); on a tie `a` is kept, so every caller passes the nearer
// carry as `a` and a tie keeps the nearer caster.
vec4 GE_TerrainShadowHigherCarry(vec4 a, vec4 b)
{
    bool center = b.x > a.x;
    bool lower = b.z > a.z;
    return vec4(center ? b.x : a.x, center ? b.y : a.y, lower ? b.z : a.z, lower ? b.w : a.w);
}

// A caster's own carry at its sample: its height on both rays, at distance 0.
vec4 GE_TerrainShadowCasterCarry(float caster)
{
    return vec4(caster, 0.0f, caster, 0.0f);
}

// A carry seen `samplesDownwind` samples downwind of the sample it was seen from: each ray's top
// falls by its slope, and its caster recedes, per meter. The operator the line's runs are combined
// with, and one step of the walk.
vec4 GE_TerrainShadowCarryDownwind(GE_TerrainShadowGrid g, vec4 carry, float samplesDownwind, float tanLower)
{
    float meters = samplesDownwind * g.Texel;
    return vec4(carry.x - meters * g.TanElevation, carry.y + meters, carry.z - meters * tanLower, carry.w + meters);
}

// What the map stores at a sample whose ground is `ground`, from `seen`, the carry of every caster
// upwind of it: x = the margin, y = the distance of the occluder that sizes its penumbra. In shadow
// (the center ray's top above the ground) that is the center ray's caster: x = the clearance. Lit,
// it is the caster that blocks the disc's lower edge most, which the lit half of the penumbra
// depends on: x = that caster's margin under the center ray, at most the clearance and so never
// positive (each sample's sign, the hard compare at a sample, is unchanged; between samples the
// bilinear zero crossing can move by up to a texel where the two rays pick different casters). With a
// point sun (`tanLower` = tan(elevation)) both are the clearance.
// One caster per side is kept, the one with the largest vertical margin, which can be a farther caster
// than the one that hides the most of the disc: a near low occluder's umbra inside a far ridge's
// penumbra reads as the ridge's penumbra (measured on a 1.5 m mound inside a 30 m ridge's penumbra,
// sun 15 degrees, uncapped: 0.325 lit where the marched disc reads 0 at a 0.53 degree sun).
vec2 GE_TerrainShadowSampleValue(GE_TerrainShadowGrid g, vec4 seen, float ground, float tanLower)
{
    float clearance = seen.x - ground;
    vec2 value = vec2(clearance, seen.y);
    if (!(clearance > 0.0f))
        value = vec2(min(seen.z - seen.w * (g.TanElevation - tanLower) - ground, clearance), seen.w);
    return vec2(clamp(value.x, -kGE_TerrainShadowClearanceLimit, kGE_TerrainShadowClearanceLimit),
                min(value.y, kGE_TerrainShadowClearanceLimit));
}

// The shadow volume's top (height above the terrain's base) at the grid position `gridCoord`, as the
// map's bilinear read weighs its four samples: their clearance plus the ground each was measured
// from; y = the occluder distance there. Off the terrain that ground is the terrain's edge height
// clamped outward, which can kink (a cliff at the edge); interpolating the clearance against the
// receiver's own ground there would misplace the shadow along the whole kink line.
vec2 GE_TerrainShadowTop(GE_TerrainShadowGrid g, vec2 gridCoord)
{
    float i0 = floor(gridCoord.x);
    float j0 = floor(gridCoord.y);
    float fx = gridCoord.x - i0;
    float fy = gridCoord.y - j0;
    vec2 p00 = GE_TerrainShadowGridPoint(g, vec2(i0, j0));
    vec2 p10 = GE_TerrainShadowGridPoint(g, vec2(i0 + 1.0f, j0));
    vec2 p01 = GE_TerrainShadowGridPoint(g, vec2(i0, j0 + 1.0f));
    vec2 p11 = GE_TerrainShadowGridPoint(g, vec2(i0 + 1.0f, j0 + 1.0f));
    float g00 = GE_TerrainShadowReadGround(p00.x, p00.y);
    float g10 = GE_TerrainShadowReadGround(p10.x, p10.y);
    float g01 = GE_TerrainShadowReadGround(p01.x, p01.y);
    float g11 = GE_TerrainShadowReadGround(p11.x, p11.y);
    float ga = g00 + (g10 - g00) * fx;
    float gb = g01 + (g11 - g01) * fx;
    vec2 read = GE_TerrainShadowReadClearance(gridCoord);
    return vec2(read.x + ga + (gb - ga) * fy, read.y);
}

// The receiver at (x, y, z) against the shadow volume: x = its margin, the meters by which the
// volume's top passes above it (positive: the terrain hides the sun's center), y = the occluder
// distance. `onGround` is 1 for the terrain's own surface, which stands on the field whatever
// height its facet put it at, so its shadow does not follow the tessellation (one tap). Other
// receivers on the terrain keep their height, a base sunk into the terrain counting as on the
// ground (two taps); receivers off it (water beyond the terrain's edge) compare their height with
// the shadow's top (five).
vec2 GE_TerrainShadowMargin(GE_TerrainShadowGrid g, float x, float y, float z, float onGround)
{
    vec2 c = GE_TerrainShadowGridCoord(g, x, z);
    // Beside the grid or upwind of its last sample: no terrain between the receiver and the sun.
    if (c.y < 0.0f || c.y > g.SamplesV - 1.0f || c.x > g.SamplesU - 1.0f)
        return vec2(-kGE_TerrainShadowClearanceLimit, 0.0f);
    if (c.x < 0.0f)
    {
        // Downwind of the grid (off the terrain) nothing casts: the shadow volume's top falls by
        // tan(elevation) per meter from the grid's first sample on the receiver's line, and the
        // occluder recedes by the same meters.
        vec2 edge = GE_TerrainShadowTop(g, vec2(0.0f, c.y));
        float top = edge.x + c.x * g.Texel * g.TanElevation;
        return vec2(top - y, edge.y - c.x * g.Texel);
    }
    if (onGround > 0.5f)
        return GE_TerrainShadowReadClearance(c);
    if (GE_TerrainShadowOnTerrain(g, x, z) < 0.5f)
    {
        vec2 top = GE_TerrainShadowTop(g, c);
        return vec2(top.x - y, top.y);
    }
    float above = max(y - GE_TerrainShadowReadGround(x, z), 0.0f);
    vec2 read = GE_TerrainShadowReadClearance(c);
    return vec2(read.x - above, read.y);
}

// The share of the sun's disc that reaches the receiver at (x, y, z) past the terrain: 1 lit, 0 in
// full shadow. `tanHalfAngle` is tan of the sun's angular radius (0: a point sun, a hard edge, the
// one compare); `maxPenumbraRadius` bounds the penumbra's radius across the ray in meters, as the
// caller's mesh shadows bound theirs. See GE_TerrainShadowMargin for `onGround`.
float GE_TerrainShadowLit(GE_TerrainShadowGrid g, float x, float y, float z, float onGround, float tanHalfAngle,
                          float maxPenumbraRadius)
{
    vec2 margin = GE_TerrainShadowMargin(g, x, y, z, onGround);
    if (tanHalfAngle <= 0.0f)
        return margin.x > 0.0f ? 0.0f : 1.0f;
    float cosElevation = 1.0f / sqrt(1.0f + g.TanElevation * g.TanElevation);
    float radius = max(min(margin.y / cosElevation * tanHalfAngle, maxPenumbraRadius),
                       kGE_TerrainShadowMinPenumbraRadius);
    float t = margin.x * cosElevation / radius;
    if (t >= 1.0f)
        return 0.0f;
    if (t <= -1.0f)
        return 1.0f;
    // The share of a unit disc beyond a chord at signed distance t from its center.
    return (acos(t) - t * sqrt(1.0f - t * t)) / kGE_TerrainShadowPi;
}

// The lit field (TerrainShadowFieldLit, terrain_shadow_field_lit.comp) holds, per texel, the lit share
// of the map samples it covers and the mean over them of the occluder octaves: 0 for a lit sample, for
// a shadowed one log2 of its occluder's distance over one map texel, at least 0. A pixel reads it at
// the level of its footprint and splits the shadow it covers in two: the local shadow, cast by relief
// inside the footprint, which the view's own masking by that relief can hide or show, and the rest,
// cast from farther, which the view sees as it is.

// The share of the ground that lies in local shadow: shadowed by an occluder no farther than the
// footprint of `footprintSamples` map samples. `litShare` and `meanOctaves` are the field's two
// channels. The field keeps one moment of the occluder distances, so the share takes the shadowed
// samples' distances as spread evenly from 0 to the longest, the spread of a relief's own shadows. On
// that spread the mean lies 1 / ln 2 octaves below the longest, and the share below a footprint F is
// F over the longest: 0.37 at the mean, 1 at the longest. The share is a linear ramp in octaves chosen
// as a stand-in for that curve: 0 at 1 / ln 2 octaves below the mean, 0.5 at the mean, 1 at the
// longest.
float GE_TerrainShadowLocalShare(float litShare, float meanOctaves, float footprintSamples)
{
    float shadow = clamp(1.0f - litShare, 0.0f, 1.0f);
    if (shadow <= 0.0f)
        return 0.0f;
    float mean = meanOctaves / shadow;
    float footprint = log2(max(footprintSamples, 1.0f));
    return shadow * clamp((footprint - mean) * (0.5f * kGE_TerrainShadowLn2) + 0.5f, 0.0f, 1.0f);
}

// The share of the relief inside a footprint that the drawn surface no longer shows, at the terrain's
// continuous draw level `drawLevel` (cbt_height_chain.glsl CBT_ChainLevel; the receiver's distance
// times the level scale). The view's masking that hides lit ground (GE_TerrainShadowSeenShare below
// the field's share, the eye looking toward the sun) is counted only for relief the draw does not
// show. The ramp over draw levels 1 to 2 is a chosen trade, not derived from the view model. The draw
// reaches the chain's level 1 at draw level 1, and that level still draws a lattice's finest relief at
// its own sample spacing, where the field's lit share is noisy; taking lit ground away from a noisy
// share speckles a back-lit far band, which the ramp keeps clean.
float GE_TerrainShadowUndrawnRelief(float drawLevel)
{
    return clamp(drawLevel - 1.0f, 0.0f, 1.0f);
}

// The lit share of the ground a pixel sees. `litShare` is the lit share of all the ground its
// footprint covers and `localShadow` the part of that ground in local shadow
// (GE_TerrainShadowLocalShare). `tanView` is the tangent of the view's elevation above the mean
// surface and `tanSun` the tangent of the sun's elevation above the horizontal (the grid's
// TanElevation); `sunSide` is the cosine of the horizontal angle between the
// direction toward the eye and the direction toward the sun: 1 with the sun behind the eye, -1 with
// the eye looking into it.
//
// A distant eye sees the ground a sun at the eye's direction would light, so the relief hides from
// the view the share it would shadow from there. The relief is read from the local shadow as a field
// of symmetric grooves: the share they shadow at an elevation with tangent t is
// k / (k + t), k fixed by the local shadow at the sun's elevation, which gives the hidden share at the
// view's elevation. The view and the sun each see a share 1 / (1 + L) of the ground, L = hidden over
// seen (the sun's L = local / open, the view's = relief / clear), and the ground both see is the
// height-direction correlated form 1 / (1 + max(L) + c min(L)): both favor the highest ground, and
// the nearer their azimuths the more they favor the same ground. c = a / (a + 1 / phi), phi the
// horizontal angle between the eye and the sun and at most a quarter turn (pi / 2), a =
// kGE_TerrainShadowAzimuthCorrelation. With the sun behind the eye (c = 0) the view sees no shadowed
// ground while it is lower than the sun: it looks down the shadows. From the side c is 0.87, and a
// grazing view still sees mostly lit tops. With the eye looking into the sun the hidden ground is the
// lit ground first (the view sees the faces turned from the sun), mixed in from the side's value by
// -sunSide. Hidden ground outside the local shadow is taken from the lit and the far-shadowed ground
// in proportion. No local shadow (flat ground, a cast shadow from farther than the footprint) leaves
// the share unchanged.
float GE_TerrainShadowSeenShare(float litShare, float localShadow, float tanView, float tanSun, float sunSide)
{
    float local = clamp(localShadow, 0.0f, 1.0f);
    float open = 1.0f - local;
    if (local <= 0.0f || open <= 0.0f)
        return litShare;
    float relief = local * max(tanSun, 0.0f);
    float clear = open * max(tanView, kGE_TerrainShadowMinViewTan);
    float visible = clear / (relief + clear);
    float sunHidden = local / open;
    float viewHidden = relief / clear;
    float azimuth = min(acos(clamp(sunSide, -1.0f, 1.0f)), 0.5f * kGE_TerrainShadowPi);
    float correlation = kGE_TerrainShadowAzimuthCorrelation * azimuth /
                        (kGE_TerrainShadowAzimuthCorrelation * azimuth + 1.0f);
    // The ground outside the local shadow that the pixel sees, as a share of all the ground.
    float seenOpen = 1.0f / (1.0f + max(sunHidden, viewHidden) + correlation * min(sunHidden, viewHidden));
    if (sunSide < 0.0f)
        seenOpen = mix(seenOpen, max(open + visible - 1.0f, 0.0f), min(-sunSide, 1.0f));
    return clamp(litShare * seenOpen / (open * visible), 0.0f, 1.0f);
}

// The meters of ground a terrain pixel covers, for both shadow modes. `headOn` is the pixel's head-on
// footprint (its distance times the view ray's angle per pixel), `viewCos` the cosine between the view
// and the ground's mean surface, `drawLevel` the terrain's continuous draw level. A grazing pixel
// covers 1 / cos more ground along the view; the widening follows the draw's level rule
// (cbt_height_chain.glsl CBT_GrazingShare): none at draw level 0 and nearer, where the drawn relief
// decides what the pixel sees, all of it from draw level 1, and between draw levels 0 and 1 the
// widening is 1 / cos raised to the draw level.
float GE_TerrainShadowFootprint(float headOn, float viewCos, float drawLevel)
{
    float spread = 1.0f / max(viewCos, kGE_TerrainShadowMinViewCos);
    return headOn * pow(spread, clamp(drawLevel, 0.0f, 1.0f));
}

// The lit share the field shows a pixel. `read` is the field's two channels at the pixel's footprint,
// `samplesPerPixel` the footprint in map samples, `toEyeAcross` the horizontal part of the direction
// toward the eye (any length; zero for a view straight down), `viewCos` the cosine between the view
// and the ground's mean surface, `drawLevel` the terrain's continuous draw level. The field averages
// all the ground of the footprint, the ground the view's relief hides with it. Where the view sees
// more light than the field holds (it looks down the shadows), the seen share is shown wherever the
// field is read; where it sees less, only for the relief the draw no longer shows
// (GE_TerrainShadowUndrawnRelief). The two meet where the seen share equals the field's.
float GE_TerrainShadowFieldShown(GE_TerrainShadowGrid g, vec2 read, float samplesPerPixel, vec2 toEyeAcross,
                                 float viewCos, float drawLevel)
{
    float local = GE_TerrainShadowLocalShare(read.x, read.y, samplesPerPixel);
    float across = length(toEyeAcross);
    float sunSide = across > 0.0f ? dot(toEyeAcross / across, vec2(g.SunX, g.SunZ)) : 0.0f;
    float cosView = clamp(viewCos, 0.0f, 1.0f);
    // The view's elevation above the mean surface as a tangent, at most 64 (a view straight down).
    float tanView = cosView / max(sqrt(1.0f - cosView * cosView), cosView * kGE_TerrainShadowMinViewTan);
    float seen = GE_TerrainShadowSeenShare(read.x, local, tanView, g.TanElevation, sunSide);
    return seen >= read.x ? seen : mix(read.x, seen, GE_TerrainShadowUndrawnRelief(drawLevel));
}
// GE_SHARED_TERRAIN_SHADOW_END

#ifdef GE_TERRAIN_SHADOW_READS_FIELD
// The terrain's shadow for both directional-shadow modes (the raster term in shadow_sampling.glsl, the
// ray-traced mask in rt_shadow_mask.comp). An includer that defines GE_TERRAIN_SHADOW_READS_FIELD
// defines, before this file:
//   vec2 GE_TerrainShadowReadField(vec2 gridCoord, float level)   the lit field's two channels at a
//                                                                  grid position in map samples
//   float GE_TerrainShadowFieldLevels()                            the field's levels, 0 when none

// The share of the sun the terrain lets reach the receiver at (x, y, z), heights above the terrain's
// base. `headOn` is the pixel's head-on footprint in meters (0: not a ground pixel), `toEye` the unit
// direction toward the eye, `viewCos` the cosine between it and the ground's mean surface, `drawLevel`
// the terrain's continuous draw level there; the pixel covers GE_TerrainShadowFootprint of them, one
// rule for both modes. A pixel that covers more than one map sample reads the lit field at the level
// of its footprint: one read of the map takes one sample of the relief's shadow per pixel and aliases
// it into bands along the screen rows. The field fades in from one to two samples per pixel; from two
// on it is the whole answer and the map's own read is skipped. The field's share is what the view
// sees of it (GE_TerrainShadowFieldShown). `onGround`, `tanHalfAngle` and `maxPenumbraRadius` are
// GE_TerrainShadowLit's.
float GE_TerrainShadowFootprintLit(GE_TerrainShadowGrid g, float x, float y, float z, float onGround,
                                   float tanHalfAngle, float maxPenumbraRadius, float headOn, vec3 toEye,
                                   float viewCos, float drawLevel)
{
    float samplesPerPixel = GE_TerrainShadowFootprint(headOn, viewCos, drawLevel) / g.Texel;
    float levels = GE_TerrainShadowFieldLevels();
    float field = 1.0;
    float fieldWeight = 0.0;
    if (levels > 0.0 && samplesPerPixel > 1.0)
    {
        vec2 c = GE_TerrainShadowGridCoord(g, x, z);
        if (c.x >= 0.0 && c.y >= 0.0 && c.x <= g.SamplesU - 1.0 && c.y <= g.SamplesV - 1.0)
        {
            // Level 0 holds 2 x 2 samples.
            float level = clamp(log2(samplesPerPixel) - 1.0, 0.0, levels - 1.0);
            field = GE_TerrainShadowFieldShown(g, GE_TerrainShadowReadField(c, level), samplesPerPixel, toEye.xz,
                                               viewCos, drawLevel);
            fieldWeight = smoothstep(1.0, 2.0, samplesPerPixel);
        }
    }
    if (fieldWeight >= 1.0)
        return field;
    float lit = GE_TerrainShadowLit(g, x, y, z, onGround, tanHalfAngle, maxPenumbraRadius);
    return fieldWeight > 0.0 ? mix(lit, field, fieldWeight) : lit;
}
#endif
