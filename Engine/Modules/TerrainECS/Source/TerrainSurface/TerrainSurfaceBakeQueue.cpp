#include "TerrainSurface/TerrainSurfaceBakeQueue.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <tuple>

namespace GameEngine::TerrainECS
{

namespace
{

// The bake order's key of a page: the coarser level first, then the nearer page, then the least Morton code.
std::tuple<int32, float32, uint64> BakeOrderKey(const PageStreaming::PageAddress& page, float32 distance)
{
    return {-static_cast<int32>(page.Level), distance, PageStreaming::PageMortonCode(page.X, page.Z)};
}

} // namespace

void TerrainSurfaceBakeQueue::Reset(std::span<const PageStreaming::PageStoreLevel> levels, uint32 pinnedLevel)
{
    m_Levels.assign(levels.begin(), levels.end());
    m_PinnedLevel = pinnedLevel;
    m_Planes.clear();
    m_Baked.Clear();
    m_LastWanted.clear();
    Clear();
}

void TerrainSurfaceBakeQueue::SetPresent(SurfacePlaneLayout layout)
{
    const uint8 plane = layout.Plane;
    RemovePlane(plane);
    m_Planes.push_back(std::move(layout));
    QueueWanted();
}

void TerrainSurfaceBakeQueue::RemovePlane(uint8 plane)
{
    std::erase_if(m_Planes, [plane](const SurfacePlaneLayout& entry) { return entry.Plane == plane; });
    for (auto& [address, planes] : m_Baked)
        planes &= static_cast<uint8>(~plane);
    std::vector<PageStreaming::PageAddress> emptied;
    for (auto& [address, planes] : m_Edits)
        if ((planes &= static_cast<uint8>(~plane)) == 0u)
            emptied.push_back(address);
    for (const PageStreaming::PageAddress& address : emptied)
        m_Edits.Erase(address);
    QueueWanted();
}

void TerrainSurfaceBakeQueue::Want(std::span<const PageStreaming::PageWant> wanted)
{
    if (std::ranges::equal(wanted, m_LastWanted))
        return;
    const bool samePages =std::ranges::equal(wanted, m_LastWanted, {}, &PageStreaming::PageWant::Address,
                                              &PageStreaming::PageWant::Address);
    m_LastWanted.assign(wanted.begin(), wanted.end());
    // The same pages at new distances keep the queued pages: only the order changes, and Take sorts.
    m_WantedSorted = false;
    if (!samePages)
        QueueWanted();
}

void TerrainSurfaceBakeQueue::QueueNext(std::span<const SurfaceBakePage> pages)
{
    for (const SurfaceBakePage& page : pages)
    {
        assert(page.Address.Level < m_Levels.size());
        m_Edits.GetOrInsert(page.Address) |= page.Planes;
        m_EditOrder.push_back(page.Address);
    }
    // An edit page bakes once: as an edit, not also as a wanted page.
    std::erase_if(m_Wanted, [this](const WantedPage& page) { return m_Edits.Contains(page.Page.Address); });
}

void TerrainSurfaceBakeQueue::AppendPresent(uint8 plane, uint32 level, const PageStreaming::PageSampleRect& pages,
                                            std::vector<PageStreaming::PageAddress>& out) const
{
    for (uint32 z = pages.MinZ; z <= pages.MaxZ; ++z)
        for (uint32 x = pages.MinX; x <= pages.MaxX; ++x)
        {
            const PageStreaming::PageAddress address{0, static_cast<uint8>(level), x, z};
            if ((PresentPlanes(address) & plane) != 0u)
                out.push_back(address);
        }
}

void TerrainSurfaceBakeQueue::MarkBaked(std::span<const SurfaceBakePage> pages)
{
    for (const SurfaceBakePage& page : pages)
        // BakePages bakes the absent ancestors of a page, up to the pinned levels that the open made.
        for (PageStreaming::PageAddress address = page.Address; address.Level < m_PinnedLevel;
             address = address.Parent())
            m_Baked.GetOrInsert(address) |= page.Planes;
    QueueWanted();
}

void TerrainSurfaceBakeQueue::Take(std::size_t count, std::vector<SurfaceBakePage>& out)
{
    assert(std::has_single_bit(count) && std::countr_zero(count) % 2 == 0);
    out.clear();
    while (out.size() < count && !m_EditOrder.empty())
    {
        const PageStreaming::PageAddress address = m_EditOrder.back();
        m_EditOrder.pop_back();
        if (const uint8* planes = m_Edits.Find(address))
        {
            out.push_back(SurfaceBakePage{address, *planes});
            m_Edits.Erase(address);
        }
    }
    if (m_Edits.Empty())
        m_EditOrder.clear(); // only stale pages are left
    if (!out.empty() || m_Wanted.empty())
        return;
    SortWanted();
    // The next page, then the wanted pages of its square of `side` pages a side in its level: they read
    // the same heights.
    const PageStreaming::PageAddress first = m_Wanted.back().Page.Address;
    const uint32 side = 1u << (std::countr_zero(count) / 2);
    for (auto page = m_Wanted.end(); page != m_Wanted.begin() && out.size() < count;)
    {
        const PageStreaming::PageAddress& address = (--page)->Page.Address;
        if (address.Level == first.Level && address.X / side == first.X / side && address.Z / side == first.Z / side)
        {
            out.push_back(page->Page);
            page = m_Wanted.erase(page);
        }
    }
    while (out.size() < count && !m_Wanted.empty())
    {
        out.push_back(m_Wanted.back().Page);
        m_Wanted.pop_back();
    }
}

void TerrainSurfaceBakeQueue::Clear()
{
    m_Edits.Clear();
    m_EditOrder.clear();
    m_Wanted.clear();
}

uint8 TerrainSurfaceBakeQueue::PresentPlanes(const PageStreaming::PageAddress& page) const
{
    uint8 planes = 0;
    for (const SurfacePlaneLayout& plane : m_Planes)
        if (plane.Layout.FindPresent(page))
            planes |= plane.Plane;
    if (const uint8* baked = m_Baked.Find(page))
        planes |= *baked;
    return planes;
}

void TerrainSurfaceBakeQueue::QueueWanted()
{
    uint8 every = 0;
    for (const SurfacePlaneLayout& plane : m_Planes)
        every |= plane.Plane;
    m_Wanted.clear();
    for (const PageStreaming::PageWant& want : m_LastWanted)
    {
        const PageStreaming::PageAddress& address = want.Address;
        if (address.Level >= m_PinnedLevel || address.Level >= m_Levels.size())
            continue; // a pinned level is whole from the open
        if (address.X >= m_Levels[address.Level].PagesX || address.Z >= m_Levels[address.Level].PagesZ)
            continue; // a wanted set of another lattice, before the stream asks again
        if (m_Edits.Contains(address))
            continue;
        if (const uint8 lacking = static_cast<uint8>(every & ~PresentPlanes(address)); lacking != 0u)
            m_Wanted.push_back(WantedPage{SurfaceBakePage{address, lacking},
                                          static_cast<uint32>(&want - m_LastWanted.data())});
    }
    m_WantedSorted = false;
}

void TerrainSurfaceBakeQueue::SortWanted()
{
    if (m_WantedSorted)
        return;
    // The next page at the back.
    std::sort(m_Wanted.begin(), m_Wanted.end(), [this](const WantedPage& a, const WantedPage& b) {
        return BakeOrderKey(a.Page.Address, m_LastWanted[a.Want].Distance) >
               BakeOrderKey(b.Page.Address, m_LastWanted[b.Want].Distance);
    });
    m_WantedSorted = true;
}

} // namespace GameEngine::TerrainECS
