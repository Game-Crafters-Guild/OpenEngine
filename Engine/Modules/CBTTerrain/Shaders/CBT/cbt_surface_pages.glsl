// The surface planes of a paged terrain at a point, from one locate (CBT_SurfPagesAtPoint) that the
// terrain fragment and the grass placement share. Off GE_COMPAT_PROFILE only: the includer declares
// gPageTable (height table, then surface table), gPageCache and the plane caches.
#ifndef CBT_SURFACE_PAGES_GLSL
#define CBT_SURFACE_PAGES_GLSL

// The surface planes of a paged terrain at a point. Resolved false: no page answers there.
struct CBTSurfacePages
{
    bool Resolved;
    vec4 Splat;
    vec2 NormalXZ;
    vec2 Grass;
    vec4 Color;
};

CBTSurfacePages CBT_SurfPagesNone()
{
    CBTSurfacePages pages;
    pages.Resolved = false;
    pages.Splat = vec4(0.0);
    pages.NormalXZ = vec2(0.0);
    pages.Grass = vec2(1.0);
    pages.Color = vec4(0.0);
    return pages;
}

#if !defined(GE_COMPAT_PROFILE)
#include "cbt_page.glsl"

// The plane bits of a surface table's plane mask (mirror CBTLayout.h kCBTSurfacePlane*).
const uint CBT_SURF_PLANE_SPLAT = 1u;
const uint CBT_SURF_PLANE_NORMAL = 2u;
const uint CBT_SURF_PLANE_GRASS = 4u;
const uint CBT_SURF_PLANE_COLOR = 8u;

// The first word of the table the taps read: 0 for the height table, the surface table's first word
// during a surface locate and its fetch.
uint gCbtPageRegion = 0u;

uint CBT_PageTableWord(uint index)
{
    return gPageTable.Words[gCbtPageRegion + CBT_PAGE_FADE_OFFSET + gPageTable.Words[gCbtPageRegion + 4u] + index];
}

float CBT_PageSlotFade(uint slot)
{
    return uintBitsToFloat(gPageTable.Words[gCbtPageRegion + CBT_PAGE_FADE_OFFSET + slot]);
}

// The update's VertexEval counts the walks that find no page; these consumers read the same table.
void CBT_PageUnresolved()
{
}

CBTPageLevel CBT_PageLevelShape(uint level)
{
    uint at = gCbtPageRegion + CBT_PAGE_HEADER_WORDS + 4u * level;
    CBTPageLevel shape;
    shape.FirstEntry = gPageTable.Words[at];
    shape.PagesX = gPageTable.Words[at + 1u];
    shape.PagesZ = gPageTable.Words[at + 2u];
    shape.Pad = 0u;
    return shape;
}

float CBT_PageCacheBilinear(float texelX, float texelZ)
{
    return textureLod(gPageCache, CBT_PageCacheUV(texelX, texelZ, vec2(textureSize(gPageCache, 0))), 0.0).r;
}

// The header of the table at word `base` of the slot.
CBTPageField CBT_SurfPageField(uint base)
{
    CBTPageField field;
    field.Level0SamplesX = gPageTable.Words[base];
    field.Level0SamplesZ = gPageTable.Words[base + 1u];
    field.LevelCount = gPageTable.Words[base + 2u];
    field.SlotsPerRow = gPageTable.Words[base + 3u];
    field.FloorLevel = gPageTable.Words[base + 5u];
    field.TierCount = gPageTable.Words[base + 6u];
    field.DirectoryOffset = gPageTable.Words[base + 7u];
    field.Pad = 0u;
    return field;
}

// GE_SHARED_SURFACE_PAGE_LEVEL_BEGIN
// The world size of one pixel from the position's steps along screen x and y: length(fwidth(position)).
float CBT_SurfPixelFootprint(vec3 dPdx, vec3 dPdy)
{
    return length(vec3(abs(dPdx.x) + abs(dPdy.x), abs(dPdx.y) + abs(dPdy.y), abs(dPdx.z) + abs(dPdy.z)));
}

// The continuous level a surface locate reads for a pixel `footprint` meters across: the level of one
// level-0 sample a pixel, at the larger of the two axis spacings of a terrain `worldSizeX` x
// `worldSizeZ` meters with `samplesX` x `samplesZ` level-0 samples (the requester's rule).
float CBT_SurfPageLevel(float footprint, float worldSizeX, float worldSizeZ, uint samplesX, uint samplesZ)
{
    float intervalsX = float((samplesX < 2u ? 2u : samplesX) - 1u);
    float intervalsZ = float((samplesZ < 2u ? 2u : samplesZ) - 1u);
    float spacing = max(worldSizeX / intervalsX, worldSizeZ / intervalsZ);
    return log2(max(footprint / spacing, 1.0f));
}
// GE_SHARED_SURFACE_PAGE_LEVEL_END

