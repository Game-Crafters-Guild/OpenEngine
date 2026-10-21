// Memory::RangeAllocator's ranges: exact grants split from the low end, the request's own bin
// reused before a larger one is split, merging with free neighbors on both sides, refusal,
// deferred release by stamp, growth in place, randomized churn against a model of the offset
// space, that release never allocates, that a failed record growth changes nothing, and the
// stats.

#include "Memory/RangeAllocator.h"
#include "Memory/StatsSnapshot.h"
#include "Memory/Testing/ScopedAllocationFault.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

namespace
{
using GameEngine::Memory::RangeAllocator;
using GameEngine::Memory::Residency;
using GameEngine::Memory::SnapshotStats;
using GameEngine::Memory::StatsEntry;
using GameEngine::Memory::StatsKind;
using GameEngine::Memory::Testing::FaultRepeat;
using GameEngine::Memory::Testing::ScopedAllocationFault;
using Allocation = RangeAllocator::Allocation;

constexpr std::uint64_t kCapacity = 4096;

std::optional<StatsEntry> FindEntry(std::string_view name)
{
    for (StatsEntry& entry : SnapshotStats())
    {
        if (entry.Name == name)
            return entry;
    }
    return std::nullopt;
}

/// Sets every unit of `range` in `owner` to `value`; false when a unit already holds it (a
/// range handed out twice, or freed twice).
bool MarkUnits(std::vector<std::uint8_t>& owner, const Allocation& range, std::uint8_t value)
{
    for (std::uint64_t unit = range.Offset; unit < range.Offset + range.Size; ++unit)
    {
        if (owner[unit] == value)
            return false;
        owner[unit] = value;
    }
    return true;
}

/// The longest run of units neither live in `owner` nor waiting in `pending` for a reclaim.
std::uint64_t LongestFreeRun(const std::vector<std::uint8_t>& owner,
                             const std::vector<std::pair<std::uint64_t, Allocation>>& pending)
{
    std::vector<std::uint8_t> taken = owner;
    for (const auto& entry : pending)
        std::fill_n(taken.begin() + static_cast<std::ptrdiff_t>(entry.second.Offset), entry.second.Size, 1);
    std::uint64_t longest = 0;
    std::uint64_t run = 0;
    for (const std::uint8_t unit : taken)
    {
        run = unit == 0 ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    return longest;
}
} // namespace

// A grant is exactly the request, cut from the low end of the free range it came from, and the
// rest stays one free range.
TEST(RangeAllocatorTests, AllocateGrantsExactlyTheRequestFromTheLowEnd)
{
    RangeAllocator ranges("RangeAllocatorTests.Exact", Residency::Cpu, 1, kCapacity, 4);
    const Allocation first = ranges.Allocate(13);
    const Allocation second = ranges.Allocate(100);
    ASSERT_TRUE(first.IsValid());
    ASSERT_TRUE(second.IsValid());
    EXPECT_EQ(first.Offset, 0u);
    EXPECT_EQ(first.Size, 13u);
    EXPECT_EQ(second.Offset, 13u);
    EXPECT_EQ(second.Size, 100u);

    const RangeAllocator::Stats stats = ranges.GetStats();
    EXPECT_EQ(stats.UsedUnits, 113u);
    EXPECT_EQ(stats.FreeRanges, 1u);
    EXPECT_EQ(stats.LargestFreeUnits, kCapacity - 113);
    EXPECT_EQ(stats.EndUnits, 113u);
    EXPECT_EQ(stats.Fragmentation, 0.0f);
}

// A hole of the request's own size is reused before a larger free range is split.
TEST(RangeAllocatorTests, AHoleOfTheRequestsSizeIsReusedBeforeALargerRangeIsSplit)
{
    RangeAllocator ranges("RangeAllocatorTests.Reuse", Residency::Cpu, 1, kCapacity, 4);
    const Allocation left = ranges.Allocate(20);
    const Allocation hole = ranges.Allocate(20);
    const Allocation right = ranges.Allocate(20);
    ranges.Free(hole);

    const Allocation reused = ranges.Allocate(20);
    EXPECT_EQ(reused.Offset, hole.Offset);
    EXPECT_EQ(ranges.GetStats().FreeRanges, 1u);
    ranges.Free(left);
    ranges.Free(reused);
    ranges.Free(right);
}

// Allocate takes the smallest free range that fits, and the lowest offset among equal ones,
// whichever was freed last.
TEST(RangeAllocatorTests, AllocateTakesTheSmallestFitAtTheLowestOffset)
{
    RangeAllocator ranges("RangeAllocatorTests.BestFit", Residency::Cpu, 1, kCapacity, 16);
    std::array<Allocation, 7> live{};
    const std::array<std::uint64_t, 7> sizes = {24, 1, 24, 1, 25, 1, 1};
    for (std::size_t index = 0; index < live.size(); ++index)
        live[index] = ranges.Allocate(sizes[index]);
    // Holes of 24 (offset 0), 24 (offset 25) and 25 (offset 50), freed lowest first, so the
    // 25 heads its bin's list and the lower 24 is last.
    ranges.Free(live[0]);
    ranges.Free(live[2]);
    ranges.Free(live[4]);

    const Allocation fit = ranges.Allocate(22);
    EXPECT_EQ(fit.Offset, live[0].Offset);
    EXPECT_EQ(ranges.Allocate(23).Offset, live[2].Offset);
}

// A freed range merges with the free range on each side, so freeing every range leaves one
// free range of the whole capacity, whatever the order.
TEST(RangeAllocatorTests, FreeMergesWithTheFreeNeighborOnEachSide)
{
    RangeAllocator ranges("RangeAllocatorTests.Merge", Residency::Cpu, 1, kCapacity, 4);
    const Allocation a = ranges.Allocate(64);
    const Allocation b = ranges.Allocate(64);
    const Allocation c = ranges.Allocate(64);
    ranges.Free(a);
    ranges.Free(c); // merges with the tail on its right
    EXPECT_EQ(ranges.GetStats().FreeRanges, 2u);
    ranges.Free(b); // merges with a on its left and the tail on its right
    const RangeAllocator::Stats stats = ranges.GetStats();
    EXPECT_EQ(stats.FreeRanges, 1u);
    EXPECT_EQ(stats.LargestFreeUnits, kCapacity);
    EXPECT_TRUE(ranges.IsEmpty());
    EXPECT_EQ(ranges.Allocate(kCapacity).Offset, 0u);
}

// A request larger than any free range, or of zero units, is refused and changes nothing.
TEST(RangeAllocatorTests, ARequestThatDoesNotFitIsRefused)
{
    RangeAllocator ranges("RangeAllocatorTests.Refuse", Residency::Cpu, 1, kCapacity, 4);
    EXPECT_FALSE(ranges.Allocate(0).IsValid());
    EXPECT_FALSE(ranges.Allocate(kCapacity + 1).IsValid());
    const Allocation a = ranges.Allocate(kCapacity / 2);
    const Allocation b = ranges.Allocate(kCapacity / 2);
    ASSERT_TRUE(b.IsValid());
    ranges.Free(a);
    EXPECT_FALSE(ranges.Allocate(kCapacity / 2 + 1).IsValid());
    EXPECT_EQ(ranges.Used(), kCapacity / 2);
    ranges.Free(b);
}

// FreeAfter keeps a range in use until ReclaimThrough reaches its stamp; stamps may arrive in
// any order.
TEST(RangeAllocatorTests, FreeAfterHoldsTheRangeUntilReclaimThroughReachesItsStamp)
{
    RangeAllocator ranges("RangeAllocatorTests.Deferred", Residency::Gpu, 16, 256, 4);
    const Allocation late = ranges.Allocate(128);
    const Allocation early = ranges.Allocate(128);
    ranges.FreeAfter(late, 7);
    ranges.FreeAfter(early, 5);
    EXPECT_FALSE(ranges.Allocate(1).IsValid());
    EXPECT_EQ(ranges.GetStats().PendingRanges, 2u);

    ranges.ReclaimThrough(4);
    EXPECT_EQ(ranges.Used(), 256u);
    ranges.ReclaimThrough(5);
    EXPECT_EQ(ranges.Used(), 128u);
    EXPECT_EQ(ranges.GetStats().PendingRanges, 1u);
    ranges.ReclaimThrough(9);
    EXPECT_TRUE(ranges.IsEmpty());
    EXPECT_EQ(ranges.GetStats().FreeRanges, 1u);
}

// A range grows in place only into a free range right after it; a range followed by a live one
// stays as it is.
TEST(RangeAllocatorTests, TryGrowInPlaceTakesOnlyTheFreeRangeAfterIt)
{
    RangeAllocator ranges("RangeAllocatorTests.Grow", Residency::Cpu, 1, 100, 4);
    Allocation blocked = ranges.Allocate(10);
    Allocation tail = ranges.Allocate(10);
    EXPECT_FALSE(ranges.TryGrowInPlace(blocked, 11));
    EXPECT_EQ(blocked.Size, 10u);

    ASSERT_TRUE(ranges.TryGrowInPlace(tail, 50));
    EXPECT_EQ(tail.Offset, 10u);
    EXPECT_EQ(tail.Size, 50u);
    ASSERT_TRUE(ranges.TryGrowInPlace(tail, 90)); // takes the whole free range
    EXPECT_EQ(ranges.GetStats().FreeRanges, 0u);
    EXPECT_FALSE(ranges.TryGrowInPlace(tail, 91));
    EXPECT_EQ(ranges.Used(), 100u);
    ranges.Free(blocked);
    ranges.Free(tail);
    EXPECT_EQ(ranges.GetStats().LargestFreeUnits, 100u);
}

// Seeded churn of allocations, frees, deferred frees and growth, checked after every step
// against a model of the offset space: live ranges never overlap and stay inside the capacity,
// the used and free counts agree with the model, and a request is refused only when no free run
// of the model fits it.
TEST(RangeAllocatorTests, RandomizedChurnKeepsRangesDisjointAndAccounted)
{
    constexpr std::uint64_t kSpace = 1 << 14;
    RangeAllocator ranges("RangeAllocatorTests.Churn", Residency::Cpu, 1, kSpace, 8);
    std::vector<std::uint8_t> owner(kSpace, 0);
    std::vector<Allocation> live;
    std::uint64_t stamp = 0;
    std::uint64_t pendingUnits = 0;
    std::vector<std::pair<std::uint64_t, Allocation>> pending;
    std::mt19937 random(0x5EEDu);

    for (int step = 0; step < 20000; ++step)
    {
        const std::uint32_t action = random() % 8;
        if (action < 4 || live.empty())
        {
            const std::uint64_t units = 1 + (random() % 4 == 0 ? random() % 600 : random() % 24);
            const Allocation range = ranges.Allocate(units);
            if (range.IsValid())
            {
                ASSERT_EQ(range.Size, units);
                ASSERT_LE(range.Offset + range.Size, kSpace);
                ASSERT_TRUE(MarkUnits(owner, range, 1)) << "step " << step;
                live.push_back(range);
            }
            else
            {
                ASSERT_LT(LongestFreeRun(owner, pending), units)
                    << "step " << step << ": refused " << units << " units while a free run fits them";
            }
        }
        else
        {
            const std::size_t index = random() % live.size();
            Allocation& range = live[index];
            if (action == 4)
            {
                const std::uint64_t old = range.Size;
                const std::uint64_t grown = old + 1 + random() % 16;
                const std::uint64_t oldEnd = range.Offset + old;
                if (ranges.TryGrowInPlace(range, grown))
                    ASSERT_TRUE(MarkUnits(owner, {oldEnd, grown - old, 0}, 1)) << "step " << step;
                else
                    ASSERT_EQ(range.Size, old);
            }
            else
            {
                ASSERT_TRUE(MarkUnits(owner, range, 0)) << "step " << step;
                if (action == 5)
                {
                    ranges.FreeAfter(range, stamp + 2);
                    pending.push_back({stamp + 2, range});
                    pendingUnits += range.Size;
                }
                else
                {
                    ranges.Free(range);
                }
                range = live.back();
                live.pop_back();
            }
        }
        if (step % 64 == 0)
        {
            ++stamp;
            ranges.ReclaimThrough(stamp);
            std::erase_if(pending, [&](const auto& entry) {
                if (entry.first > stamp)
                    return false;
                pendingUnits -= entry.second.Size;
                return true;
            });
        }

        std::uint64_t liveUnits = 0;
        for (const Allocation& range : live)
            liveUnits += range.Size;
        ASSERT_EQ(ranges.Used(), liveUnits + pendingUnits) << "step " << step;
    }

    for (const Allocation& range : live)
        ranges.Free(range);
    ranges.ReclaimThrough(UINT64_MAX);
    const RangeAllocator::Stats stats = ranges.GetStats();
    EXPECT_TRUE(ranges.IsEmpty());
    EXPECT_EQ(stats.FreeRanges, 1u);
    EXPECT_EQ(stats.LargestFreeUnits, kSpace);
}

// Free, FreeAfter and ReclaimThrough allocate nothing, even after the free ranges fragment and
// when many ranges wait at once: Allocate provisioned their records and queue entries.
TEST(RangeAllocatorTests, ReleaseNeverAllocates)
{
    RangeAllocator ranges("RangeAllocatorTests.Release", Residency::Cpu, 1, kCapacity, 1);
    std::array<Allocation, 96> live{};
    for (Allocation& range : live)
        range = ranges.Allocate(8);

    bool threw = false;
    bool denied = false;
    {
        ScopedAllocationFault fault{0, 1, FaultRepeat::Every};
        try
        {
            for (std::size_t index = 0; index < live.size(); index += 3)
                ranges.Free(live[index]);
            for (std::size_t index = 1; index < live.size(); index += 3)
                ranges.FreeAfter(live[index], 1);
            for (std::size_t index = 2; index < live.size(); index += 3)
                ranges.FreeAfter(live[index], 2);
            ranges.ReclaimThrough(1);
            ranges.ReclaimThrough(2);
        }
        catch (const std::bad_alloc&)
        {
            threw = true;
        }
        denied = fault.Fired();
    }
    EXPECT_FALSE(threw);
    EXPECT_FALSE(denied);
    EXPECT_TRUE(ranges.IsEmpty());
    EXPECT_EQ(ranges.GetStats().FreeRanges, 1u);
}

// When its records cannot grow, Allocate refuses and leaves every count and range as it was;
// the next Allocate succeeds and every range is still returnable.
TEST(RangeAllocatorTests, AFailedRecordGrowthLeavesTheAllocatorUnchanged)
{
    RangeAllocator ranges("RangeAllocatorTests.Growth", Residency::Cpu, 1, kCapacity, 1);
    std::vector<Allocation> live;
    live.reserve(64);
    bool refused = false;
    while (!refused && live.size() < 63)
    {
        const RangeAllocator::Stats before = ranges.GetStats();
        Allocation range;
        {
            ScopedAllocationFault fault{0, 1, FaultRepeat::Once};
            range = ranges.Allocate(16);
        }
        if (range.IsValid())
        {
            live.push_back(range);
            continue;
        }
        refused = true;
        const RangeAllocator::Stats after = ranges.GetStats();
        EXPECT_EQ(after.UsedUnits, before.UsedUnits);
        EXPECT_EQ(after.FreeRanges, before.FreeRanges);
        EXPECT_EQ(after.LargestFreeUnits, before.LargestFreeUnits);
    }
    ASSERT_TRUE(refused);
    live.push_back(ranges.Allocate(16));
    ASSERT_TRUE(live.back().IsValid());
    for (const Allocation& range : live)
        ranges.Free(range);
    EXPECT_TRUE(ranges.IsEmpty());
    EXPECT_EQ(ranges.GetStats().LargestFreeUnits, kCapacity);
}

// The snapshot reports the allocator under its name, in bytes: units times the unit size.
TEST(RangeAllocatorTests, TheSnapshotReportsTheAllocatorInBytes)
{
    RangeAllocator ranges("RangeAllocatorTests.Stats", Residency::Gpu, 32, kCapacity, 4);
    const Allocation a = ranges.Allocate(10);
    const Allocation b = ranges.Allocate(6);
    ranges.Free(a);

    const std::optional<StatsEntry> entry = FindEntry("RangeAllocatorTests.Stats");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->Kind, StatsKind::RangeAllocator);
    EXPECT_EQ(entry->Residency, Residency::Gpu);
    EXPECT_EQ(entry->Bytes, 6u * 32u);
    EXPECT_EQ(entry->HighWaterBytes, 16u * 32u);
    EXPECT_EQ(entry->ReservedBytes, 16u * 32u);
    EXPECT_EQ(entry->Blocks, 1u);
    ranges.Free(b);
}
