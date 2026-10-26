#pragma once

#include "TerrainECS/TerrainColorMap.h"

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStoreFormat.h"
#include "Types/Types.h"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::TerrainECS
{

class TerrainModifierSystem;
struct SurfaceHeightWindow;
struct TiledTerrainData;

/// Fills `outSamples` with height page `address`: kPageSampleCount heights, normalized as the tiles
/// hold them (the base page plus the modifier overlay). False when the page is not on hand now.
/// Called on worker threads at once.
using SurfaceHeightPageFetch = std::function<bool(const PageStreaming::PageAddress&, std::span<float32>)>;

/// Writes one surface store (Splat, Normal, GrassControl or Color) of one tiled terrain (design
/// page-streaming 3.8). A page reads heights one sample past its stride, from the neighbor height
/// pages. A Normal store also writes its samples cache (SurfaceSamplesCachePath).
class TerrainSurfacePageBaker
{
public:
    /// Bakes `plane` (Splat, Normal, GrassControl or Color) with `system`'s applied modifiers over
    /// `tiled`. `pool` runs the bake; null runs it on this thread.
    TerrainSurfacePageBaker(TerrainModifierSystem& system, const TiledTerrainData& tiled, PageStreaming::PageFieldKind plane,
                     SurfaceHeightPageFetch fetchHeights, JobSystem::WorkStealingThreadPool* pool);

    /// Makes the store at `file` under `key` with every page of the levels from `firstCoarseLevel` up (at least 1),
    /// coarsest first, each from the height page of the same address, and no level-0 page.
    /// Returns an empty string, else the reason (a height page is not on hand, an I/O failure).
    std::string CreateStore(const std::filesystem::path& file, uint64 key, uint32 firstCoarseLevel);

    /// Bakes the level-0 pages `pages` into the store at `file` and refilters their ancestors. An
    /// ancestor the store does not hold yet is baked at its own spacing first. A page whose neighbor
    /// height page is not on hand takes its apron from the edge of its own height page. A page whose
    /// own height page is not on hand is not baked. Returns an empty string, else the reason.
    std::string BakeLevel0Pages(const std::filesystem::path& file, std::span<const PageStreaming::PageAddress> pages);

    /// The level-0 pages whose bake reads a sample of `changed` (TiledTerrainData::SurfaceChanged),
    /// with their apron and the height ring past it. Bake them again to refresh the store.
    std::vector<PageStreaming::PageAddress> Level0PagesReading(const PageStreaming::PageSampleRect& changed) const;

    /// The ground write of the terrain's grass (TerrainColorMapBaker::ResolveGroundWrite) that the
    /// Color plane composes. Off until set.
    void SetGroundWrite(const TerrainColorGroundWrite& write) { m_GroundWrite = write; }
    /// Pages written with new bytes, over every call.
    uint64 PagesWritten() const { return m_PagesWritten; }

private:
    struct BakedPage;

    /// Bakes `pages` of `level`. A page that cannot be baked has no texels.
    std::vector<BakedPage> BakePages(uint32 level, std::span<const PageStreaming::PageAddress> pages);
    /// Bakes the texels of the page at `address` of level `shape` over the heights of `window`.
    bool BakePageTexels(const PageStreaming::PageAddress& address, const PageStreaming::PageStoreLevel& shape,
                        const SurfaceHeightWindow& window, std::vector<uint8>& outTexels) const;

    TerrainModifierSystem& m_System;
    const TiledTerrainData& m_Tiled;
    PageStreaming::PageFieldKind m_Plane;
    SurfaceHeightPageFetch m_FetchHeights;
    std::vector<PageStreaming::PageStoreLevel> m_Levels; // the store's, from the terrain's lattice
    JobSystem::WorkStealingThreadPool* m_Pool = nullptr;
    uint64 m_PagesWritten = 0;
    TerrainColorGroundWrite m_GroundWrite;
};

} // namespace GameEngine::TerrainECS