// One plane's bilinear tap at absolute texel (x, z) of its cache texture.
vec4 CBT_SurfPlaneTap(sampler2D plane, float texelX, float texelZ)
{
    return textureLod(plane, CBT_PageCacheUV(texelX, texelZ, vec2(textureSize(plane, 0))), 0.0);
}

// `pages` plus `weight` of every plane in `planes` from the page of `level` in `slot` at `point`.
void CBT_SurfPagesAdd(inout CBTSurfacePages pages, CBTPagePoint point, uint level, uint slot, float weight,
                      uint planes, CBTPageField field)
{
    CBTPageOwned owned = CBT_PageOwnedAt(point, level);
    float x = CBT_PageSlotTexelX(owned, slot, CBT_PAGE_STRIDE, CBT_PAGE_APRON, field);
    float z = CBT_PageSlotTexelZ(owned, slot, CBT_PAGE_STRIDE, CBT_PAGE_APRON, field);
    float blockX = CBT_PageSlotTexelX(owned, slot, CBT_PAGE_BLOCK_STRIDE, CBT_PAGE_BLOCK_APRON, field);
    float blockZ = CBT_PageSlotTexelZ(owned, slot, CBT_PAGE_BLOCK_STRIDE, CBT_PAGE_BLOCK_APRON, field);
    pages.Splat += weight * CBT_SurfPlaneTap(cbt_SurfaceSplat, x, z);
    pages.NormalXZ += weight * (CBT_SurfPlaneTap(cbt_SurfaceNormal, blockX, blockZ).rg * 2.0 - 1.0);
    if ((planes & CBT_SURF_PLANE_GRASS) != 0u)
        pages.Grass += weight * CBT_SurfPlaneTap(cbt_SurfaceGrass, x, z).rg;
    if ((planes & CBT_SURF_PLANE_COLOR) != 0u)
        pages.Color += weight * CBT_SurfPlaneTap(cbt_SurfaceColor, x, z);
}

// The surface planes at page point `point` and level `lambda` of the surface table `field` at word `base`,
// planar or planet: one locate serves every plane in `planes`. A plane the terrain does not have reads as
// unpaged: grass (1, 1), color 0.
CBTSurfacePages CBT_SurfPagesAtPoint(CBTPagePoint point, float lambda, CBTPageField field, uint base, uint planes)
{
    CBTSurfacePages pages = CBT_SurfPagesNone();
    gCbtPageRegion = base;
    CBTPageLocation location = CBT_PageLocate(point, lambda, field);
    if (location.Count > 0u)
    {
        pages.Resolved = true;
        pages.Grass = (planes & CBT_SURF_PLANE_GRASS) != 0u ? vec2(0.0) : vec2(1.0);
        CBT_SurfPagesAdd(pages, point, location.Level0, location.Slot0, location.Weight0, planes, field);
        if (location.Count > 1u)
            CBT_SurfPagesAdd(pages, point, location.Level1, location.Slot1, location.Weight1, planes, field);
        if (location.Count > 2u)
            CBT_SurfPagesAdd(pages, point, location.Level2, location.Slot2, location.Weight2, planes, field);
        if (location.Count > 3u)
            CBT_SurfPagesAdd(pages, point, location.Level3, location.Slot3, location.Weight3, planes, field);
    }
    gCbtPageRegion = 0u;
    return pages;
}

// The surface planes at terrain UV `uv` of a paged planar terrain `worldSizeX` x `worldSizeZ` meters,
// for a pixel `footprint` meters across, from the surface table at word `base` (CBT_SurfPagesAtPoint).
CBTSurfacePages CBT_SurfPagesAt(vec2 uv, float footprint, float worldSizeX, float worldSizeZ, uint base, uint planes)
{
    CBTPageField field = CBT_SurfPageField(base);
    float lambda = CBT_SurfPageLevel(footprint, worldSizeX, worldSizeZ, field.Level0SamplesX, field.Level0SamplesZ);
    return CBT_SurfPagesAtPoint(CBT_PagePointAtUV(uv.x, uv.y, field), lambda, field, base, planes);
}

#endif // !GE_COMPAT_PROFILE

#endif // CBT_SURFACE_PAGES_GLSL
