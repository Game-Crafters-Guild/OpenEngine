#include "Memory/RangeAllocator.h"

#include "StatsRegistry.h"
#include "Types/GeometricReserve.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <new>

namespace GameEngine::Memory
{

RangeAllocator::RangeAllocator(std::string_view statsName, Residency residency, std::uint64_t unitBytes,
                               std::uint64_t capacityUnits, std::uint32_t initialRangeCapacity)
    : m_Capacity(capacityUnits), m_UnitBytes(unitBytes)
{
    assert(!statsName.empty() && "a RangeAllocator needs a stats name");
    assert(unitBytes != 0 && "a RangeAllocator's unit is at least one byte");
    m_FreeHeads.fill(kNoNode);
    m_Nodes.reserve(std::max<std::uint32_t>(initialRangeCapacity, 1));
    m_Pending.reserve(initialRangeCapacity);
    if (capacityUnits != 0)
    {
        m_Nodes.push_back(Node{0, capacityUnits, kNoNode, kNoNode, kNoNode, kNoNode, NodeState::Free});
        InsertFree(0);
    }
    Detail::RegisterStats(m_Record, statsName, StatsKind::RangeAllocator, residency);
}

RangeAllocator::~RangeAllocator()
{
    Detail::UnregisterStats(m_Record);
}

RangeAllocator::Bin RangeAllocator::BinOf(std::uint64_t units)
{
    if (units < kSecondLevelCount)
        return {0, static_cast<std::uint32_t>(units)};
    const auto log2 = static_cast<std::uint32_t>(std::bit_width(units) - 1);
    return {log2 - (kSecondLevelBits - 1),
            static_cast<std::uint32_t>(units >> (log2 - kSecondLevelBits)) & (kSecondLevelCount - 1)};
}

std::uint32_t RangeAllocator::SmallestFit(std::uint32_t head, std::uint64_t units) const
{
    std::uint32_t best = kNoNode;
    for (std::uint32_t node = head; node != kNoNode; node = m_Nodes[node].NextFree)
    {
        const Node& candidate = m_Nodes[node];
        if (candidate.Size < units)
            continue;
        if (best == kNoNode || candidate.Size < m_Nodes[best].Size ||
            (candidate.Size == m_Nodes[best].Size && candidate.Offset < m_Nodes[best].Offset))
            best = node;
    }
    return best;
}

std::uint32_t RangeAllocator::FindFit(std::uint64_t units) const
{
    // The smallest fitting range is in the request's own bin, or else in the first non-empty
    // larger bin, where every range fits.
    const Bin own = BinOf(units);
    const std::uint32_t ownFit = SmallestFit(m_FreeHeads[own.FirstLevel * kSecondLevelCount + own.SecondLevel], units);
    if (ownFit != kNoNode)
        return ownFit;

    const std::uint32_t larger = own.SecondLevel + 1;
    const std::uint32_t sameLevel =
        larger < kSecondLevelCount ? (m_SecondLevelMaps[own.FirstLevel] & (~0u << larger)) : 0u;
    Bin found{own.FirstLevel, 0};
    if (sameLevel != 0)
    {
        found.SecondLevel = static_cast<std::uint32_t>(std::countr_zero(sameLevel));
    }
    else
    {
        const std::uint32_t nextLevel = own.FirstLevel + 1;
        const std::uint64_t levels = nextLevel < 64 ? (m_FirstLevelMap & (~0ull << nextLevel)) : 0ull;
        if (levels == 0)
            return kNoNode;
        found.FirstLevel = static_cast<std::uint32_t>(std::countr_zero(levels));
        found.SecondLevel = static_cast<std::uint32_t>(
            std::countr_zero(static_cast<std::uint32_t>(m_SecondLevelMaps[found.FirstLevel])));
    }
    return SmallestFit(m_FreeHeads[found.FirstLevel * kSecondLevelCount + found.SecondLevel], units);
}

void RangeAllocator::InsertFree(std::uint32_t node)
{
    Node& record = m_Nodes[node];
    const Bin bin = BinOf(record.Size);
    std::uint32_t& head = m_FreeHeads[bin.FirstLevel * kSecondLevelCount + bin.SecondLevel];
    record.State = NodeState::Free;
    record.PreviousFree = kNoNode;
    record.NextFree = head;
    if (head != kNoNode)
        m_Nodes[head].PreviousFree = node;
    head = node;
    m_SecondLevelMaps[bin.FirstLevel] |= static_cast<std::uint8_t>(1u << bin.SecondLevel);
    m_FirstLevelMap |= 1ull << bin.FirstLevel;
    ++m_FreeRanges;
}

void RangeAllocator::RemoveFree(std::uint32_t node)
{
    Node& record = m_Nodes[node];
    if (record.NextFree != kNoNode)
        m_Nodes[record.NextFree].PreviousFree = record.PreviousFree;
    if (record.PreviousFree != kNoNode)
    {
        m_Nodes[record.PreviousFree].NextFree = record.NextFree;
    }
    else
    {
        const Bin bin = BinOf(record.Size);
        std::uint32_t& head = m_FreeHeads[bin.FirstLevel * kSecondLevelCount + bin.SecondLevel];
        head = record.NextFree;
        if (head == kNoNode)
        {
            m_SecondLevelMaps[bin.FirstLevel] &= static_cast<std::uint8_t>(~(1u << bin.SecondLevel));
            if (m_SecondLevelMaps[bin.FirstLevel] == 0)
                m_FirstLevelMap &= ~(1ull << bin.FirstLevel);
        }
    }
    record.PreviousFree = kNoNode;
    record.NextFree = kNoNode;
    --m_FreeRanges;
}

void RangeAllocator::RetireNode(std::uint32_t node)
{
    Node& record = m_Nodes[node];
    if (record.PreviousPhysical != kNoNode)
        m_Nodes[record.PreviousPhysical].NextPhysical = record.NextPhysical;
    if (record.NextPhysical != kNoNode)
        m_Nodes[record.NextPhysical].PreviousPhysical = record.PreviousPhysical;
    record = Node{};
    record.NextFree = m_UnusedHead;
    m_UnusedHead = node;
}

std::uint32_t RangeAllocator::TakeNode()
{
    if (m_UnusedHead != kNoNode)
    {
        const std::uint32_t node = m_UnusedHead;
        m_UnusedHead = m_Nodes[node].NextFree;
        m_Nodes[node].NextFree = kNoNode;
        return node;
    }
    assert(m_Nodes.size() < m_Nodes.capacity() && "Allocate provisions a record before it splits");
    m_Nodes.emplace_back();
    return static_cast<std::uint32_t>(m_Nodes.size() - 1);
}

RangeAllocator::Allocation RangeAllocator::Allocate(std::uint64_t units)
{
    if (units == 0 || units > m_Capacity)
        return {};
    const std::uint32_t node = FindFit(units);
    if (node == kNoNode)
        return {};

    // Provision before anything changes: a split may need one record, and every outstanding
    // range may later wait in the deferred queue at once.
    try
    {
        if (m_UnusedHead == kNoNode)
            ReserveGeometric(m_Nodes, m_Nodes.size() + 1);
        ReserveGeometric(m_Pending, static_cast<std::size_t>(m_Outstanding) + 1);
    }
    catch (const std::bad_alloc&)
    {
        return {};
    }

    RemoveFree(node);
    if (m_Nodes[node].Size > units)
    {
        const std::uint32_t remainder = TakeNode();
        Node& taken = m_Nodes[node];
        Node& rest = m_Nodes[remainder];
        rest.Offset = taken.Offset + units;
        rest.Size = taken.Size - units;
        rest.PreviousPhysical = node;
        rest.NextPhysical = taken.NextPhysical;
        if (taken.NextPhysical != kNoNode)
            m_Nodes[taken.NextPhysical].PreviousPhysical = remainder;
        taken.NextPhysical = remainder;
        taken.Size = units;
        InsertFree(remainder);
    }

    Node& taken = m_Nodes[node];
    taken.State = NodeState::Live;
    m_Used += units;
    m_Peak = std::max(m_Peak, m_Used);
    m_End = std::max(m_End, taken.Offset + units);
    ++m_Outstanding;
    PublishStats();
    return {taken.Offset, units, node};
}

bool RangeAllocator::IsLiveAllocation(const Allocation& allocation) const
{
    return allocation.Node < m_Nodes.size() && m_Nodes[allocation.Node].State == NodeState::Live &&
           m_Nodes[allocation.Node].Offset == allocation.Offset && m_Nodes[allocation.Node].Size == allocation.Size;
}

void RangeAllocator::ReleaseNode(std::uint32_t node) noexcept
{
    m_Used -= m_Nodes[node].Size;
    --m_Outstanding;

    const std::uint32_t previous = m_Nodes[node].PreviousPhysical;
    if (previous != kNoNode && m_Nodes[previous].State == NodeState::Free)
    {
        RemoveFree(previous);
        m_Nodes[previous].Size += m_Nodes[node].Size;
        RetireNode(node);
        node = previous;
    }
    const std::uint32_t next = m_Nodes[node].NextPhysical;
    if (next != kNoNode && m_Nodes[next].State == NodeState::Free)
    {
        RemoveFree(next);
        m_Nodes[node].Size += m_Nodes[next].Size;
        RetireNode(next);
    }
    InsertFree(node);
}

void RangeAllocator::Free(Allocation allocation) noexcept
{
    if (!allocation.IsValid())
        return;
    assert(IsLiveAllocation(allocation) && "this range is not live in this RangeAllocator: freed twice, "
                                           "queued by FreeAfter, or from another allocator");
    ReleaseNode(allocation.Node);
    PublishStats();
}

void RangeAllocator::FreeAfter(Allocation allocation, std::uint64_t retireStamp) noexcept
{
    if (!allocation.IsValid())
        return;
    assert(IsLiveAllocation(allocation) && "this range is not live in this RangeAllocator: freed twice, "
                                           "queued by FreeAfter, or from another allocator");
    assert(m_Pending.size() < m_Pending.capacity() && "Allocate provisions a queue entry per outstanding range");
    m_Nodes[allocation.Node].State = NodeState::Pending;
    m_Pending.push_back({retireStamp, allocation.Node});
    m_EarliestPendingStamp = std::min(m_EarliestPendingStamp, retireStamp);
}

void RangeAllocator::ReclaimThrough(std::uint64_t safeStamp) noexcept
{
    if (safeStamp < m_EarliestPendingStamp)
        return;
    std::uint64_t earliest = UINT64_MAX;
    std::size_t kept = 0;
    for (const PendingRange& pending : m_Pending)
    {
        if (pending.RetireStamp <= safeStamp)
        {
            ReleaseNode(pending.Node);
            continue;
        }
        earliest = std::min(earliest, pending.RetireStamp);
        m_Pending[kept++] = pending;
    }
    m_Pending.resize(kept);
    m_EarliestPendingStamp = earliest;
    PublishStats();
}

bool RangeAllocator::TryGrowInPlace(Allocation& allocation, std::uint64_t units)
{
    assert(IsLiveAllocation(allocation) && "TryGrowInPlace needs a live range of this RangeAllocator");
    assert(units > allocation.Size && "TryGrowInPlace grows: the range already holds this many units");
    const std::uint32_t next = m_Nodes[allocation.Node].NextPhysical;
    if (next == kNoNode || m_Nodes[next].State != NodeState::Free ||
        m_Nodes[next].Size < units - allocation.Size)
        return false;

    const std::uint64_t delta = units - allocation.Size;
    RemoveFree(next);
    if (m_Nodes[next].Size == delta)
    {
        RetireNode(next);
    }
    else
    {
        m_Nodes[next].Offset += delta;
        m_Nodes[next].Size -= delta;
        InsertFree(next);
    }
    m_Nodes[allocation.Node].Size = units;
    allocation.Size = units;
    m_Used += delta;
    m_Peak = std::max(m_Peak, m_Used);
    m_End = std::max(m_End, allocation.Offset + units);
    PublishStats();
    return true;
}

RangeAllocator::Stats RangeAllocator::GetStats() const
{
    Stats stats{};
    stats.CapacityUnits = m_Capacity;
    stats.UsedUnits = m_Used;
    stats.PeakUnits = m_Peak;
    stats.EndUnits = m_End;
    stats.FreeRanges = m_FreeRanges;
    stats.PendingRanges = static_cast<std::uint32_t>(m_Pending.size());
    if (m_FirstLevelMap != 0)
    {
        const auto firstLevel = static_cast<std::uint32_t>(std::bit_width(m_FirstLevelMap) - 1);
        const auto secondLevel =
            static_cast<std::uint32_t>(std::bit_width(static_cast<std::uint32_t>(m_SecondLevelMaps[firstLevel])) - 1);
        for (std::uint32_t node = m_FreeHeads[firstLevel * kSecondLevelCount + secondLevel]; node != kNoNode;
             node = m_Nodes[node].NextFree)
            stats.LargestFreeUnits = std::max(stats.LargestFreeUnits, m_Nodes[node].Size);
    }
    const std::uint64_t freeUnits = m_Capacity - m_Used;
    if (freeUnits != 0)
        stats.Fragmentation =
            static_cast<float>(freeUnits - stats.LargestFreeUnits) / static_cast<float>(freeUnits);
    return stats;
}

void RangeAllocator::PublishStats() noexcept
{
    m_Record.Bytes.store(m_Used * m_UnitBytes, std::memory_order_relaxed);
    m_Record.HighWaterBytes.store(m_Peak * m_UnitBytes, std::memory_order_relaxed);
    m_Record.ReservedBytes.store(m_End * m_UnitBytes, std::memory_order_relaxed);
    m_Record.Blocks.store(m_Outstanding, std::memory_order_relaxed);
}

} // namespace GameEngine::Memory
