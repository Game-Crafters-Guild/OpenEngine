// Memory::ScratchScope: nesting on one thread (plain, across a participating Wait, through the
// inline pool's trampoline, past one chunk of frame storage), allocation through an outer
// scope, an outer container across many inner scopes, the Debug checks, thread exit,
// retention, and reads after a close under AddressSanitizer.

#include "Memory/AllocationCountScope.h"
#include "Memory/ScratchScope.h"
#include "Memory/StatsSnapshot.h"

#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace
{
using GameEngine::Memory::AllocationCountScope;
using GameEngine::Memory::CountWindow;
using GameEngine::Memory::ScratchScope;
using GameEngine::Memory::StatsEntry;
using GameEngine::Memory::StatsKind;
using JobSystem::JobCounter;
using JobSystem::WorkStealingThreadPool;

/// The id the scratch stats record names this thread by.
std::uint64_t ThisOsThreadId()
{
#if defined(_WIN32)
    return ::GetCurrentThreadId();
#elif defined(__APPLE__)
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#else
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif
}

/// This thread's scratch entry, if it has opened a scope.
std::optional<StatsEntry> FindThisThreadsScratch()
{
    const std::uint64_t id = ThisOsThreadId();
    for (const StatsEntry& entry : GameEngine::Memory::SnapshotStats())
        if (entry.Kind == StatsKind::Scratch && entry.ThreadId == id)
            return entry;
    return std::nullopt;
}

void Fill(std::span<std::uint32_t> values, std::uint32_t seed)
{
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = seed + static_cast<std::uint32_t>(i);
}

bool Holds(std::span<const std::uint32_t> values, std::uint32_t seed)
{
    for (std::size_t i = 0; i < values.size(); ++i)
        if (values[i] != seed + static_cast<std::uint32_t>(i))
            return false;
    return true;
}

/// Opens one scope per level down to `deepest`, each holding its own level number, and checks
/// every level's value after the levels below it have closed.
void NestTo(int level, int deepest, int& reached)
{
    ScratchScope scratch;
    int* mine = scratch.New<int>(level);
    reached = std::max(reached, level);
    if (level < deepest)
        NestTo(level + 1, deepest, reached);
    EXPECT_EQ(*mine, level);
}

/// The same, each level run as a unit of an inline pool: the trampoline executes it at publish.
void NestInlineUnits(WorkStealingThreadPool& pool, int level, int deepest, int& reached)
{
    ScratchScope scratch;
    int* mine = scratch.New<int>(level);
    reached = std::max(reached, level);
    if (level < deepest)
    {
        JobCounter counter;
        pool.Run([&pool, level, deepest, &reached] { NestInlineUnits(pool, level + 1, deepest, reached); }, counter);
        pool.Wait(counter);
    }
    EXPECT_EQ(*mine, level);
}

/// Opens one scope per level down to `deepest`; the deepest grows `grown`, a container on a scope
/// opened before them all.
void GrowAtDepth(int level, int deepest, std::pmr::vector<std::uint32_t>& grown)
{
    ScratchScope scratch;
    scratch.Allocate(16);
    if (level < deepest)
    {
        GrowAtDepth(level + 1, deepest, grown);
        return;
    }
    grown.resize(1024);
    Fill(grown, 42);
}
} // namespace

TEST(ScratchScopeTests, NestedScopesRewindToTheirOwnMarks)
{
    ScratchScope outer;
    std::span<std::uint32_t> kept = outer.AllocateArrayForOverwrite<std::uint32_t>(64);
    Fill(kept, 100);
    void* innerFirst = nullptr;
    {
        ScratchScope inner;
        innerFirst = inner.Allocate(256);
        std::span<std::uint32_t> scratch = inner.AllocateArrayForOverwrite<std::uint32_t>(256);
        Fill(scratch, 7);
    }
    EXPECT_TRUE(Holds(kept, 100));
    // The inner scope's memory came back at its close: the next allocation reuses its first byte.
    EXPECT_EQ(outer.Allocate(256), innerFirst);
}

