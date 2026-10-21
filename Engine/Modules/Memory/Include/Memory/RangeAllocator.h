#pragma once

#include "Memory/Residency.h"
#include "Memory/StatsRecord.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace GameEngine::Memory
{

/// Hands out ranges of offsets into storage the caller owns (a GPU buffer, a vector of
/// slots), counted in the caller's units: a vertex, an index, a slot, a block of aligned
/// bytes. It never touches the storage.
///
///     Memory::RangeAllocator m_Ranges{"MyGame.Particles", Memory::Residency::Gpu,
///                                     sizeof(Particle), particleCapacity, 256};
///     Memory::RangeAllocator::Allocation range = m_Ranges.Allocate(count);
///     if (!range.IsValid()) { /* nothing fits: grow the storage elsewhere or refuse */ }
///     Write(buffer, range.Offset * sizeof(Particle), ...);
///     m_Ranges.FreeAfter(range, frameIndex + framesInFlight);   // the GPU may still read it
///     m_Ranges.ReclaimThrough(retiredFrameIndex);                // once per frame
///
/// - **Algorithm.** Best fit over two-level segregated bins: the first level is the size's
///   power of two, the second splits it into eight linear bins, and a bitmap per level finds
///   the first non-empty bin in constant time. `Allocate` takes the smallest free range that
///   fits, the lowest offset among equals, found in the request's own bin or else in the
///   first non-empty larger bin by walking those bins' lists (so its cost grows with the free
///   ranges of at most two bins, never with all of them), and splits it: the grant is exactly the
///   request, at the low end of the range it came from. `Free` merges the range with free
///   neighbors on both sides in constant time.
/// - **Release never allocates.** Range records and the deferred-free queue grow only inside
///   `Allocate` (geometrically), so `Free`, `FreeAfter` and `ReclaimThrough` are `noexcept`
///   and allocation-free: release in destructors and after a failure is safe.
/// - **Failure.** An invalid `Allocation` when nothing fits, or when its records cannot
///   grow; the allocator is then unchanged.
/// - **Threading.** None. The owner serializes every call (a lock, the frame, one thread).
/// - **Stats.** Registered under `statsName` (copied, at most 47 characters) in
///   `Memory::SnapshotStats` as `StatsKind::RangeAllocator`, in bytes (units times
///   `unitBytes`), written at every call that changes them. Not copyable, not movable: the
///   stats record lives inside the object.
class RangeAllocator
{
  public:
    /// The `Node` of an invalid `Allocation`.
    static constexpr std::uint32_t kNoNode = UINT32_MAX;

    /// A range handed out: `Offset` and `Size` in units. `Node` names the range's record
    /// inside the allocator, so `Free` finds it without a search; pass the `Allocation`
    /// back unchanged.
    struct Allocation
    {
        std::uint64_t Offset = 0;
        std::uint64_t Size = 0;
        std::uint32_t Node = kNoNode;

        bool IsValid() const { return Size != 0; }
    };

    /// Counts in units.
    struct Stats
    {
        std::uint64_t CapacityUnits = 0;
        /// Live ranges, including those waiting in `FreeAfter`'s queue.
        std::uint64_t UsedUnits = 0;
        std::uint64_t PeakUnits = 0;
        /// The end of the highest range ever handed out: the span of the caller's storage
        /// that has been written through this allocator. It never decreases.
        std::uint64_t EndUnits = 0;
        std::uint64_t LargestFreeUnits = 0;
        std::uint32_t FreeRanges = 0;
        /// Ranges waiting in `FreeAfter`'s queue.
        std::uint32_t PendingRanges = 0;
        /// The share of free units outside the largest free range: (free − largest) ÷ free,
        /// 0 when the free space is one range (or there is none).
        float Fragmentation = 0.0f;
    };

    /// `capacityUnits` is the size of the caller's storage, in units of `unitBytes` bytes
    /// (used only to report bytes). `initialRangeCapacity` is the number of ranges, free and
    /// live, the allocator has records for before its first growth. `statsName` must not be
    /// empty.
    RangeAllocator(std::string_view statsName, Residency residency, std::uint64_t unitBytes,
                   std::uint64_t capacityUnits, std::uint32_t initialRangeCapacity);
    ~RangeAllocator();

    // The stats record links into the process's registry at its address.
    RangeAllocator(const RangeAllocator&) = delete;
    RangeAllocator& operator=(const RangeAllocator&) = delete;
    RangeAllocator(RangeAllocator&&) = delete;
    RangeAllocator& operator=(RangeAllocator&&) = delete;

    /// A range of exactly `units`, or an invalid `Allocation` when nothing fits (or `units`
    /// is 0, or the records cannot grow).
    Allocation Allocate(std::uint64_t units);

    /// Returns `allocation` now. An invalid `allocation` does nothing.
    void Free(Allocation allocation) noexcept;

    /// Returns `allocation` at the first `ReclaimThrough(safeStamp)` with `safeStamp` at or
    /// past `retireStamp`: for a range the GPU may still read. Until then it stays in use.
    /// An invalid `allocation` does nothing.
    void FreeAfter(Allocation allocation, std::uint64_t retireStamp) noexcept;

    /// Returns every range queued by `FreeAfter` with a stamp at or below `safeStamp`.
    void ReclaimThrough(std::uint64_t safeStamp) noexcept;

    /// Grows `allocation` to `units` (more than its size) in place when the range after it
    /// is free and large enough; updates `allocation.Size` and returns true, else changes
    /// nothing and returns false.
    bool TryGrowInPlace(Allocation& allocation, std::uint64_t units);

    std::uint64_t Capacity() const { return m_Capacity; }
    /// Units in live ranges, including those waiting in `FreeAfter`'s queue.
    std::uint64_t Used() const { return m_Used; }
    /// True when no range is live or queued.
    bool IsEmpty() const { return m_Outstanding == 0; }

    /// Walks the largest non-empty bin for the largest free range; meant for tools, tests
    /// and debug views.
    Stats GetStats() const;

  private:
    static constexpr std::uint32_t kSecondLevelBits = 3;
    static constexpr std::uint32_t kSecondLevelCount = 1u << kSecondLevelBits;
    /// One first level per power of two from 2^3 (sizes below 8 share level 0, one bin each).
    static constexpr std::uint32_t kFirstLevelCount = 64 - kSecondLevelBits + 1;

    enum class NodeState : std::uint8_t
    {
        Unused,
        Free,
        Live,
        Pending,
    };

    /// One range, free or handed out, linked to its neighbors by offset and, while free, into
    /// its bin's list. Links are indices into `m_Nodes`, which grows; `Unused` records are
    /// chained through `NextFree`.
    struct Node
    {
        std::uint64_t Offset = 0;
        std::uint64_t Size = 0;
        std::uint32_t PreviousPhysical = kNoNode;
        std::uint32_t NextPhysical = kNoNode;
        std::uint32_t PreviousFree = kNoNode;
        std::uint32_t NextFree = kNoNode;
        NodeState State = NodeState::Unused;
    };

    struct PendingRange
    {
        std::uint64_t RetireStamp = 0;
        std::uint32_t Node = kNoNode;
    };

    struct Bin
    {
        std::uint32_t FirstLevel;
        std::uint32_t SecondLevel;
    };

    static Bin BinOf(std::uint64_t units);
    /// The smallest free range of at least `units` (the lowest offset among equals), or
    /// `kNoNode` when none fits.
    std::uint32_t FindFit(std::uint64_t units) const;
    /// The smallest range of at least `units` in the free list starting at `head`, the lowest
    /// offset among equals.
    std::uint32_t SmallestFit(std::uint32_t head, std::uint64_t units) const;
    void InsertFree(std::uint32_t node);
    void RemoveFree(std::uint32_t node);
    /// Unlinks `node` from its offset neighbors and returns its record to the unused chain.
    void RetireNode(std::uint32_t node);
    /// A record for a split remainder: from the unused chain, else appended (capacity
    /// provisioned by `Allocate`).
    std::uint32_t TakeNode();
    /// Marks `node` free, merges it with free neighbors and puts the result in its bin.
    void ReleaseNode(std::uint32_t node) noexcept;
    bool IsLiveAllocation(const Allocation& allocation) const;
    void PublishStats() noexcept;

    std::vector<Node> m_Nodes;
    std::vector<PendingRange> m_Pending;
    std::array<std::uint32_t, kFirstLevelCount * kSecondLevelCount> m_FreeHeads;
    std::array<std::uint8_t, kFirstLevelCount> m_SecondLevelMaps{};
    std::uint64_t m_FirstLevelMap = 0;
    std::uint32_t m_UnusedHead = kNoNode;
    std::uint32_t m_FreeRanges = 0;
    std::uint64_t m_Capacity = 0;
    std::uint64_t m_UnitBytes = 1;
    std::uint64_t m_Used = 0;
    std::uint64_t m_Peak = 0;
    std::uint64_t m_End = 0;
    /// Ranges handed out and not yet returned, queued ones included: the bound on the
    /// deferred queue's length.
    std::uint64_t m_Outstanding = 0;
    /// The lowest stamp in `m_Pending`, so a `ReclaimThrough` with nothing due returns at once.
    std::uint64_t m_EarliestPendingStamp = UINT64_MAX;
    Detail::StatsRecord m_Record;
};

} // namespace GameEngine::Memory
