// Memory::Arena's allocation, growth and reset: what an allocation returns, how blocks chain
// and coalesce, and that a warmed arena allocates nothing from the heap.

#include "Memory/AllocationCountScope.h"
#include "Memory/Arena.h"
#include "Memory/StatsSnapshot.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace
{
using GameEngine::Memory::AllocationCountScope;
using GameEngine::Memory::Arena;
using GameEngine::Memory::CountWindow;
using GameEngine::Memory::SnapshotStats;
using GameEngine::Memory::StatsEntry;
using GameEngine::Memory::StatsKind;

bool IsAligned(const void* pointer, std::size_t alignment)
{
    return (reinterpret_cast<std::uintptr_t>(pointer) & (alignment - 1)) == 0;
}

std::optional<StatsEntry> FindEntry(std::string_view name)
{
    for (StatsEntry& entry : SnapshotStats())
    {
        if (entry.Name == name)
            return std::move(entry);
    }
    return std::nullopt;
}

std::uint64_t BlockCount(std::string_view name)
{
    const std::optional<StatsEntry> entry = FindEntry(name);
    EXPECT_TRUE(entry.has_value()) << "no arena registered as " << name;
    return entry ? entry->Blocks : 0;
}

struct Tracked
{
    static int s_Live;
    static int s_Destroyed;
    int Value = 0;
    explicit Tracked(int value) : Value(value) { ++s_Live; }
    ~Tracked()
    {
        --s_Live;
        ++s_Destroyed;
    }
};
int Tracked::s_Live = 0;
int Tracked::s_Destroyed = 0;

/// One frame's worth of mixed allocations: 25.6 KiB of 16-byte-aligned requests.
void FillOneFrame(Arena& arena)
{
    constexpr std::array<std::size_t, 3> kSizes = {16, 64, 256};
    for (int allocation = 0; allocation < 240; ++allocation)
        ASSERT_NE(arena.Allocate(kSizes[static_cast<std::size_t>(allocation) % kSizes.size()]), nullptr);
}
} // namespace

TEST(ArenaTests, SingleAllocStoresValueAndAligns)
{
    Arena arena("Tests.Arena.SingleAlloc");
    int* integer = arena.New<int>(42);
    double* real = arena.New<double>(3.5);
    EXPECT_EQ(*integer, 42);
    EXPECT_DOUBLE_EQ(*real, 3.5);
    EXPECT_TRUE(IsAligned(integer, alignof(int)));
    EXPECT_TRUE(IsAligned(real, alignof(double)));
    EXPECT_NE(static_cast<void*>(integer), static_cast<void*>(real));
}

TEST(ArenaTests, OverAlignedAllocationRespectsAlignment)
{
    Arena arena("Tests.Arena.OverAligned");
    arena.New<char>('x'); // moves the cursor off the block's alignment
    EXPECT_TRUE(IsAligned(arena.Allocate(8, 64), 64));
    EXPECT_TRUE(IsAligned(arena.Allocate(8, 128), 128));
}

TEST(ArenaTests, AllocateArrayValueInitializesAndIsContiguous)
{
    Arena arena("Tests.Arena.Array");
    const std::span<std::uint32_t> span = arena.AllocateArray<std::uint32_t>(16);
    ASSERT_EQ(span.size(), 16u);
    for (std::uint32_t value : span)
        EXPECT_EQ(value, 0u) << "AllocateArray value-initialises; the 0xCD fill must not show";
    for (std::size_t index = 0; index < span.size(); ++index)
        span[index] = static_cast<std::uint32_t>(index * 7);
    EXPECT_EQ(&span[15] - &span[0], 15);
    EXPECT_EQ(span[15], 15u * 7u);
}

TEST(ArenaTests, ZeroCountArrayIsEmptyAndSafe)
{
    Arena arena("Tests.Arena.ZeroCount");
    EXPECT_TRUE(arena.AllocateArray<int>(0).empty());
}

