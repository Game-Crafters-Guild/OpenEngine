#include "TerrainECS/PagedHeightSampler.h"

#include "Mathematics/Interpolation.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/TerrainAtlas.h"

#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageTableEntry.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainECS
{

using PageStreaming::kNoPage;
using PageStreaming::kPageApronSamples;
using PageStreaming::kPageOwnedSamples;
using PageStreaming::kPageStrideSamples;

PageCacheGeometry MakePageCacheGeometry(uint32 slotCount)
{
    PageCacheGeometry geometry;
    geometry.SlotsPerRow = std::max(1u, static_cast<uint32>(std::ceil(std::sqrt(static_cast<float64>(slotCount)))));
    geometry.CacheDim = geometry.SlotsPerRow * kPageStrideSamples;
    return geometry;
}

void PageTable::Reset(uint32 samplesX, uint32 samplesZ, uint32 slotCount)
{
    Levels = PageStreaming::BuildPageStoreLevels(samplesX, samplesZ);
    const std::size_t entries = Levels.empty() ? 0u
                                               : static_cast<std::size_t>(Levels.back().FirstEntry) +
                                                     Levels.back().PagesX * Levels.back().PagesZ;
    Entries.assign(entries, kNoPage);
    SlotFade.assign(slotCount, 1.0f);
}

uint32 PageTable::Entry(uint32 level, uint32 pageX, uint32 pageZ) const
{
    const PageStreaming::PageStoreLevel& shape = Levels[level];
    return Entries[shape.FirstEntry + static_cast<std::size_t>(pageZ) * shape.PagesX + pageX];
}

void PageTable::SetEntry(uint32 level, uint32 pageX, uint32 pageZ, uint32 entry)
{
    const PageStreaming::PageStoreLevel& shape = Levels[level];
    Entries[shape.FirstEntry + static_cast<std::size_t>(pageZ) * shape.PagesX + pageX] = entry;
}

uint32 UpdatePageTable(const PageStreaming::PageCache& cache, uint32 stream, PageTable& table)
{
    uint32 writes = 0;
    for (const PageStreaming::CachedPage& page : cache.Transitions())
    {
        if (page.Stream != stream)
            continue;
        const uint32 slot = cache.SlotOf(page);
        const bool resident = slot != PageStreaming::kNoResidentSlot;
        const PageStreaming::PageAddress& address = page.Address;
        table.SetEntry(address.Level, address.X, address.Z,
                       resident ? PageStreaming::PackPageEntry(slot, address.Level) : kNoPage);
        if (resident)
            table.SlotFade[slot] = cache.FadeOf(page);
        ++writes;
    }
    for (const PageStreaming::CachedPage& page : cache.FadeChanges())
    {
        const uint32 slot = cache.SlotOf(page);
        if (page.Stream != stream || slot == PageStreaming::kNoResidentSlot)
            continue;
        table.SlotFade[slot] = cache.FadeOf(page);
        ++writes;
    }
    return writes;
}

namespace
{

// The continuous level-`level` lattice coordinate of terrain coordinate t in [0, 1] along an axis of
// `level0Samples`: level-0 sample i sits at i / (samples - 1), level-L sample j at level-0 j * 2^L.
float32 LatticeCoordinate(float32 t, uint32 level0Samples, uint32 level)
{
    return Math::Clamp01(t) * static_cast<float32>(level0Samples - 1u) / static_cast<float32>(1u << level);
}

// The page along an axis holding lattice coordinate c, and c's position inside the page's stride.
void PageAndStride(float32 c, uint32 pages, uint32& page, float32& stride)
{
    page = std::min(static_cast<uint32>(c) / kPageOwnedSamples, pages - 1u);
    stride = c - static_cast<float32>(page * kPageOwnedSamples) + static_cast<float32>(kPageApronSamples);
}

} // namespace

PageTexel LocatePageTexel(std::span<const PageStreaming::PageStoreLevel> levels, uint32 level, float32 u, float32 v)
{
    const PageStreaming::PageStoreLevel& shape = levels[level];
    PageTexel texel;
    PageAndStride(LatticeCoordinate(u, levels.front().SamplesX, level), shape.PagesX, texel.PageX, texel.StrideX);
    PageAndStride(LatticeCoordinate(v, levels.front().SamplesZ, level), shape.PagesZ, texel.PageZ, texel.StrideZ);
    return texel;
}

float32 PagedHeightSampler::SampleResidentPage(const PageTexel& texel, uint32 slot) const
{
    const float32 originX = static_cast<float32>(Geometry.TexelX(slot, 0));
    const float32 originZ = static_cast<float32>(Geometry.TexelZ(slot, 0));
    return SampleGridBilinearTexel(Cache, Geometry.CacheDim, Geometry.CacheDim, originX + texel.StrideX,
                                   originZ + texel.StrideZ);
}

float32 PagedHeightSampler::SampleLevel(float32 u, float32 v, uint32 level, uint32& outLevel) const
{
    // A fading page's parent may be fading too: each resident page down the walk takes `fade` of the
    // weight still owed to the levels above, a settled page (or the top level) takes all of it. The
    // order of operations is cbt_page.glsl's CBT_PageSampleLevel, so the two agree bit for bit.
    const uint32 levels = static_cast<uint32>(Table->Levels.size());
    float32 height = 0.0f;
    float32 remaining = 1.0f;
    outLevel = levels;
    for (uint32 walk = level; walk < levels; ++walk)
    {
        const PageTexel texel = LocatePageTexel(Table->Levels, walk, u, v);
        const uint32 entry = Table->Entry(walk, texel.PageX, texel.PageZ);
        if (entry == kNoPage)
            continue; // the fallback walk: the first resident ancestor answers
        const uint32 slot = PageStreaming::PageEntrySlot(entry);
        const float32 own = SampleResidentPage(texel, slot);
        const float32 fade = Table->SlotFade[slot];
        if (outLevel == levels)
            outLevel = walk;
        if (fade >= 1.0f || walk + 1u >= levels)
            return height + remaining * own;
        height = height + remaining * fade * own;
        remaining = remaining * (1.0f - fade);
    }
    return height;
}

float32 PagedHeightSampler::Sample(float32 u, float32 v, float32 level) const
{
    if (!Table || !Cache || Table->Levels.empty())
        return 0.0f;
    const uint32 top = static_cast<uint32>(Table->Levels.size()) - 1u;
    const float32 clamped = std::clamp(level, 0.0f, static_cast<float32>(top));
    const uint32 base = std::min(static_cast<uint32>(clamped), top);
    uint32 resolved = 0;
    const float32 own = SampleLevel(u, v, base, resolved);
    const float32 w = base < top ? PageParentBlend(clamped) : 0.0f;
    if (w <= 0.0f || resolved > base)
        return own; // the fallback ancestor answers alone: its own parent is coarser still
    uint32 parentLevel = 0;
    const float32 parent = SampleLevel(u, v, base + 1u, parentLevel);
    return own + (parent - own) * w;
}

} // namespace GameEngine::TerrainECS
