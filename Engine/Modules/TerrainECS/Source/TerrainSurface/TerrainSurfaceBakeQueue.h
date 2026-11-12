#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <cstddef>
#include <span>
#include <vector>

namespace GameEngine::TerrainECS
{

/// A surface page of any level to bake, and the mask of the planes that bake it (bit PageFieldKind).
struct SurfaceBakePage
{
    PageStreaming::PageAddress Address;
    uint8 Planes = 0;
};

/// The pages one plane's store held when the open job read it.
struct SurfacePlaneLayout
{
    uint8 Plane = 0; ///< SurfacePlaneBit of the plane
    PageStreaming::PageStoreLayout Layout;
};

/// The surface pages that the stores of one terrain still bake: the pages that an edit reaches, then the
/// wanted pages that a plane lacks, the coarsest level first and the nearest first in a level. A page is
/// present for a plane when the plane's store held it at the open, or a job baked it since. No dense array.
class TerrainSurfaceBakeQueue
{
public:
    /// Empties the queue and forgets every plane. The levels below `pinnedLevel` of `levels` bake on demand.
    void Reset(std::span<const PageStreaming::PageStoreLevel> levels, uint32 pinnedLevel);
    /// Sets the pages that plane `layout.Plane` holds to the present pages of `layout`, and forgets the
    /// pages that the plane baked or that an edit queued for it.
    void SetPresent(SurfacePlaneLayout layout);
    /// Forgets plane `plane` (a SurfacePlaneBit): no page of it is wanted or queued.
    void RemovePlane(uint8 plane);
    /// Queues the pages of `wanted` below the pinned level that a plane lacks, in place of the wanted
    /// pages queued before.
    void Want(std::span<const PageStreaming::PageWant> wanted);
    /// Queues `pages` ahead of the wanted pages, the last first. A queued page adds its planes.
    void QueueNext(std::span<const SurfaceBakePage> pages);
    /// Appends to `out` the pages of `level` in `pages` (columns MinX to MaxX, rows MinZ to MaxZ) that
    /// plane `plane` holds.
    void AppendPresent(uint8 plane, uint32 level, const PageStreaming::PageSampleRect& pages,
                       std::vector<PageStreaming::PageAddress>& out) const;
    /// Records that a job baked `pages` and the absent ancestors that BakePages bakes with them.
    void MarkBaked(std::span<const SurfaceBakePage> pages);
    /// Clears `out` and moves at most `count` queued pages (a power of 4) into it: the edit pages, else the
    /// next wanted page and the wanted pages of its block of `count` pages in its level, then the next ones.
    void Take(std::size_t count, std::vector<SurfaceBakePage>& out);
    /// Empties the queue and keeps the planes and the wanted set.
    void Clear();

    /// The count of queued pages.
    std::size_t Size() const { return m_Edits.Size() + m_Wanted.size(); }
    bool Empty() const { return Size() == 0u; }

private:
    struct WantedPage
    {
        SurfaceBakePage Page;
        uint32 Want = 0; // the index of the page in m_LastWanted, which holds its distance
    };

    // The planes that hold `page`, as plane bits.
    uint8 PresentPlanes(const PageStreaming::PageAddress& page) const;
    // Queues the pages of the last wanted set that a plane lacks, in no order.
    void QueueWanted();
    // Puts the queued wanted pages in the bake order at their last distances, the next at the back.
    void SortWanted();

    std::vector<PageStreaming::PageStoreLevel> m_Levels;
    uint32 m_PinnedLevel = 1;
    std::vector<SurfacePlaneLayout> m_Planes;
    FlatMap<PageStreaming::PageAddress, uint8> m_Baked;  // the planes a job baked a page of, since its open
    FlatMap<PageStreaming::PageAddress, uint8> m_Edits;  // the queued edit pages and their planes
    std::vector<PageStreaming::PageAddress> m_EditOrder; // the next at the back; a page not in m_Edits is stale
    std::vector<PageStreaming::PageWant> m_LastWanted;
    std::vector<WantedPage> m_Wanted; // in the bake order, the next at the back, when m_WantedSorted
    bool m_WantedSorted = true;
};

} // namespace GameEngine::TerrainECS