// A body that opens a scope and waits on its forks runs those forks inside its Wait on a
// one-worker pool; their scopes open and close above the body's, which survives them.
TEST(ScratchScopeTests, AScopeOpenAcrossAParticipatingWaitSurvivesTheInnerUnitsScopes)
{
    WorkStealingThreadPool pool(1);
    std::atomic<int> unitsOnTheBodysThread{0};
    bool survived = false;
    JobCounter gate;
    pool.Run(
        [&pool, &unitsOnTheBodysThread, &survived] {
            const std::thread::id body = std::this_thread::get_id();
            ScratchScope scratch;
            std::span<std::uint32_t> kept = scratch.AllocateArrayForOverwrite<std::uint32_t>(1024);
            Fill(kept, 3);
            JobCounter units;
            for (int unit = 0; unit < 8; ++unit)
                pool.Run(
                    [body, &unitsOnTheBodysThread] {
                        ScratchScope unitScratch;
                        Fill(unitScratch.AllocateArrayForOverwrite<std::uint32_t>(4096), 0xA000u);
                        if (std::this_thread::get_id() == body)
                            unitsOnTheBodysThread.fetch_add(1, std::memory_order_relaxed);
                    },
                    units);
            pool.Wait(units);
            survived = Holds(kept, 3) && scratch.Allocate(16) != nullptr;
        },
        gate);
    pool.Wait(gate);
    EXPECT_EQ(unitsOnTheBodysThread.load(), 8);
    EXPECT_TRUE(survived);
}

TEST(ScratchScopeTests, InlinePoolNestsScopesToDepth64)
{
    WorkStealingThreadPool pool(0);
    ASSERT_TRUE(pool.IsInlineMode());
    int reached = 0;
    NestInlineUnits(pool, 1, 64, reached);
    EXPECT_EQ(reached, 64);
}

TEST(ScratchScopeTests, ScopesNestPastOneStorageChunk)
{
    int reached = 0;
    NestTo(1, 200, reached);
    EXPECT_EQ(reached, 200);
}

// An allocation through the outer scope while the inner one is open, raw and by a container's
// growth: correct after the inner scope closes, and counted.
TEST(ScratchScopeTests, OutOfOrderGrowthIsCorrectAndCounted)
{
    const std::optional<StatsEntry> before = FindThisThreadsScratch();
    ScratchScope outer;
    std::pmr::vector<std::uint32_t> grown{&outer.Resource()};
    std::span<std::uint32_t> raw;
    {
        ScratchScope inner;
        Fill(inner.AllocateArrayForOverwrite<std::uint32_t>(32), 1);
        raw = {static_cast<std::uint32_t*>(outer.Allocate(256 * sizeof(std::uint32_t), alignof(std::uint32_t))), 256};
        Fill(raw, 50);
        grown.reserve(512);
        grown.resize(512);
        Fill(grown, 900);
        Fill(inner.AllocateArrayForOverwrite<std::uint32_t>(1024), 2);
    }
    EXPECT_TRUE(Holds(raw, 50));
    EXPECT_TRUE(Holds(grown, 900));
    const std::optional<StatsEntry> after = FindThisThreadsScratch();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->OutOfOrder - (before ? before->OutOfOrder : 0), 2u);
}

// A container on an outer scope's resource stays correct however many scopes open after it:
// growth inside the 256th inner scope, and growth after 256 inner scopes have closed.
TEST(ScratchScopeTests, OuterContainerGrowsInsideThe256thInnerScope)
{
    ScratchScope outer;
    std::pmr::vector<std::uint32_t> grown{&outer.Resource()};
    for (int i = 0; i < 255; ++i)
    {
        ScratchScope inner;
        inner.Allocate(16);
    }
    {
        ScratchScope inner;
        grown.resize(1024);
        Fill(grown, 40);
    }
    {
        ScratchScope reuse;
        Fill(reuse.AllocateArrayForOverwrite<std::uint32_t>(4096), 0xBEEF0000u);
    }
    EXPECT_TRUE(Holds(grown, 40));
}

TEST(ScratchScopeTests, OuterContainerGrowsAfter256InnerScopes)
{
    ScratchScope outer;
    std::pmr::vector<std::uint32_t> grown{&outer.Resource()};
    for (int i = 0; i < 256; ++i)
    {
        ScratchScope inner;
        inner.Allocate(16);
    }
    grown.resize(1024);
    Fill(grown, 41);
    EXPECT_TRUE(Holds(grown, 41));
}

