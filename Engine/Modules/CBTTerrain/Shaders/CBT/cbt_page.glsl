// cbt_page.glsl: the paged height resolve. A terrain's height field is a pyramid of 128-sample
// pages (130 with the apron) held in one physical cache texture; a page table maps each page of
// every level to the cache slot holding it, dense, or tiered past the dense budget (CBT_PageTableEntry,
// the walk of TerrainECS PageTable). The resolve samples level L = floor(lambda) and blends
// toward its parent over the last quarter of the level's ring; a level whose page is not resident
// resolves to its first resident ancestor alone (the fallback walk), and a page that has just
// arrived fades in from its parent.
//
// A position is a CBTPagePoint: (face, level-0 texel, fraction) per axis. A planar terrain makes it
// from its UV (face 0); a planet's vertex makes it from its exact cube numerator (cbt_deep_decode.glsl
// CBT_PlanetPagePoint). Both share the walk below.
//
// LOCKSTEP with the CPU mirror PagedHeightSampler (Engine/Modules/TerrainECS/.../PagedHeightSampler.
// {h,cpp}): the same walk, the same order of operations. The mirror locates a texel from the level's
// lattice coordinate, this file from (texel, fraction); both forms are exact, so the two agree bit for
// bit on the same texels. The block between the GE_SHARED_PAGE_RESOLVE markers is compiled as C++ by
// TerrainAtlasTests (GlslShim.h) and swept against the mirror: write it component-wise (no
// swizzles), float literals suffixed `f`, no out parameters.
//
// Pure math, no bindings. The consumer defines the four taps declared below, after including this
// file (GLSL resolves a prototype at link):
//   uint CBT_PageTableWord(uint index);        // word `index` of the table's words (PageTable::Words)
//   float CBT_PageSlotFade(uint slot);         // the arrival fade of a slot: 0 just assigned, 1 settled
//   CBTPageLevel CBT_PageLevelShape(uint level);
//   float CBT_PageCacheBilinear(float texelX, float texelZ);
// The last one reads the cache bilinearly at absolute texel coordinates (an integer is a texel's
// centre): on the GPU a linear-clamp sampler at (texel + 0.5) / CacheDim. The page's apron keeps
// every tap inside its slot, so the sampler never blends a neighboring slot in.
#ifndef CBT_PAGE_GLSL
#define CBT_PAGE_GLSL

// GE_SHARED_PAGE_TYPES_BEGIN
// One level of the pyramid (mirror PageStreaming::PageStoreLevel's page counts). std430, 16 B.
struct CBTPageLevel
{
    uint FirstEntry; // the level's first entry in the table
    uint PagesX;
    uint PagesZ;
    uint Pad;
};

// The field a resolve reads (mirror PageTable's level-0 shape and tiers, and PageCacheGeometry).
// std430, 32 B.
struct CBTPageField
{
    uint Level0SamplesX;
    uint Level0SamplesZ;
    uint LevelCount;  // 0: no field, the resolve answers 0
    uint SlotsPerRow; // the cache texture's slots per row (square-packed)
    uint FloorLevel;  // the finest dense level: 0 for a dense table
    uint TierCount;   // the tiers of blocks below the floor
    uint DirectoryOffset; // the floor's directory: one block index per floor page
    uint Pad;
};

// A position on a field: its face (0 on a planar field), and along each axis the level-0 texel and
// the fraction past it in [0, 1).
struct CBTPagePoint
{
    uint Face;
    uint TexelX;
    uint TexelZ;
    float FractionX;
    float FractionZ;
};

// A level's sample and the resident level that answered it.
struct CBTPageLevelSample
{
    float Height;
    uint Level; // LevelCount when no page down the walk is resident
};
// GE_SHARED_PAGE_TYPES_END

// The consumer's taps (see the header).
uint CBT_PageTableWord(uint index);
float CBT_PageSlotFade(uint slot);
CBTPageLevel CBT_PageLevelShape(uint level);
float CBT_PageCacheBilinear(float texelX, float texelZ);

// The normalized coordinate of absolute cache texel (texelX, texelZ) in a cache of `cacheDim`
// texels a side: what a consumer's CBT_PageCacheBilinear hands its linear-clamp sampler.
vec2 CBT_PageCacheUV(float texelX, float texelZ, vec2 cacheDim)
{
    return (vec2(texelX, texelZ) + 0.5) / cacheDim;
}

