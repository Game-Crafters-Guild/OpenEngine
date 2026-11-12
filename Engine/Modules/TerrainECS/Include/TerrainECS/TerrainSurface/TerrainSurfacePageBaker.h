#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "Types/Types.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::PageStreaming
{
class PageStoreAccess;
}

namespace GameEngine::TerrainECS
{

struct SurfaceHeightWindow;
struct TerrainSurfaceBakeInputs;

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
    /// Bakes `plane` (Splat, Normal, GrassControl or Color) of the terrain of `inputs`
    /// (TerrainModifierSystem::SnapshotSurfaceBake) through `store`, the plane's store access on the
    /// terrain's lattice. `pool` runs the bake; null runs it on this thread.
    TerrainSurfacePageBaker(std::shared_ptr<const TerrainSurfaceBakeInputs> inputs, PageStreaming::PageFieldKind plane,
                            std::shared_ptr<PageStreaming::PageStoreAccess> store, SurfaceHeightPageFetch fetchHeights,
                            JobSystem::WorkStealingThreadPool* pool);
    /// Publishes the store it holds open (Finish).
    ~TerrainSurfacePageBaker();
    TerrainSurfacePageBaker(const TerrainSurfacePageBaker&) = delete;
    TerrainSurfacePageBaker& operator=(const TerrainSurfacePageBaker&) = delete;

    /// Bakes from now on from `inputs`, a snapshot of the same lattice, and `fetchHeights`. `key` is
    /// the plane's store key of `inputs` (TerrainModifierSystem::SurfaceStoreKey). Not while another
    /// call of this baker runs.
    void SetInputs(std::shared_ptr<const TerrainSurfaceBakeInputs> inputs, uint64 key,
                   SurfaceHeightPageFetch fetchHeights);

    /// Makes the store at `file` under `key` with every page of the levels from `firstCoarseLevel` up (at least 1),
    /// coarsest first, each from the height page of the same address, and no level-0 page.
    /// Returns an empty string, else the reason (a height page is not on hand, an I/O failure).
    std::string CreateStore(const std::filesystem::path& file, uint64 key, uint32 firstCoarseLevel);

    /// Bakes `pages` of any level, and the ancestors the store lacks, into the store at `file`, each at its own
    /// spacing. A coarse texel is then the filter of its finer level where every finer page it reads is present.
    /// The store stays open until Finish; its access reads each page on return. Returns "", else the reason.
    std::string BakePages(const std::filesystem::path& file, std::span<const PageStreaming::PageAddress> pages);

    /// Publishes the store that BakePages holds open at its file. Returns an empty string (also
    /// when no store is open), else the reason.
    std::string Finish();

    /// One rect a level: the pages (columns MinX to MaxX, rows MinZ to MaxZ) whose bake reads a level-0
    /// sample of `changed` (TiledTerrainData::SurfaceChanged), with their apron and the height ring past it.
    /// Bake the present ones again to refresh the store.
    std::vector<PageStreaming::PageSampleRect> PagesReading(const PageStreaming::PageSampleRect& changed) const;

    /// The height pages that CreateStore from `firstCoarseLevel` reads, and that BakePages of `pages` can
    /// read (their ancestors included). Empty for a plane that reads no heights.
    std::vector<PageStreaming::PageAddress> HeightPagesReadByCreate(uint32 firstCoarseLevel) const;
    std::vector<PageStreaming::PageAddress>
    HeightPagesReadByBake(std::span<const PageStreaming::PageAddress> pages) const;
    /// Pages written with new bytes, over every call.
    uint64 PagesWritten() const { return m_PagesWritten; }

private:
    struct BakedPage;
    struct OpenStore;

    /// Opens the store at `file` to patch, unless it is open. Returns an empty string, else the reason.
    std::string OpenForBake(const std::filesystem::path& file);

    /// Bakes `pages` of `level`. A page that cannot be baked has no texels.
    std::vector<BakedPage> BakeLevelPages(uint32 level, std::span<const PageStreaming::PageAddress> pages);
    /// Bakes the texels of the page at `address` of level `shape` over the heights of `window`.
    bool BakePageTexels(const PageStreaming::PageAddress& address, const PageStreaming::PageStoreLevel& shape,
                        const SurfaceHeightWindow& window, std::vector<uint8>& outTexels) const;

    // The first coarse level CreateStore bakes from `firstCoarseLevel`.
    uint32 FirstCoarseLevel(uint32 firstCoarseLevel) const;

    std::shared_ptr<const TerrainSurfaceBakeInputs> m_Inputs;
    uint64 m_Key = 0; // the store key of m_Inputs; 0 matches no store
    PageStreaming::PageFieldKind m_Plane;
    std::shared_ptr<PageStreaming::PageStoreAccess> m_Store;
    SurfaceHeightPageFetch m_FetchHeights;
    std::vector<PageStreaming::PageStoreLevel> m_Levels; // the store's, from the terrain's lattice
    JobSystem::WorkStealingThreadPool* m_Pool = nullptr;
    uint64 m_PagesWritten = 0;
    std::unique_ptr<OpenStore> m_Open; // the store BakePages patches, until Finish
};

} // namespace GameEngine::TerrainECS