// With more than 256 scopes open at once, a scope past the ring takes its frame's own resource,
// never an open scope's: a container on the outermost scope grown 300 levels down stays its own.
TEST(ScratchScopeTests, OuterContainerGrowsWith300ScopesOpen)
{
    ScratchScope outer;
    std::pmr::vector<std::uint32_t> grown{&outer.Resource()};
    GrowAtDepth(1, 300, grown);
    {
        ScratchScope reuse;
        Fill(reuse.AllocateArrayForOverwrite<std::uint32_t>(4096), 0xBEEF0000u);
    }
    EXPECT_TRUE(Holds(grown, 42));
}

// A thread_local object constructed before the thread's first scope is destroyed after the
// scratch state's exit sentinel; a scope its destructor opens finds the state gone.
namespace
{
struct LateScopeResult
{
    std::atomic<bool> Ran{false};
    std::atomic<bool> Correct{false};
    std::atomic<std::uint64_t> HeapAllocations{0};
};

struct OpensAScopeAtThreadExit
{
    LateScopeResult* Result = nullptr;
    ~OpensAScopeAtThreadExit()
    {
        if (Result == nullptr)
            return;
        AllocationCountScope count(CountWindow::ThisThread);
        bool correct = false;
        {
            ScratchScope scratch;
            std::span<std::uint32_t> values = scratch.AllocateArrayForOverwrite<std::uint32_t>(256);
            Fill(values, 11);
            std::pmr::vector<std::uint32_t> grown{&scratch.Resource()};
            grown.assign(values.begin(), values.end());
            correct = Holds(values, 11) && Holds(grown, 11) &&
                      GameEngine::Memory::Detail::PeekScratchThread() == nullptr;
        }
        Result->HeapAllocations.store(count.Count(), std::memory_order_relaxed);
        Result->Correct.store(correct, std::memory_order_relaxed);
        Result->Ran.store(true, std::memory_order_release);
    }
};

thread_local OpensAScopeAtThreadExit t_OpensAScopeAtThreadExit;
} // namespace

TEST(ScratchScopeTests, ScopeOpenedAfterThreadExitServesFromTheHeap)
{
    LateScopeResult result;
    std::thread thread([&result] {
        t_OpensAScopeAtThreadExit.Result = &result;
        ScratchScope first;
        first.Allocate(64);
    });
    thread.join();
    ASSERT_TRUE(result.Ran.load(std::memory_order_acquire));
    EXPECT_TRUE(result.Correct.load());
    // The heap frame, the array and the vector's storage.
    EXPECT_EQ(result.HeapAllocations.load(), 3u);
}

// Outermost scopes that each use more than 4 MiB keep their block: after warm-up, 2048 closes
// allocate nothing and release nothing.
TEST(ScratchScopeTests, SteadyUseAboveTheOldCapNeverShrinks)
{
    constexpr int kWarmupCloses = 4;
    constexpr int kMeasuredCloses = 2048;
    constexpr std::size_t kPieces = 9;
    constexpr std::size_t kPieceBytes = 512 * 1024;
    std::uint64_t allocations = 0;
    std::optional<StatsEntry> scratch;
    std::thread thread([&allocations, &scratch] {
        const auto useOnce = [] {
            ScratchScope outermost;
            for (std::size_t piece = 0; piece < kPieces; ++piece)
                static_cast<unsigned char*>(outermost.Allocate(kPieceBytes))[0] = 1;
        };
        for (int close = 0; close < kWarmupCloses; ++close)
            useOnce();
        AllocationCountScope count(CountWindow::ThisThread);
        for (int close = 0; close < kMeasuredCloses; ++close)
            useOnce();
        allocations = count.Count();
        scratch = FindThisThreadsScratch();
    });
    thread.join();
    EXPECT_EQ(allocations, 0u);
    ASSERT_TRUE(scratch.has_value());
    EXPECT_EQ(scratch->Shrinks, 0u);
    EXPECT_GE(scratch->ReservedBytes, kPieces * kPieceBytes);
}

