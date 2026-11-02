#include "PageStreaming/PageCache.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace GameEngine::PageStreaming
{

std::size_t PageAddressHash::operator()(const PageAddress& address) const noexcept
{
    return static_cast<std::size_t>(PageMortonCode(address.X, address.Z) * 0x9E3779B97F4A7C15ull ^
                                    (static_cast<uint64>(address.Level) << 56u) ^
                                    (static_cast<uint64>(address.Face) << 61u));
}

void PageCache::Configure(uint32 slotCount, uint32 framesInFlight, float32 fadeSeconds)
{
    m_Pool.Initialize(slotCount, framesInFlight);
    m_Slots.assign(slotCount, SlotPage{});
    m_Resident.clear();
    m_Resident.reserve(slotCount);
    m_LastWanted.clear();
    m_Order.clear();
    m_Uploads.clear();
    m_Transitions.clear();
    // Sized for every slot once, so a frame that changes each slot once allocates nothing.
    m_Uploads.reserve(slotCount);
    m_Transitions.reserve(slotCount);
    m_Rewrites.clear();
    m_FadeChanges.clear();
    m_FadeSeconds = fadeSeconds;
    m_LastFrame = 0;
    m_UnwantedResident = 0;
    m_FadesPending = false;
    m_Visits = 0;
    m_Deferred = 0;
}

float32 PageCache::FadeOf(const CachedPage& page) const
{
    const uint32 slot = m_Pool.SlotOf(page);
    return slot == kNoResidentSlot ? 1.0f : m_Slots[slot].Fade;
}

void PageCache::Release(const CachedPage& page)
{
    const uint32 parent = m_Pool.SlotOf(page.Parent());
    if (parent != kNoResidentSlot)
        --m_Slots[parent].ResidentChildren;
    if (!m_Slots[m_Pool.SlotOf(page)].Wanted)
        --m_UnwantedResident;
    m_Pool.Release(page, m_Pool.Find(page).Generation);
    ++m_Releases;
}

void PageCache::ReleaseStream(uint32 stream)
{
    // Finest first, so a page's children go before it; compacted in place, keeping the order.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < m_Resident.size(); ++i)
    {
        const CachedPage page = m_Resident[i];
        if (page.Stream == stream)
        {
            Release(page);
            continue;
        }
        m_Resident[kept++] = page;
    }
    m_Resident.resize(kept);
}

void PageCache::BeginFrame(uint64 frameIndex)
{
    m_Uploads.clear();
    m_Transitions.clear();
    m_Rewrites.clear();
    m_Releases = 0;
    m_Visits = 0;
    m_Pool.BeginFrame(frameIndex);
}

void PageCache::MarkWanted(uint64 frameIndex, std::span<const PageWant> wanted)
{
    // A page wanted through the last frame and not now was last wanted then.
    for (const CachedPage& page : m_Resident)
    {
        SlotPage& state = m_Slots[m_Pool.SlotOf(page)];
        if (state.Wanted)
            state.LastWantedFrame = m_LastFrame;
        state.Wanted = false;
    }
    m_UnwantedResident = static_cast<uint32>(m_Resident.size());
    for (const PageWant& want : wanted)
    {
        const uint32 slot = m_Pool.SlotOf(want.Page());
        if (slot == kNoResidentSlot || m_Slots[slot].Wanted)
            continue;
        m_Slots[slot].Wanted = true;
        m_Slots[slot].LastWantedFrame = frameIndex;
        m_Slots[slot].Distance = want.Distance;
        --m_UnwantedResident;
    }
    m_Visits += static_cast<uint32>(m_Resident.size() + wanted.size());
}

void PageCache::ReleaseStale(uint64 frameIndex)
{
    if (m_UnwantedResident == 0)
        return;
    // Finest first, so a page's children are released before it is considered; compacted in place,
    // keeping the order.
    m_Visits += static_cast<uint32>(m_Resident.size());
    std::size_t kept = 0;
    for (std::size_t i = 0; i < m_Resident.size(); ++i)
    {
        const CachedPage page = m_Resident[i];
        const SlotPage& state = m_Slots[m_Pool.SlotOf(page)];
        if (!state.Wanted && frameIndex - state.LastWantedFrame > kPageReleaseHysteresisFrames &&
            state.ResidentChildren == 0)
        {
            Release(page);
            m_Transitions.push_back(page);
            continue;
        }
        m_Resident[kept++] = page;
    }
    m_Resident.resize(kept);
}

void PageCache::TickFades(float32 deltaSeconds)
{
    m_ActiveFades = 0;
    m_FadeChanges.clear();
    if (m_FadeSeconds <= 0.0f || !m_FadesPending)
        return;
    m_Visits += static_cast<uint32>(m_Resident.size());
    for (const CachedPage& page : m_Resident)
    {
        float32& fade = m_Slots[m_Pool.SlotOf(page)].Fade;
        if (fade >= 1.0f)
            continue;
        fade = std::min(1.0f, fade + deltaSeconds / m_FadeSeconds);
        m_FadeChanges.push_back(page);
        if (fade < 1.0f)
            ++m_ActiveFades;
    }
    m_FadesPending = m_ActiveFades != 0;
}

void PageCache::SortCandidates(std::span<const PageWant> wanted)
{
    // A parked camera wants the same set every frame: the order is kept, not sorted again.
    if (std::equal(wanted.begin(), wanted.end(), m_LastWanted.begin(), m_LastWanted.end()))
        return;
    m_LastWanted.assign(wanted.begin(), wanted.end());
    m_Order.resize(wanted.size());
    for (uint32 i = 0; i < m_Order.size(); ++i)
        m_Order[i] = i;
    std::sort(m_Order.begin(), m_Order.end(), [&](uint32 a, uint32 b) {
        const PageWant& wa = wanted[a];
        const PageWant& wb = wanted[b];
        return wa.Address.Level != wb.Address.Level ? wa.Address.Level > wb.Address.Level : wa.Distance < wb.Distance;
    });
}

bool PageCache::EvictFor(const PageWant& want)
{
    // The farthest settled page of the wanted page's level or finer with no resident child, farther
    // than the wanted page (a page no longer wanted counts as infinitely far).
    uint32 victim = kNoResidentSlot;
    float32 victimDistance = want.Distance;
    for (const CachedPage& page : m_Resident)
    {
        if (page.Address.Level > want.Address.Level)
            break; // finest first: everything after is coarser
        const uint32 slot = m_Pool.SlotOf(page);
        const SlotPage& state = m_Slots[slot];
        if (state.Fade < 1.0f || state.ResidentChildren != 0)
            continue;
        const float32 distance = state.Wanted ? state.Distance : std::numeric_limits<float32>::infinity();
        if (distance > victimDistance)
        {
            victim = slot;
            victimDistance = distance;
        }
    }
    if (victim == kNoResidentSlot)
        return false;
    const CachedPage page = m_Pool.KeyOf(victim);
    Release(page);
    m_Transitions.push_back(page);
    m_Resident.erase(std::find(m_Resident.begin(), m_Resident.end(), page));
    return true;
}

void PageCache::Assign(const PageWant& want, uint32 slot, uint64 frameIndex)
{
    SlotPage& state = m_Slots[slot];
    state = SlotPage{frameIndex, want.Distance, 1.0f, 0, true};
    if (m_FadeSeconds > 0.0f && !want.Top)
    {
        state.Fade = 0.0f;
        m_FadesPending = true;
    }
    const CachedPage page = want.Page();
    const uint32 parent = m_Pool.SlotOf(page.Parent());
    if (parent != kNoResidentSlot && !want.Top)
        ++m_Slots[parent].ResidentChildren;
    const auto at = std::upper_bound(m_Resident.begin(), m_Resident.end(), page, [](const CachedPage& a, const CachedPage& b) {
        return a.Address.Level < b.Address.Level;
    });
    m_Resident.insert(at, page);
    m_Uploads.push_back(PageUploadRequest{page, slot});
    m_Transitions.push_back(page);
}

void PageCache::Update(uint64 frameIndex, float32 deltaSeconds, std::span<const PageWant> wanted, uint32 maxAssigns)
{
    BeginFrame(frameIndex);
    m_Deferred = 0;
    MarkWanted(frameIndex, wanted);
    ReleaseStale(frameIndex);
    TickFades(deltaSeconds);

    SortCandidates(wanted);
    uint32 evictions = 0;
    m_Visits += static_cast<uint32>(m_Order.size());
    for (const uint32 index : m_Order)
    {
        const PageWant& want = wanted[index];
        if (m_Pool.SlotOf(want.Page()) != kNoResidentSlot)
            continue;
        const bool parentResident = want.Top || m_Pool.SlotOf(want.Page().Parent()) != kNoResidentSlot;
        if (!parentResident || m_Uploads.size() + evictions >= maxAssigns)
        {
            ++m_Deferred;
            continue;
        }
        if (m_Pool.FreeCount() == 0)
        {
            // The freed slot is quarantined for the frames in flight; the page takes it then.
            evictions += EvictFor(want) ? 1u : 0u;
            ++m_Deferred;
            continue;
        }
        const ResidentSlotRef slot = m_Pool.Acquire(want.Page(), want.Distance * want.Distance);
        Assign(want, slot.Slot, frameIndex);
    }
    m_LastFrame = frameIndex;
}

void PageCache::Tick(uint64 frameIndex, float32 deltaSeconds)
{
    assert(m_Deferred == 0 && "Tick after an Update that left wanted pages without a slot");
    BeginFrame(frameIndex);
    ReleaseStale(frameIndex);
    TickFades(deltaSeconds);
    m_LastFrame = frameIndex;
}

bool PageCache::Rewrite(const CachedPage& page, const PageSampleRect& level0Rect)
{
    const uint32 slot = m_Pool.SlotOf(page);
    if (slot == kNoResidentSlot)
        return false;
    m_Uploads.push_back(PageUploadRequest{page, slot});
    m_Rewrites.push_back(PageRewrite{page, level0Rect});
    return true;
}

} // namespace GameEngine::PageStreaming
