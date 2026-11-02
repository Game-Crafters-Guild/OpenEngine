#pragma once

// The definitions of ResidentSlotPool (ResidentSlotPool.h).

namespace GameEngine::PageStreaming
{

template <typename Key>
void ResidentSlotPool<Key>::Initialize(uint32 slotCount, uint32 framesInFlight)
{
    m_Slots.assign(slotCount, Slot{});
    m_FreeList.clear();
    m_FreeList.reserve(slotCount);
    // Hand out low indices first (deterministic packing for the seam oracles).
    for (uint32 i = slotCount; i > 0; --i)
        m_FreeList.push_back(i - 1);
    m_KeyToSlot.Clear();
    m_KeyToSlot.Reserve(slotCount);
    m_FramesInFlight = framesInFlight;
    m_CurrentFrame = 0;
    m_ResidentCount = 0;
    m_QuarantinedCount = 0;
    m_AssignsThisFrame = 0;
    m_EvictionsThisFrame = 0;
}

template <typename Key>
void ResidentSlotPool<Key>::Reset()
{
    m_FreeList.clear();
    m_FreeList.reserve(m_Slots.size());
    for (uint32 i = static_cast<uint32>(m_Slots.size()); i > 0; --i)
    {
        Slot& s = m_Slots[i - 1];
        if (s.State != SlotState::Free)
            ++s.Generation; // stale any handle held across the sweep
        s.State = SlotState::Free;
        s.Owner = {};
        s.LastTouchFrame = 0;
        s.QuarantineUntilFrame = 0;
        m_FreeList.push_back(i - 1);
    }
    m_KeyToSlot.Clear();
    m_ResidentCount = 0;
    m_QuarantinedCount = 0;
    m_AssignsThisFrame = 0;
    m_EvictionsThisFrame = 0;
}

template <typename Key>
void ResidentSlotPool<Key>::BeginFrame(uint64 frameIndex)
{
    m_CurrentFrame = frameIndex;
    m_AssignsThisFrame = 0;
    m_EvictionsThisFrame = 0;

    for (uint32 i = 0; m_QuarantinedCount != 0 && i < m_Slots.size(); ++i)
    {
        Slot& s = m_Slots[i];
        if (s.State == SlotState::Quarantined && frameIndex >= s.QuarantineUntilFrame)
        {
            s.State = SlotState::Free;
            m_FreeList.push_back(i);
            --m_QuarantinedCount;
        }
    }
}

template <typename Key>
ResidentSlotRef ResidentSlotPool<Key>::Find(const Key& key) const
{
    const uint32* slot = m_KeyToSlot.Find(key);
    if (!slot)
        return {};
    return ResidentSlotRef{*slot, m_Slots[*slot].Generation};
}

template <typename Key>
uint32 ResidentSlotPool<Key>::SlotOf(const Key& key) const
{
    const uint32* slot = m_KeyToSlot.Find(key);
    return slot ? *slot : kNoResidentSlot;
}

template <typename Key>
void ResidentSlotPool<Key>::QuarantineSlot(uint32 slotIndex)
{
    Slot& s = m_Slots[slotIndex];
    if (s.State != SlotState::Live)
        return;
    m_KeyToSlot.Erase(s.Owner);
    ++s.Generation;
    --m_ResidentCount;
    if (m_FramesInFlight == 0)
    {
        // Quarantine disabled (discriminator): the slot is immediately reusable,
        // which is exactly the wrong-terrain hazard the eviction-churn oracle catches.
        s.State = SlotState::Free;
        m_FreeList.push_back(slotIndex);
    }
    else
    {
        s.State = SlotState::Quarantined;
        s.QuarantineUntilFrame = m_CurrentFrame + m_FramesInFlight;
        ++m_QuarantinedCount;
    }
}

template <typename Key>
void ResidentSlotPool<Key>::Touch(const Key& key, float32 priorityDistanceSq)
{
    const uint32* slot = m_KeyToSlot.Find(key);
    if (!slot)
        return;
    Slot& s = m_Slots[*slot];
    s.LastTouchFrame = m_CurrentFrame;
    s.LastPriorityDistanceSq = priorityDistanceSq;
}

template <typename Key>
ResidentSlotRef ResidentSlotPool<Key>::Acquire(const Key& key, float32 priorityDistanceSq)
{
    if (const uint32* slot = m_KeyToSlot.Find(key))
    {
        Slot& s = m_Slots[*slot];
        s.LastTouchFrame = m_CurrentFrame;
        s.LastPriorityDistanceSq = priorityDistanceSq;
        return ResidentSlotRef{*slot, s.Generation};
    }

    // Assign a free slot if one is available.
    if (!m_FreeList.empty())
    {
        const uint32 slotIndex = m_FreeList.back();
        m_FreeList.pop_back();
        Slot& s = m_Slots[slotIndex];
        s.Owner = key;
        s.State = SlotState::Live;
        s.LastTouchFrame = m_CurrentFrame;
        s.LastPriorityDistanceSq = priorityDistanceSq;
        m_KeyToSlot.InsertOrAssign(key, slotIndex);
        ++m_ResidentCount;
        ++m_AssignsThisFrame;
        return ResidentSlotRef{slotIndex, s.Generation};
    }

    // Pool full: evict the farthest live slot that is untouched this frame and
    // strictly farther than the requester (a nearer key reclaims a farther
    // one). The evicted slot is quarantined, so the requester gets no slot THIS
    // frame — it renders coarse until the slot clears quarantine and a later
    // Acquire places it. Refusing same-frame reuse is the Risk 2 guarantee.
    uint32 victim = kNoResidentSlot;
    float32 victimDistSq = priorityDistanceSq;
    for (uint32 i = 0; i < m_Slots.size(); ++i)
    {
        const Slot& s = m_Slots[i];
        if (s.State != SlotState::Live || s.LastTouchFrame == m_CurrentFrame)
            continue;
        if (s.LastPriorityDistanceSq > victimDistSq)
        {
            victim = i;
            victimDistSq = s.LastPriorityDistanceSq;
        }
    }
    if (victim != kNoResidentSlot)
    {
        QuarantineSlot(victim);
        ++m_EvictionsThisFrame;
    }
    return {}; // not resident this frame
}

template <typename Key>
bool ResidentSlotPool<Key>::Release(const Key& key, uint32 generation)
{
    const uint32* slot = m_KeyToSlot.Find(key);
    if (!slot)
        return false;
    const uint32 slotIndex = *slot;
    if (m_Slots[slotIndex].Generation != generation)
        return false; // stale handle — refuse (the current owner keeps its slot)
    QuarantineSlot(slotIndex);
    return true;
}

} // namespace GameEngine::PageStreaming
