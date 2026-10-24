// cbt_height_chain.glsl: the prefiltered height chain and the terrain draw's level rule.
//
// A far facet of the CBT spans many lattice cells, so its corners, which sample the level-0
// heights, alias a relief finer than the facet into a fan of facets along the horizon. The draw
// moves each vertex to the chain level its distance asks for instead: the surface prefiltered to
// the detail the screen can show there. The chain holds levels 1..top of the unified height texture
// (CBTLayout.h kCBTHeightChain*): level L + 1 has S / 2 + 1 samples along an axis where level L has
// S, and its sample j is the [1 2 1] / 4 tent of level L around sample 2j, edges clamped. These are
// the shapes and the filter of a paged terrain's store (PageStoreFormat, HeightPyramidBuilder), and
// the level rule is the page rule's (TerrainECS PageLevelRule): lambda = log2(d x t / (f x s0)).
//
// A vertex shared by several facets has the same UV and the same cached position in each of them
// (the integer barycentric walk), and the eye is one value per frame, so every copy computes the same
// level and the same height: the draw stays crack-free. Below lambda 0.75 the vertex keeps its cached
// level-0 height bit for bit, so the near field draws the level-0 surface exactly.
//
// The block between the GE_SHARED_HEIGHT_CHAIN markers is compiled as C++ by CBTTerrainTests
// (GlslShim.h): written component-wise, float literals suffixed `f`, no out parameters. Pure math, no
// bindings. The consumer defines the tap below after including this file:
//   float CBT_ChainLoad(uint word); // the chain buffer's word `word`, as a float
// A consumer that needs only the level rule (the surface) defines CBT_CHAIN_LEVEL_RULE_ONLY first and
// gets the addressing and the level functions without the tap.
#ifndef CBT_HEIGHT_CHAIN_GLSL
#define CBT_HEIGHT_CHAIN_GLSL

#ifndef CBT_CHAIN_LEVEL_RULE_ONLY
float CBT_ChainLoad(uint word);
#endif

// GE_SHARED_HEIGHT_CHAIN_BEGIN
// Mirror CBTLayout.h kCBTHeightChainHeaderWords and kCBTHeightChainMaxLevel.
const uint CBT_CHAIN_HEADER_WORDS = 4u;
const uint CBT_CHAIN_MAX_LEVEL = 15u;

// Samples along an axis of `level0Samples` at level `level` (mirror CBTHeightChainSamples).
uint CBT_ChainSamples(uint level0Samples, uint level)
{
    uint samples = level0Samples;
    for (uint l = 0u; l < level; ++l)
        samples = samples / 2u + 1u;
    return samples;
}

// The coarsest stored level (mirror CBTHeightChainTopLevel).
uint CBT_ChainTopLevel(uint samplesX, uint samplesZ)
{
    uint level = 0u;
    uint sx = samplesX;
    uint sz = samplesZ;
    while (level < CBT_CHAIN_MAX_LEVEL && (sx > 2u || sz > 2u))
    {
        sx = sx / 2u + 1u;
        sz = sz / 2u + 1u;
        level = level + 1u;
    }
    return level;
}

// The word of level `level`'s first sample, level >= 1 (mirror CBTHeightChainLevelWord).
uint CBT_ChainLevelWord(uint samplesX, uint samplesZ, uint level)
{
    uint word = CBT_CHAIN_HEADER_WORDS;
    uint sx = samplesX;
    uint sz = samplesZ;
    for (uint l = 1u; l < level; ++l)
    {
        sx = sx / 2u + 1u;
        sz = sz / 2u + 1u;
        word = word + sx * sz;
    }
    return word;
}

// The continuous level a vertex `distance` meters from the eye draws at: log2(distance x scale),
// scale = t / (f x s0) (mirror TerrainECS GeometryPageLevel). Negative is finer than level 0.
float CBT_ChainLevel(float distance, float scale)
{
    precise float edge = max(distance, 1.17549435e-38f) * scale;
    return log2(edge);
}

// The parent level's weight at continuous level `lambda`: 0 over the first three quarters of a
// level, a linear ramp to 1 over the last (mirror cbt_page.glsl CBT_PageParentBlend), so the surface
// reaches the next level exactly where the level ends and nothing pops as the eye moves.
float CBT_ChainParentBlend(float lambda)
{
    precise float inRing = lambda - floor(lambda);
    return clamp((inRing - 0.75f) / (1.0f - 0.75f), 0.0f, 1.0f);
}

// How a fragment at continuous level `lambda` takes its normal: 0 = the level-0 normal map alone (the
// near field and level 1's ring); 1 = the normal chain alone at mip lambda - 1, from level 2 on; between,
// over the last quarter of level 1, the weight of the chain's level-2 mip against the level-0 map. The
// surface never reads the chain's first mip.
float CBT_ChainNormalWeight(float lambda)
{
    if (!(lambda > 1.75f))
        return 0.0f;
    if (lambda >= 2.0f)
        return 1.0f;
    return (lambda - 1.75f) / (2.0f - 1.75f);
}

// The level from which a fragment shades the whole of its grazing footprint. A pixel that sees the
// ground at a grazing angle covers `spread` = 1 / cos more ground along the view than a head-on pixel
// at its distance. The draw shows the level-0 relief out to lambda 0.75 and reaches its first
// prefiltered level at lambda 1 (CBT_ChainDrawnHeight): nearer, the drawn relief itself decides what
// the pixel sees; from lambda 1 on the drawn surface is the mean, and the shading averages the ground
// the pixel covers. The footprint takes over across the level before that, a factor of two in view
// distance, so the shading changes over a distance band and never at a ring, which a field of ridges
// would cut into a staircase.
const float CBT_GRAZING_FULL_LEVEL = 1.0f;