TEST(ArenaTests, CopyArrayPreservesValues)
{
    Arena arena("Tests.Arena.Copy");
    const std::array<int, 5> source = {1, 2, 3, 4, 5};
    const std::span<int> copy = arena.CopyArray<int>(source);
    ASSERT_EQ(copy.size(), source.size());
    EXPECT_NE(copy.data(), source.data());
    for (std::size_t index = 0; index < source.size(); ++index)
        EXPECT_EQ(copy[index], source[index]);
}

// A small first block forces a chain; pointers handed out earlier stay valid (blocks chain,
// never move).
TEST(ArenaTests, GrowthChainsBlocksWithoutInvalidatingPointers)
{
    Arena arena("Tests.Arena.Growth", 256);
    std::vector<int*> pointers;
    for (int value = 0; value < 2000; ++value)
        pointers.push_back(arena.New<int>(value));
    EXPECT_GT(BlockCount("Tests.Arena.Growth"), 1u);
    for (int value = 0; value < 2000; ++value)
        ASSERT_EQ(*pointers[static_cast<std::size_t>(value)], value) << "pointer invalidated at " << value;
    const std::set<int*> unique(pointers.begin(), pointers.end());
    EXPECT_EQ(unique.size(), pointers.size());
}

// Reset empties the arena and keeps enough capacity for the frame it saw: the same fill
// again acquires no block.
TEST(ArenaTests, ResetRewindsAndKeepsTheHighWaterCapacity)
{
    Arena arena("Tests.Arena.ResetKeeps", 256);
    for (int value = 0; value < 2000; ++value)
        arena.New<int>(value);
    const std::size_t usedAfterFill = arena.BytesUsed();
    EXPECT_GT(usedAfterFill, 0u);

    arena.Reset();
    EXPECT_EQ(arena.BytesUsed(), 0u);
    EXPECT_GE(arena.BytesReserved(), usedAfterFill);
    const std::uint64_t blocksAfterReset = BlockCount("Tests.Arena.ResetKeeps");

    for (int value = 0; value < 2000; ++value)
        arena.New<int>(value);
    EXPECT_EQ(BlockCount("Tests.Arena.ResetKeeps"), blocksAfterReset);
}

TEST(ArenaTests, NewRunsDestructorsOnReset)
{
    Tracked::s_Live = 0;
    Tracked::s_Destroyed = 0;
    {
        Arena arena("Tests.Arena.NewReset");
        for (int value = 0; value < 10; ++value)
            arena.New<Tracked>(value);
        EXPECT_EQ(Tracked::s_Live, 10);
        EXPECT_EQ(arena.GetMarker().DestructorCount, 10u);

        arena.Reset();
        EXPECT_EQ(Tracked::s_Live, 0);
        EXPECT_EQ(Tracked::s_Destroyed, 10);
        EXPECT_EQ(arena.GetMarker().DestructorCount, 0u);
    }
    EXPECT_EQ(Tracked::s_Destroyed, 10) << "destroying the arena after Reset destroyed an object twice";
}

TEST(ArenaTests, DestructorsRunOnArenaDestruction)
{
    Tracked::s_Live = 0;
    Tracked::s_Destroyed = 0;
    {
        Arena arena("Tests.Arena.Destruction");
        arena.New<Tracked>(1);
        arena.New<Tracked>(2);
        EXPECT_EQ(Tracked::s_Live, 2);
    }
    EXPECT_EQ(Tracked::s_Live, 0);
    EXPECT_EQ(Tracked::s_Destroyed, 2);
}

TEST(ArenaTests, RewindRunsOnlyTheDestructorsAboveTheMarker)
{
    Tracked::s_Live = 0;
    Tracked::s_Destroyed = 0;
    Arena arena("Tests.Arena.RewindMarker");
    arena.New<Tracked>(1);
    arena.New<Tracked>(2);
    const Arena::Marker marker = arena.GetMarker();
    const std::size_t usedAtMarker = arena.BytesUsed();
    arena.New<Tracked>(3);
    arena.New<Tracked>(4);
    arena.New<Tracked>(5);

    arena.Rewind(marker);
    EXPECT_EQ(Tracked::s_Destroyed, 3);
    EXPECT_EQ(Tracked::s_Live, 2);
    EXPECT_EQ(arena.BytesUsed(), usedAtMarker);

    arena.Reset();
    EXPECT_EQ(Tracked::s_Destroyed, 5);
}