// One outermost scope's spike is held through the hysteresis window, then released.
TEST(ScratchScopeTests, ASpikeIsReleasedAfterTheTrimWindow)
{
    std::optional<StatsEntry> held;
    std::optional<StatsEntry> released;
    std::thread thread([&held, &released] {
        {
            ScratchScope spike;
            spike.Allocate(8 * 1024 * 1024);
        }
        const auto smallClose = [] {
            ScratchScope outermost;
            outermost.Allocate(1024);
        };
        for (int close = 0; close < 1500; ++close)
            smallClose();
        held = FindThisThreadsScratch();
        for (int close = 0; close < 700; ++close)
            smallClose();
        released = FindThisThreadsScratch();
    });
    thread.join();
    ASSERT_TRUE(held.has_value() && released.has_value());
    EXPECT_EQ(held->Shrinks, 0u);
    EXPECT_GE(held->ReservedBytes, 8u * 1024 * 1024);
    EXPECT_EQ(released->Shrinks, 1u);
    EXPECT_EQ(released->ReservedBytes, GameEngine::Memory::kArenaFirstBlockBytes);
    EXPECT_GE(released->HighWaterBytes, 8u * 1024 * 1024);
}

// A peak held only by a nested scope survives that scope's close: the thread's high water and
// its retained block take it, and the next bookkeeping close keeps the block.
TEST(ScratchScopeTests, ANestedScopesPeakSurvivesItsClose)
{
    constexpr std::size_t kNestedBytes = 8 * 1024 * 1024;
    std::optional<StatsEntry> scratch;
    std::thread thread([&scratch] {
        {
            ScratchScope outermost;
            outermost.Allocate(1024);
            ScratchScope nested;
            nested.Allocate(kNestedBytes);
        }
        for (int close = 0; close < 64; ++close)
        {
            ScratchScope outermost;
            outermost.Allocate(1024);
        }
        scratch = FindThisThreadsScratch();
    });
    thread.join();
    ASSERT_TRUE(scratch.has_value());
    EXPECT_GE(scratch->HighWaterBytes, kNestedBytes);
    EXPECT_GE(scratch->ReservedBytes, kNestedBytes);
    EXPECT_EQ(scratch->Shrinks, 0u);
}

// A nested peak that stays inside the first block, whose close is the in-block rewind, still
// reaches the thread's high water at the next bookkeeping close.
TEST(ScratchScopeTests, ANestedPeakInsideTheFirstBlockReachesTheHighWater)
{
    constexpr std::size_t kNestedBytes = 32 * 1024;
    std::optional<StatsEntry> scratch;
    std::thread thread([&scratch] {
        {
            ScratchScope outermost;
            outermost.Allocate(1024);
            ScratchScope nested;
            nested.Allocate(kNestedBytes);
        }
        for (int close = 0; close < 64; ++close)
        {
            ScratchScope outermost;
            outermost.Allocate(1024);
        }
        scratch = FindThisThreadsScratch();
    });
    thread.join();
    ASSERT_TRUE(scratch.has_value());
    EXPECT_GE(scratch->HighWaterBytes, kNestedBytes);
}

#if GE_DEBUG_INSTRUMENTATION && !defined(NDEBUG)
TEST(ScratchScopeDeathTest, AllocationFromAnotherThreadAsserts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            ScratchScope scratch;
            std::thread other([&scratch] { scratch.Allocate(16); });
            other.join();
        },
        "allocates only on the thread that opened it");
}

TEST(ScratchScopeDeathTest, ContainerOutlivingItsScopeAssertsOnGrowth)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            std::optional<std::pmr::vector<int>> outlived;
            {
                ScratchScope scratch;
                outlived.emplace(&scratch.Resource());
                outlived->push_back(1);
            }
            outlived->reserve(1000);
        },
        "a container outlived its ScratchScope");
}
#endif

#if defined(GE_MEMORY_ASAN)
// A steady close takes the arena's inline fast path (one block, not a 64th outermost close);
// it poisons what it takes back like the full Reset and Rewind.
TEST(ScratchScopeDeathTest, ReadAfterAnOutermostCloseIsReportedUnderAsan)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            volatile int* value = nullptr;
            {
                ScratchScope scratch;
                value = scratch.New<int>(5);
            }
            [[maybe_unused]] const int read = *value;
        },
        "READ of size");
}

TEST(ScratchScopeDeathTest, ReadAfterANestedCloseIsReportedUnderAsan)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            ScratchScope outer;
            ASSERT_NE(outer.New<int>(1), nullptr);
            volatile int* value = nullptr;
            {
                ScratchScope inner;
                value = inner.New<int>(5);
            }
            [[maybe_unused]] const int read = *value;
        },
        "READ of size");
}
#endif