// GE_SHARED_PAGE_RESOLVE_BEGIN
// Mirror PageStreaming::kNoPage, kPageSlotMask, kPageOwnedSamples, kPageApronSamples, kPageStrideSamples.
const uint CBT_PAGE_NO_PAGE = 0xFFFFFFFFu;
const uint CBT_PAGE_SLOT_MASK = 0x00FFFFFFu;
const uint CBT_PAGE_OWNED = 128u;
const uint CBT_PAGE_OWNED_SHIFT = 7u; // log2(CBT_PAGE_OWNED)
const uint CBT_PAGE_APRON = 1u;
const uint CBT_PAGE_STRIDE = 130u;
// Mirror kNoPageBlock, kPageTableTierWords in TerrainECS PageTable.cpp.
const uint CBT_PAGE_NO_BLOCK = 0xFFFFFFFFu;
const uint CBT_PAGE_TIER_WORDS = 4u;

// Mirror PageParentBlend: 0 over the first three quarters of a level's ring, a linear ramp to 1
// over the last quarter.
float CBT_PageParentBlend(float level)
{
    float inRing = level - floor(level);
    return clamp((inRing - 0.75f) / (1.0f - 0.75f), 0.0f, 1.0f);
}

// The point of terrain UV (u, v) on a planar field: level-0 sample i sits at i / (samples - 1), so
// the level-0 lattice coordinate is clamp(t) x (samples - 1), split into its texel and fraction.
// The field has at least one level.
CBTPagePoint CBT_PagePointAtUV(float u, float v, CBTPageField field)
{
    float cx = clamp(u, 0.0f, 1.0f) * float(field.Level0SamplesX - 1u);
    float cz = clamp(v, 0.0f, 1.0f) * float(field.Level0SamplesZ - 1u);
    CBTPagePoint point;
    point.Face = 0u;
    point.TexelX = uint(cx);
    point.TexelZ = uint(cz);
    point.FractionX = cx - float(point.TexelX);
    point.FractionZ = cz - float(point.TexelZ);
    return point;
}

// The page of `level` along an axis of `pages` pages that holds level-0 texel `texel`: level-L
// sample j sits on level-0 sample j x 2^L, and a page owns CBT_PAGE_OWNED samples of its level.
uint CBT_PageIndex(uint texel, uint level, uint pages)
{
    return min(texel >> (level + CBT_PAGE_OWNED_SHIFT), pages - 1u);
}

// The position along an axis inside the stride of page `page` of `level` of level-0 texel `texel`
// plus `fraction`: the level's lattice coordinate past the page's first owned sample, plus the apron.
float CBT_PageStride(uint texel, float fraction, uint level, uint page)
{
    uint past = texel - (page << (level + CBT_PAGE_OWNED_SHIFT));
    return (float(past) + fraction) / float(1u << level) + float(CBT_PAGE_APRON);
}

// The first word of level k below a block's root (k from 1): 4 + 16 + ... + 4^(k-1).
uint CBT_PageBlockLevelOffset(uint k)
{
    return ((1u << (2u * k)) - 4u) / 3u;
}

// The entry of page (pageX, pageZ) of `level` on `face` (mirror PageTable::Entry; planar is face 0): one read
// on a dense level. Below the floor, the floor directory names a block; each tier's block holds the entry or
// names the next tier's block. CBT_PAGE_NO_BLOCK: no page under it is resident.
uint CBT_PageTableEntry(uint level, uint face, uint pageX, uint pageZ, CBTPageField field)
{
    CBTPageLevel shape = CBT_PageLevelShape(level);
    if (level >= field.FloorLevel)
        return CBT_PageTableWord(shape.FirstEntry + (face * shape.PagesZ + pageZ) * shape.PagesX + pageX);
    uint shift = field.FloorLevel - level;
    uint rootX = pageX >> shift;
    uint rootZ = pageZ >> shift;
    CBTPageLevel floorShape = CBT_PageLevelShape(field.FloorLevel);
    uint floorIndex = (face * floorShape.PagesZ + rootZ) * floorShape.PagesX + rootX;
    uint block = CBT_PageTableWord(field.DirectoryOffset + floorIndex);
    for (uint tier = 0u; tier < field.TierCount; ++tier)
    {
        if (block == CBT_PAGE_NO_BLOCK)
            return CBT_PAGE_NO_PAGE;
        uint rootLevel = CBT_PageTableWord(tier * CBT_PAGE_TIER_WORDS);
        uint levels = CBT_PageTableWord(tier * CBT_PAGE_TIER_WORDS + 1u);
        uint base = CBT_PageTableWord(tier * CBT_PAGE_TIER_WORDS + 3u) +
                    block * CBT_PageTableWord(tier * CBT_PAGE_TIER_WORDS + 2u);
        uint deepest = rootLevel - levels;
        if (level >= deepest)
        {
            uint k = rootLevel - level;
            return CBT_PageTableWord(base + CBT_PageBlockLevelOffset(k) + (pageZ - (rootZ << k)) * (1u << k) +
                                     (pageX - (rootX << k)));
        }
        uint childX = pageX >> (deepest - level);
        uint childZ = pageZ >> (deepest - level);
        block = CBT_PageTableWord(base + CBT_PageBlockLevelOffset(levels + 1u) +
                                  (childZ - (rootZ << levels)) * (1u << levels) + (childX - (rootX << levels)));
        rootX = childX;
        rootZ = childZ;
    }
    return CBT_PAGE_NO_PAGE;
}