// Requests aligned well past the block alignment, in 64 KiB blocks, across several blocks.
TEST(ArenaTests, OverAlignmentOf4096IsServedIn64KiBBlocks)
{
    Arena arena("Tests.Arena.Align4096", 64 * 1024);
    for (int allocation = 0; allocation < 40; ++allocation)
    {
        arena.Allocate(1);
        void* aligned = arena.Allocate(3000, 4096);
        ASSERT_TRUE(IsAligned(aligned, 4096)) << "allocation " << allocation;
    }
    EXPECT_GT(BlockCount("Tests.Arena.Align4096"), 1u);
}

// The first frame grows a chain; Reset coalesces it into one block, and from then on a frame
// of the same shape bumps that block and allocates nothing.
TEST(ArenaTests, ResetCoalescesThenAllocatesNothingPerFrame)
{
    Arena arena("Tests.Arena.Coalesce", 4096);
    FillOneFrame(arena);
    ASSERT_GT(BlockCount("Tests.Arena.Coalesce"), 1u);
    arena.Reset();
    EXPECT_EQ(BlockCount("Tests.Arena.Coalesce"), 1u);

    const AllocationCountScope frames(CountWindow::ThisThread);
    for (int frame = 0; frame < 8; ++frame)
    {
        FillOneFrame(arena);
        arena.Reset();
    }
    EXPECT_EQ(frames.Count(), 0u) << "a steady frame on a coalesced arena reached the heap";
    EXPECT_EQ(BlockCount("Tests.Arena.Coalesce"), 1u);
}

TEST(ArenaTests, PmrVectorOnAWarmArenaMakesNoUpstreamAllocation)
{
    Arena arena("Tests.Arena.Pmr");
    arena.Allocate(1); // acquires the first block

    const AllocationCountScope scope(CountWindow::ThisThread);
    std::pmr::vector<int> values{&arena.Resource()};
    for (int value = 0; value < 1000; ++value)
        values.push_back(value);
    EXPECT_EQ(scope.Count(), 0u) << "a pmr container on the arena allocated from the heap";
    EXPECT_EQ(values[999], 999);
}

TEST(ArenaTests, TheSnapshotReportsTheArenaUnderItsName)
{
    Arena arena("Tests.Arena.Snapshot", 4096);
    arena.Allocate(1000);
    const std::optional<StatsEntry> entry = FindEntry("Tests.Arena.Snapshot");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->Kind, StatsKind::Arena);
    EXPECT_EQ(entry->Bytes, 1000u);
    EXPECT_EQ(entry->ReservedBytes, 4096u);
    EXPECT_EQ(entry->Blocks, 1u);

    for (int allocation = 0; allocation < 8; ++allocation)
        arena.Allocate(64);
    const std::size_t peak = arena.BytesUsed();
    arena.Reset();
    const std::optional<StatsEntry> afterReset = FindEntry("Tests.Arena.Snapshot");
    ASSERT_TRUE(afterReset.has_value());
    EXPECT_EQ(afterReset->Bytes, 0u);
    EXPECT_EQ(afterReset->HighWaterBytes, peak) << "the high water is the most in use before the Reset";
}

// Two allocations in one block: the second moves no block, so only Rewind can record the peak.
TEST(ArenaTests, TheHighWaterRecordsThePeakBeforeARewind)
{
    Arena arena("Tests.Arena.RewindHighWater", 4096);
    const Arena::Marker start = arena.GetMarker();
    arena.Allocate(1000);
    arena.Allocate(2000);
    const std::size_t peak = arena.BytesUsed();
    arena.Rewind(start);
    const std::optional<StatsEntry> entry = FindEntry("Tests.Arena.RewindHighWater");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->Bytes, 0u);
    EXPECT_EQ(entry->HighWaterBytes, peak) << "the high water is the most in use before the Rewind";
}
