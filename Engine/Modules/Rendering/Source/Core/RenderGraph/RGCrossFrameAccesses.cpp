#include "Rendering/Core/RenderGraph/RGCrossFrameAccesses.h"

#include <algorithm>

namespace GameEngine::Rendering::RenderGraph
{

void RGCrossFrameAccesses::CollectWaits(const Key& key, uint8_t physicalQueue, bool writes,
                                        uint64_t (&outValues)[kQueueCount]) const
{
    for (uint32_t q = 0; q < kQueueCount; ++q)
        outValues[q] = 0;
    const Entry* entry = m_Entries.Find(key);
    if (entry == nullptr)
        return;
    for (uint32_t q = 0; q < kQueueCount; ++q)
    {
        if (q == physicalQueue)
            continue;
        const uint64_t value = writes ? entry->LastAccess[q] : entry->LastWrite[q];
        outValues[q] = value > m_Completed[q] ? value : 0;
    }
}

void RGCrossFrameAccesses::SetCompleted(const uint64_t (&completedValues)[kQueueCount])
{
    for (uint32_t q = 0; q < kQueueCount; ++q)
        m_Completed[q] = completedValues[q];
}

void RGCrossFrameAccesses::Record(const Key& key, uint8_t physicalQueue, bool writes, uint64_t value)
{
    Entry& entry = m_Entries.GetOrInsert(key);
    entry.LastAccess[physicalQueue] = std::max(entry.LastAccess[physicalQueue], value);
    if (writes)
        entry.LastWrite[physicalQueue] = std::max(entry.LastWrite[physicalQueue], value);
    entry.LastPlan = m_Plan;
}

void RGCrossFrameAccesses::EndPlan()
{
    ++m_Plan;
    if (m_Plan <= kForgetAfterPlans)
        return;
    const uint64_t oldestKept = m_Plan - kForgetAfterPlans;
    m_Entries.EraseIf([oldestKept](const Key&, const Entry& entry) { return entry.LastPlan < oldestKept; });
}

void RGCrossFrameAccesses::Clear()
{
    m_Entries.Clear();
    for (uint64_t& completed : m_Completed)
        completed = 0;
}

} // namespace GameEngine::Rendering::RenderGraph
