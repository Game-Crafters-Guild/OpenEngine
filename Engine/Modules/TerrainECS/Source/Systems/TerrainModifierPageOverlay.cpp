// TerrainModifierSystem's height page overlay: what the modifiers add to a tiled terrain's base height
// pages (design page-streaming 3.8), a sparse HeightPageLayer, so a modified terrain renders every
// modifier from its first paged frame. Also the level-0 rect a bake changed on the surface, for the surface pages.

#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainRowBands.h"
#include "TerrainECS/TerrainSurface/TerrainSurfaceStore.h"

#include "Core/CpuProfiler.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace GameEngine::TerrainECS
{

namespace
{

using PageStreaming::PageSampleRect;

// Adds `changed` of `planes` to the surface change record of `tiled`, whose reader has taken
// the record when SurfaceChangedSince equals SurfaceVersion.
void AddSurfaceChange(TiledTerrainData& tiled, const PageSampleRect& changed, uint8 planes)
{
    const bool taken = tiled.SurfaceChangedSince == tiled.SurfaceVersion;
    tiled.SurfaceChanged = taken ? changed : PageStreaming::Union(tiled.SurfaceChanged, changed);
    tiled.SurfaceChangedPlanes = taken ? planes : static_cast<uint8>(tiled.SurfaceChangedPlanes | planes);
    ++tiled.SurfaceVersion;
}

// The terrain's global tile lattice: level 0 of its height pages.
struct TileLattice
{
    uint32 SamplesX = 0;
    uint32 SamplesZ = 0;
    float32 SpacingX = 0.0f;
    float32 SpacingZ = 0.0f;
    float32 OriginX = 0.0f;
    float32 OriginZ = 0.0f;
};

std::optional<TileLattice> MakeTileLattice(const TiledTerrainData& tiled)
{
    const uint32 tileW = tiled.Config.TileConfig.HeightmapWidth;
    const uint32 tileH = tiled.Config.TileConfig.HeightmapHeight;
    if (tileW < 2u || tileH < 2u || tiled.Config.TilesPerAxisX == 0u || tiled.Config.TilesPerAxisZ == 0u ||
        !(tiled.Config.TileWorldSize > 0.0f))
        return std::nullopt;
    TileLattice lattice;
    lattice.SamplesX = tiled.Config.TilesPerAxisX * (tileW - 1u) + 1u;
    lattice.SamplesZ = tiled.Config.TilesPerAxisZ * (tileH - 1u) + 1u;
    lattice.SpacingX = tiled.Config.TileWorldSize / static_cast<float32>(tileW - 1u);
    lattice.SpacingZ = tiled.Config.TileWorldSize / static_cast<float32>(tileH - 1u);
    lattice.OriginX = tiled.WorldOriginX;
    lattice.OriginZ = tiled.WorldOriginZ;
    return lattice;
}

// The lattice samples a world rect reaches, one sample of margin on each side (the bake's own
// sample mapping rounds both ways), or nothing when it misses the terrain.
std::optional<PageSampleRect> LatticeRect(const TileLattice& lattice, float32 minX, float32 minZ, float32 maxX,
                                          float32 maxZ)
{
    const float64 firstX = std::floor((minX - lattice.OriginX) / lattice.SpacingX) - 1.0;
    const float64 firstZ = std::floor((minZ - lattice.OriginZ) / lattice.SpacingZ) - 1.0;
    const float64 lastX = std::ceil((maxX - lattice.OriginX) / lattice.SpacingX) + 1.0;
    const float64 lastZ = std::ceil((maxZ - lattice.OriginZ) / lattice.SpacingZ) + 1.0;
    if (lastX < 0.0 || lastZ < 0.0 || firstX > lattice.SamplesX - 1.0 || firstZ > lattice.SamplesZ - 1.0)
        return std::nullopt;
    PageSampleRect rect;
    rect.MinX = static_cast<uint32>(std::max(firstX, 0.0));
    rect.MinZ = static_cast<uint32>(std::max(firstZ, 0.0));
    rect.MaxX = static_cast<uint32>(std::min(lastX, lattice.SamplesX - 1.0));
    rect.MaxZ = static_cast<uint32>(std::min(lastZ, lattice.SamplesZ - 1.0));
    return rect;
}

// True when rects `a` and `b` overlap or share a side.
bool RectsTouch(const PageSampleRect& a, const PageSampleRect& b)
{
    return a.MinX <= b.MaxX + 1u && b.MinX <= a.MaxX + 1u && a.MinZ <= b.MaxZ + 1u && b.MinZ <= a.MaxZ + 1u;
}

// `rects` with every pair that overlaps or touches replaced by their union, so the bands that compose
// them never compose one sample twice.
std::vector<PageSampleRect> MergeTouching(std::vector<PageSampleRect> rects)
{
    // Each pass grows a rect by every rect it touches; a grown rect can touch one it passed, so passes
    // repeat until none merges.
    for (bool merged = true; merged;)
    {
        merged = false;
        for (std::size_t kept = 0; kept < rects.size(); ++kept)
            for (std::size_t other = kept + 1; other < rects.size();)
            {
                if (!RectsTouch(rects[kept], rects[other]))
                {
                    ++other;
                    continue;
                }
                rects[kept] = Union(rects[kept], rects[other]);
                rects[other] = rects.back();
                rects.pop_back();
                merged = true;
            }
    }
    return rects;
}

// The level-0 bounds of what `layer` holds, or an empty rect at the origin when it holds nothing.
PageSampleRect ExtentOf(const PageStreaming::HeightPageLayer& layer)
{
    return layer.Extent(0).value_or(PageSampleRect{});
}

// Publishes `layer` as the terrain's overlay, `changed` the level-0 rect it changed from the
// previous one. The version is unique across every terrain's data, so the height pages tell an
// overlay derived from the one they hold from an overlay of rebuilt modifier data (a scene load
// replaces the terrain's data while its pages stay resident).
void PublishPageOverlay(TiledTerrainData& tiled, std::shared_ptr<PageStreaming::HeightPageLayer> layer,
                        const PageSampleRect& changed)
{
    static std::atomic<uint64> s_LastVersion{0};
    tiled.PageOverlay = std::move(layer);
    tiled.PageOverlayChanged = changed;
    tiled.PageOverlayChangedSince = tiled.PageOverlayVersion;
    tiled.PageOverlayVersion = ++s_LastVersion;
}

// Drops the terrain's overlay: it pages no more until a new one is built.
void ReleaseHeightPageOverlay(TiledTerrainData& tiled)
{
    if (tiled.PageOverlay)
        PublishPageOverlay(tiled, nullptr, ExtentOf(*tiled.PageOverlay));
    tiled.PageOverlayCurrent = false;
    tiled.PageOverlayRefusedBytes = 0;
    tiled.PageOverlayRefusedOffGrid = false;
}

} // namespace

void TerrainModifierSystem::BakeHeightPageOverlay(TiledTerrainData& tiled, float32 heightScale,
                                                  float32 terrainOriginY,
                                                  const std::vector<ResolvedModifier>& modifiers, bool fullBake,
                                                  const DirtyUnion& heightDirty, bool readBakedTiles)
{
    GE_CPU_PROFILE_SCOPE("Terrain.PageOverlay");
    tiled.HeightBakeReport.PageOverlayComposedSamples = 0;
    const std::optional<TileLattice> lattice = MakeTileLattice(tiled);
    if (!lattice || tiled.PageOverlayByteCap == 0)
    {
        // A terrain that keeps its height texture holds no overlay; it is built when it would page.
        ReleaseHeightPageOverlay(tiled);
        return;
    }
    std::vector<PageSampleRect> reach;
    for (const ResolvedModifier& modifier : modifiers)
    {
        if (modifier.IsSplatOnlyModifier())
            continue;
        if (const auto rect = LatticeRect(*lattice, modifier.BoundsMinX, modifier.BoundsMinZ, modifier.BoundsMaxX,
                                          modifier.BoundsMaxZ))
            reach.push_back(*rect);
    }
    PageStreaming::HeightPageLayer* previous = tiled.PageOverlay.get();
    if (reach.empty())
    {
        // No height modifier reaches the terrain: its pages are its base.
        if (previous)
            PublishPageOverlay(tiled, nullptr, ExtentOf(*previous));
        tiled.PageOverlayCurrent = true;
        tiled.PageOverlayRefusedBytes = 0;
        tiled.PageOverlayRefusedOffGrid = false;
        return;
    }
    if (tiled.PageOverlayOffGrid)
    {
        // Its pages are not on the lattice the overlay is baked on: the terrain keeps its height
        // texture while a modifier reaches it (the driver logs why).
        ReleaseHeightPageOverlay(tiled);
        tiled.PageOverlayRefusedOffGrid = true;
        return;
    }

    const std::vector<PageStreaming::PageStoreLevel> levels =
        PageStreaming::BuildPageStoreLevels(lattice->SamplesX, lattice->SamplesZ);
    reach = MergeTouching(std::move(reach));
    const uint64 bytes = PageStreaming::HeightPageLayerBytesOver(levels, reach);
    if (bytes > tiled.PageOverlayByteCap)
    {
        // Over the platform's cap: the terrain keeps its height texture (the driver logs why).
        ReleaseHeightPageOverlay(tiled);
        tiled.PageOverlayRefusedBytes = bytes;
        return;
    }
    tiled.PageOverlayRefusedBytes = 0;

    // An edit writes the dirty rect into the layer it holds: a sample outside the rect keeps its
    // delta, and a sample a modifier left or reached holds the new delta (zero where none reaches).
    std::optional<PageSampleRect> dirty;
    if (heightDirty.Any)
        dirty = LatticeRect(*lattice, heightDirty.MinX, heightDirty.MinZ, heightDirty.MaxX, heightDirty.MaxZ);
    const bool edit = !fullBake && previous && tiled.PageOverlayCurrent && previous->FitsLevels(levels);
    if (edit && !dirty)
        return; // nothing under the terrain's height changed
    const std::vector<PageSampleRect> writes = edit ? std::vector<PageSampleRect>{*dirty} : reach;

    std::vector<BakeBandRegion> regions;
    std::vector<std::vector<float32>> deltas(writes.size());
    for (std::size_t region = 0; region < writes.size(); ++region)
    {
        const PageSampleRect& write = writes[region];
        regions.push_back(BakeBandRegion{static_cast<int32>(write.MinX), static_cast<int32>(write.MinZ),
                                         static_cast<int32>(write.MaxX), static_cast<int32>(write.MaxZ)});
        deltas[region].assign(static_cast<std::size_t>(write.Width()) * write.Height(), 0.0f);
    }
    const LatticeBlockSource source = readBakedTiles ? LatticeBlockSource::BakedTiles : LatticeBlockSource::Compose;
    std::atomic<uint64> composedSamples{0};
    RunBakeRowBands(regions, [&composedSamples, &tiled, &modifiers, &writes, &deltas, heightScale, terrainOriginY,
                              source](std::size_t region, int32 x0, int32 x1, int32 z0, int32 z1) {
        composedSamples.fetch_add(ComposeLatticeDeltaBand(tiled, heightScale, terrainOriginY, modifiers,
                                                          writes[region], x0, x1, z0, z1, source, deltas[region]),
                                  std::memory_order_relaxed);
    });
    tiled.HeightBakeReport.PageOverlayComposedSamples = composedSamples.load(std::memory_order_relaxed);

    if (edit)
    {
        previous->WriteLevel0(0, *dirty, deltas.front());
        PublishPageOverlay(tiled, tiled.PageOverlay, *dirty);
        if (previous->Bytes() > tiled.PageOverlayByteCap)
        {
            const uint64 held = previous->Bytes();
            ReleaseHeightPageOverlay(tiled);
            tiled.PageOverlayRefusedBytes = held;
            return;
        }
    }
    else
    {
        // A rebuild changes what either layer holds.
        auto layer = std::make_shared<PageStreaming::HeightPageLayer>(1u, levels);
        for (std::size_t region = 0; region < writes.size(); ++region)
            layer->WriteLevel0(0, writes[region], deltas[region]);
        std::optional<PageSampleRect> changed = layer->Extent(0);
        for (const std::optional<PageSampleRect>& more : {previous ? previous->Extent(0) : std::nullopt, dirty})
            if (more)
                changed = changed ? Union(*changed, *more) : *more;
        PublishPageOverlay(tiled, std::move(layer), changed.value_or(PageSampleRect{}));
    }
    tiled.PageOverlayCurrent = true;
}

uint64 TerrainModifierSystem::ComposeLatticeDeltaBand(const TiledTerrainData& tiled, float32 heightScale,
                                                      float32 terrainOriginY,
                                                      const std::vector<ResolvedModifier>& modifiers,
                                                      const PageSampleRect& write, int32 x0, int32 x1, int32 z0,
                                                      int32 z1, LatticeBlockSource source, std::span<float32> deltas)
{
    static const std::vector<ResolvedModifier> kNoModifiers;
    const uint32 countX = static_cast<uint32>(x1 - x0 + 1);
    const uint32 countZ = static_cast<uint32>(z1 - z0 + 1);
    std::vector<float32> baked;
    std::vector<float32> base;
    uint64 bakedComposed = 0;
    uint64 baseComposed = 0;
    if (!BuildLatticeBlock(tiled, heightScale, terrainOriginY, modifiers, x0, z0, countX, countZ, source, baked,
                           bakedComposed) ||
        !BuildLatticeBlock(tiled, heightScale, terrainOriginY, kNoModifiers, x0, z0, countX, countZ,
                           LatticeBlockSource::Compose, base, baseComposed))
        return 0;
    for (uint32 z = 0; z < countZ; ++z)
    {
        float32* out = deltas.data() + static_cast<std::size_t>(z0 - static_cast<int32>(write.MinZ) + z) * write.Width() +
                       static_cast<std::size_t>(x0 - static_cast<int32>(write.MinX));
        for (uint32 x = 0; x < countX; ++x)
            out[x] = baked[static_cast<std::size_t>(z) * countX + x] - base[static_cast<std::size_t>(z) * countX + x];
    }
    return bakedComposed;
}

void TerrainModifierSystem::PublishSurfaceChanged(TiledTerrainData& tiled, bool fullBake,
                                                  const DirtyUnion& heightDirty, const DirtyUnion& splatDirty)
{
    const std::optional<TileLattice> lattice = MakeTileLattice(tiled);
    if (!lattice)
        return;
    std::optional<PageSampleRect> changed;
    if (fullBake)
        changed = PageSampleRect{0u, 0u, lattice->SamplesX - 1u, lattice->SamplesZ - 1u};
    for (const DirtyUnion* dirty : {&heightDirty, &splatDirty})
    {
        if (fullBake || !dirty->Any)
            continue;
        const std::optional<PageSampleRect> reach =
            LatticeRect(*lattice, dirty->MinX, dirty->MinZ, dirty->MaxX, dirty->MaxZ);
        if (reach)
            changed = changed ? PageStreaming::Union(*changed, *reach) : *reach;
    }
    if (!changed)
        return;
    tiled.SurfaceRefresh = SurfaceRefreshReason::Edit;
    AddSurfaceChange(tiled, *changed, kEverySurfacePlane);
}

void TerrainModifierSystem::PublishGrassSurfaceChanged(TiledTerrainData& tiled, bool entire, const DirtyUnion& dirty)
{
    const std::optional<TileLattice> lattice = MakeTileLattice(tiled);
    if (!lattice)
        return;
    std::optional<PageSampleRect> changed;
    if (entire)
        changed = PageSampleRect{0u, 0u, lattice->SamplesX - 1u, lattice->SamplesZ - 1u};
    else if (dirty.Any)
        changed = LatticeRect(*lattice, dirty.MinX, dirty.MinZ, dirty.MaxX, dirty.MaxZ);
    if (!changed)
        return;
    AddSurfaceChange(tiled, *changed, kGrassSurfacePlanes);
}

} // namespace GameEngine::TerrainECS