// The table entry of the page of `level` holding `point`.
uint CBT_PageEntryAt(CBTPagePoint point, uint level, CBTPageField field)
{
    CBTPageLevel shape = CBT_PageLevelShape(level);
    uint pageX = CBT_PageIndex(point.TexelX, level, shape.PagesX);
    uint pageZ = CBT_PageIndex(point.TexelZ, level, shape.PagesZ);
    return CBT_PageTableEntry(level, point.Face, pageX, pageZ, field);
}

// Level `level` at `point` from the page in `slot`: bilinear at the point's position inside the
// page's stride (its owned samples offset by the apron).
float CBT_PageSampleSlot(CBTPagePoint point, uint level, uint slot, CBTPageField field)
{
    CBTPageLevel shape = CBT_PageLevelShape(level);
    uint pageX = CBT_PageIndex(point.TexelX, level, shape.PagesX);
    uint pageZ = CBT_PageIndex(point.TexelZ, level, shape.PagesZ);
    float strideX = CBT_PageStride(point.TexelX, point.FractionX, level, pageX);
    float strideZ = CBT_PageStride(point.TexelZ, point.FractionZ, level, pageZ);
    float originX = float((slot % field.SlotsPerRow) * CBT_PAGE_STRIDE);
    float originZ = float((slot / field.SlotsPerRow) * CBT_PAGE_STRIDE);
    return CBT_PageCacheBilinear(originX + strideX, originZ + strideZ);
}

// Level `level` alone at `point`: its page when resident, faded in from its parent; else its first
// resident ancestor's. A fading page's parent may be fading too, so the walk carries the weight
// still owed to the levels above: each resident page takes `fade` of what is left, a settled page
// (or the top level) takes all of it.
CBTPageLevelSample CBT_PageSampleLevel(CBTPagePoint point, uint level, CBTPageField field)
{
    CBTPageLevelSample result;
    result.Height = 0.0f;
    result.Level = field.LevelCount;
    float remaining = 1.0f;
    for (uint walk = level; walk < field.LevelCount; ++walk)
    {
        uint entry = CBT_PageEntryAt(point, walk, field);
        if (entry == CBT_PAGE_NO_PAGE)
            continue; // the fallback walk: the first resident ancestor answers
        uint slot = entry & CBT_PAGE_SLOT_MASK;
        float own = CBT_PageSampleSlot(point, walk, slot, field);
        float fade = CBT_PageSlotFade(slot);
        if (result.Level == field.LevelCount)
            result.Level = walk;
        if (fade >= 1.0f || walk + 1u >= field.LevelCount)
        {
            result.Height = result.Height + remaining * own;
            return result;
        }
        result.Height = result.Height + remaining * fade * own;
        remaining = remaining * (1.0f - fade);
    }
    return result;
}

// The height at `point` at continuous level `lambda`: level L's sample blended toward level L + 1's
// by CBT_PageParentBlend, or the fallback ancestor alone when L is not resident.
float CBT_PageSamplePoint(CBTPagePoint point, float lambda, CBTPageField field)
{
    if (field.LevelCount == 0u)
        return 0.0f;
    uint top = field.LevelCount - 1u;
    float clamped = clamp(lambda, 0.0f, float(top));
    uint base = min(uint(clamped), top);
    CBTPageLevelSample own = CBT_PageSampleLevel(point, base, field);
    float w = base < top ? CBT_PageParentBlend(clamped) : 0.0f;
    if (w <= 0.0f || own.Level > base)
        return own.Height; // the fallback ancestor answers alone: its own parent is coarser still
    CBTPageLevelSample parent = CBT_PageSampleLevel(point, base + 1u, field);
    return own.Height + (parent.Height - own.Height) * w;
}

// The height at terrain UV (u, v) of a planar field at continuous level `lambda`.
float CBT_PageSample(float u, float v, float lambda, CBTPageField field)
{
    if (field.LevelCount == 0u)
        return 0.0f;
    return CBT_PageSamplePoint(CBT_PagePointAtUV(u, v, field), lambda, field);
}
// GE_SHARED_PAGE_RESOLVE_END

#endif // CBT_PAGE_GLSL