// The share of the grazing footprint a fragment at continuous level `lambda` shades with: 0 at lambda
// 0 and nearer, 1 from CBT_GRAZING_FULL_LEVEL on, linear in the level between.
float CBT_GrazingShare(float lambda)
{
    return clamp(lambda / CBT_GRAZING_FULL_LEVEL, 0.0f, 1.0f);
}

// The level of the ground a fragment at `lambda` covers when it sees the ground with the grazing
// `spread` (>= 1): the level its normal is read at (CBT_ChainNormalWeight, cbt_surface.glsl).
float CBT_GrazingLevel(float lambda, float spread)
{
    return lambda + CBT_GrazingShare(lambda) * log2(spread);
}

#ifndef CBT_CHAIN_LEVEL_RULE_ONLY
// Level `level` (>= 1) at terrain (u, v), normalized: bilinear between the four lattice samples
// around the point. Level L's sample j sits on level-0 sample j x 2^L (cbt_page.glsl's lattice).
float CBT_ChainSampleLevel(float u, float v, uint level, uint samplesX, uint samplesZ)
{
    uint sx = CBT_ChainSamples(samplesX, level);
    uint sz = CBT_ChainSamples(samplesZ, level);
    precise float cx = clamp(u, 0.0f, 1.0f) * float(samplesX - 1u) / float(1u << level);
    precise float cz = clamp(v, 0.0f, 1.0f) * float(samplesZ - 1u) / float(1u << level);
    uint ix = min(uint(cx), sx - 2u);
    uint iz = min(uint(cz), sz - 2u);
    precise float fx = cx - float(ix);
    precise float fz = cz - float(iz);
    uint word = CBT_ChainLevelWord(samplesX, samplesZ, level) + iz * sx + ix;
    float h00 = CBT_ChainLoad(word);
    float h10 = CBT_ChainLoad(word + 1u);
    float h01 = CBT_ChainLoad(word + sx);
    float h11 = CBT_ChainLoad(word + sx + 1u);
    precise float h0 = h00 + (h10 - h00) * fx;
    precise float h1 = h01 + (h11 - h01) * fx;
    precise float h = h0 + (h1 - h0) * fz;
    return h;
}

// The height a vertex draws at, world: its cached level-0 height `cachedHeight` while lambda is at
// most 0.75 (the same bits), else level floor(lambda) blended toward the next over the last quarter
// of the level, clamped to the top level. Level 0 in a blend is the cached height. The chain is
// normalized: world = normalized x heightScale + originY, as the update's height sample.
float CBT_ChainDrawnHeight(float u, float v, float cachedHeight, float lambda, uint samplesX, uint samplesZ,
                           uint top, float heightScale, float originY)
{
    if (!(lambda > 0.75f) || top == 0u)
        return cachedHeight;
    float clamped = min(lambda, float(top));
    uint base = min(uint(clamped), top);
    float w = base < top ? CBT_ChainParentBlend(clamped) : 0.0f;
    precise float own = cachedHeight;
    if (base > 0u)
        own = CBT_ChainSampleLevel(u, v, base, samplesX, samplesZ) * heightScale + originY;
    if (!(w > 0.0f))
        return own;
    precise float parent = CBT_ChainSampleLevel(u, v, base + 1u, samplesX, samplesZ) * heightScale + originY;
    precise float drawn = own + (parent - own) * w;
    return drawn;
}

// The world normal (x, z; y = sqrt(1 - x^2 - z^2)) of chain level `level` (>= 1) at terrain (u, v): the
// central difference across one level-L sample each way, at world scale (the unified normal map's
// derivation, GenerateNormalmapRegionFromHeightfield). `cellX`, `cellZ` are the level-0 sample spacing
// (m); heights are normalized, scaled by `heightScale`.
vec2 CBT_ChainNormal(float u, float v, uint level, uint samplesX, uint samplesZ, float cellX, float cellZ,
                     float heightScale)
{
    float du = float(1u << level) / float(samplesX - 1u);
    float dv = float(1u << level) / float(samplesZ - 1u);
    float hL = CBT_ChainSampleLevel(u - du, v, level, samplesX, samplesZ);
    float hR = CBT_ChainSampleLevel(u + du, v, level, samplesX, samplesZ);
    float hD = CBT_ChainSampleLevel(u, v - dv, level, samplesX, samplesZ);
    float hU = CBT_ChainSampleLevel(u, v + dv, level, samplesX, samplesZ);
    float spanX = (min(u + du, 1.0f) - max(u - du, 0.0f)) * float(samplesX - 1u) * cellX;
    float spanZ = (min(v + dv, 1.0f) - max(v - dv, 0.0f)) * float(samplesZ - 1u) * cellZ;
    float dx = (hR - hL) * heightScale / max(spanX, 1.0e-6f);
    float dz = (hU - hD) * heightScale / max(spanZ, 1.0e-6f);
    float len = sqrt(dx * dx + 1.0f + dz * dz);
    return vec2(-dx / len, -dz / len);
}

#endif // CBT_CHAIN_LEVEL_RULE_ONLY
// GE_SHARED_HEIGHT_CHAIN_END

#endif // CBT_HEIGHT_CHAIN_GLSL
