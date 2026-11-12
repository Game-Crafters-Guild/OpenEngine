// Memory::FrameMemory: a slot's two-frame lifetime, allocations on joined workers and a frame
// container grown there, units on a helping thread, retention on a worker, and the Debug
// refusals: bodies that may outlive the frame, no frame loop, thread exit, and a throwing unit.

#include "Memory/Arena.h"
#include "Memory/FrameJoinedScope.h"
#include "Memory/FrameMemory.h"
#include "Memory/StatsSnapshot.h"
#include "Memory/Testing/SimulatedFrame.h"

#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
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
using GameEngine::Memory::FrameMemory;
using GameEngine::Memory::StatsEntry;
using GameEngine::Memory::StatsKind;
using GameEngine::Memory::Testing::SimulatedFrame;
using JobSystem::JobCounter;
using JobSystem::TaskHandle;
using JobSystem::WorkStealingThreadPool;
#if GE_DEBUG_INSTRUMENTATION
using GameEngine::Memory::Detail::FrameJoinedScope;
using GameEngine::Memory::Detail::IsFrameJoined;
#endif

/// The id the frame memory stats records name this thread by.
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

/// The frame arena of `thread` that has held at least `highWater` bytes, if it exists.
std::optional<StatsEntry> FindFrameArena(std::uint64_t thread, std::size_t highWater)
{
    for (const StatsEntry& entry : GameEngine::Memory::SnapshotStats())
        if (entry.Kind == StatsKind::FrameMemory && entry.ThreadId == thread && entry.HighWaterBytes >= highWater)
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

void YieldUntil(const std::atomic<bool>& flag)
{
    while (!flag.load(std::memory_order_acquire))
        std::this_thread::yield();
}

/// Runs `unit` as a `Run` unit of the calling thread and waits for it without taking part, so a
/// worker runs it. Returns whether it ran off the calling thread.
template <class Unit>
bool RunOnAWorker(WorkStealingThreadPool& pool, Unit&& unit)
{
    const std::thread::id caller = std::this_thread::get_id();
    bool offCaller = false;
    JobCounter counter;
    pool.Run(
        [&offCaller, &unit, caller] {
            offCaller = std::this_thread::get_id() != caller;
            unit();
        },
        counter);
    while (!counter.IsZero())
        std::this_thread::yield();
    pool.Wait(counter);
    return offCaller;
}

TEST(FrameMemoryTests, ASlotRewindsOnlyWhenItsStampIsTwoFramesOld)
{
    std::span<std::uint32_t> fromFrameF;
    {
        SimulatedFrame frame;
        fromFrameF = FrameMemory::AllocateArray<std::uint32_t>(256);
        Fill(fromFrameF, 100);
    }
    {
        SimulatedFrame frame;
        std::span<std::uint32_t> fromFramePlusOne = FrameMemory::AllocateArray<std::uint32_t>(256);
        Fill(fromFramePlusOne, 900);
        EXPECT_TRUE(Holds(fromFrameF, 100)) << "frame F+1 reused frame F's memory";
    }
    {
        SimulatedFrame frame;
        std::span<std::uint32_t> fromFramePlusTwo = FrameMemory::AllocateArray<std::uint32_t>(256);
        EXPECT_EQ(fromFramePlusTwo.data(), fromFrameF.data()) << "frame F+2 did not rewind frame F's arena";
    }
}

TEST(FrameMemoryTests, AWorkerAllocationFromFrameFIsReadableInFramePlusOne)
{
    WorkStealingThreadPool pool(1);
    std::span<std::uint32_t> fromFrameF;
    {
        SimulatedFrame frame;
        ASSERT_TRUE(RunOnAWorker(pool, [&fromFrameF] {
            fromFrameF = FrameMemory::AllocateArray<std::uint32_t>(256);
            Fill(fromFrameF, 100);
        }));
    }
    SimulatedFrame frame;
    std::span<std::uint32_t> fromFramePlusOne;
    ASSERT_TRUE(RunOnAWorker(pool, [&fromFramePlusOne] {
        fromFramePlusOne = FrameMemory::AllocateArray<std::uint32_t>(256);
        Fill(fromFramePlusOne, 900);
    }));
    EXPECT_NE(fromFramePlusOne.data(), fromFrameF.data());
    EXPECT_TRUE(Holds(fromFrameF, 100)) << "the worker reused frame F's memory in frame F+1";
}

// Two probes on the frame thread around the worker's growth are adjacent only when the growth
// took nothing from the frame thread's arena.
TEST(FrameMemoryTests, AFrameVectorGrownOnAJoinedWorkerUsesTheWorkersArena)
{
    WorkStealingThreadPool pool(1);
    SimulatedFrame frame;
    std::byte* const before = static_cast<std::byte*>(FrameMemory::Allocate(16, 16));
    std::pmr::vector<std::uint32_t> values{&FrameMemory::Resource()};
    ASSERT_TRUE(RunOnAWorker(pool, [&values] {
        for (std::uint32_t i = 0; i < 1000; ++i)
            values.push_back(i);
    }));
    std::byte* const after = static_cast<std::byte*>(FrameMemory::Allocate(16, 16));
    EXPECT_EQ(after, before + 16) << "the worker's growth took memory from the frame thread's arena";
    ASSERT_EQ(values.size(), 1000u);
    EXPECT_EQ(values.front(), 0u);
    EXPECT_EQ(values.back(), 999u);
}

// The joined unit runs on the only worker, inside a Submit body that waits for its counter.
TEST(FrameMemoryTests, AJoinedUnitOnAHelpingThreadIsAllowed)
{
    // Declared before the pool: the held job can still read it after the release.
    std::atomic<bool> published{false};
    WorkStealingThreadPool pool(1);
    SimulatedFrame frame;
    std::atomic<bool> started{false};
    std::thread::id helper;
    std::thread::id ranOn;
    [[maybe_unused]] bool helperJoinedAfter = true;
    std::span<std::uint32_t> values;
    JobCounter joined;
    TaskHandle body = pool.Submit([&pool, &helper, &started, &published, &joined, &helperJoinedAfter] {
        helper = std::this_thread::get_id();
        started.store(true, std::memory_order_release);
        YieldUntil(published);
        pool.Wait(joined);
#if GE_DEBUG_INSTRUMENTATION
        helperJoinedAfter = IsFrameJoined();
#endif
    });
    YieldUntil(started);
    pool.Run(
        [&ranOn, &values] {
            ranOn = std::this_thread::get_id();
            values = FrameMemory::AllocateArray<std::uint32_t>(64);
            Fill(values, 7);
        },
        joined);
    published.store(true, std::memory_order_release);
    while (!body.IsDone())
        std::this_thread::yield();
    body.Wait();
    EXPECT_EQ(ranOn, helper) << "the waiting Submit body did not run the joined unit";
    EXPECT_TRUE(Holds(values, 7));
#if GE_DEBUG_INSTRUMENTATION
    EXPECT_FALSE(helperJoinedAfter) << "the joined unit's bit stayed on the Submit body";
#endif
}

#if GE_DEBUG_INSTRUMENTATION
TEST(FrameMemoryTests, SimulatedFrameSetsTheBitForForkedUnits)
{
    WorkStealingThreadPool pool(4);
    std::atomic<std::uint32_t> joined{0};
    const auto unit = [&joined] {
        if (IsFrameJoined())
            joined.fetch_add(1, std::memory_order_relaxed);
        Fill(FrameMemory::AllocateArray<std::uint32_t>(16), 1);
    };
    {
        SimulatedFrame frame;
        EXPECT_TRUE(IsFrameJoined());
        JobCounter counter;
        for (int i = 0; i < 16; ++i)
            pool.Run(unit, counter);
        pool.Wait(counter);
        JobSystem::ParallelFor(&pool, 16, [&unit](std::size_t) { unit(); }, JobSystem::ParallelForOptions{});
    }
    EXPECT_FALSE(IsFrameJoined());
    EXPECT_EQ(joined.load(), 32u);
}
#endif

// A slot rewinds every other frame, and a block is released one to two windows of 1024 rewinds
// after the spike that grew it.
TEST(FrameMemoryTests, ASpikeOnAWorkerIsReturnedAfterTheWindow)
{
    WorkStealingThreadPool pool(1);
    std::uint64_t worker = 0;
    const auto frameOnTheWorker = [&pool, &worker](std::size_t bytes) {
        SimulatedFrame frame;
        return RunOnAWorker(pool, [&worker, bytes] {
            worker = ThisOsThreadId();
            FrameMemory::Allocate(bytes);
        });
    };
    constexpr std::size_t kSpike = 8 * 1024 * 1024;
    ASSERT_TRUE(frameOnTheWorker(kSpike));
    for (int frame = 0; frame < 2 * 1500; ++frame)
        frameOnTheWorker(1024);
    const std::optional<StatsEntry> held = FindFrameArena(worker, kSpike);
    for (int frame = 0; frame < 2 * 700; ++frame)
        frameOnTheWorker(1024);
    const std::optional<StatsEntry> released = FindFrameArena(worker, kSpike);
    ASSERT_TRUE(held.has_value() && released.has_value());
    EXPECT_EQ(held->Shrinks, 0u);
    EXPECT_GE(held->ReservedBytes, kSpike);
    EXPECT_EQ(released->Shrinks, 1u);
    EXPECT_EQ(released->ReservedBytes, GameEngine::Memory::kArenaFirstBlockBytes);
}

#if GE_DEBUG_INSTRUMENTATION && !defined(NDEBUG)
TEST(FrameMemoryDeathTest, ASubmitBodyIsRefused)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            WorkStealingThreadPool pool(1);
            SimulatedFrame frame;
            pool.Submit([] { FrameMemory::Allocate(16); }).Wait();
        },
        "frame memory is only for the frame thread and the forks it joins");
}

TEST(FrameMemoryDeathTest, AnEnqueueWorkBodyIsRefused)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            WorkStealingThreadPool pool(1);
            SimulatedFrame frame;
            std::atomic<bool> ran{false};
            pool.EnqueueWork([&ran] {
                FrameMemory::Allocate(16);
                ran.store(true, std::memory_order_release);
            });
            YieldUntil(ran);
        },
        "frame memory is only for the frame thread and the forks it joins");
}

TEST(FrameMemoryDeathTest, AUnitOfAForkPublishedByASubmitBodyIsRefused)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            WorkStealingThreadPool pool(2);
            SimulatedFrame frame;
            pool.Submit([&pool] {
                    JobCounter counter;
                    pool.Run([] { FrameMemory::Allocate(16); }, counter);
                    pool.Wait(counter);
                })
                .Wait();
        },
        "frame memory is only for the frame thread and the forks it joins");
}

// The child of a threadsafe death test runs only this test, so no frame has started there.
TEST(FrameMemoryDeathTest, NoFrameLoopAssertsWithTheFix)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            const FrameJoinedScope joined(true);
            FrameMemory::Allocate(16);
        },
        "frame memory needs a running frame loop; use a ScratchScope or an Arena you own");
}

// The thread allocates in frame F and exits in frame F+1, while that memory is still valid.
TEST(FrameMemoryDeathTest, ThreadExitWithLiveFrameMemoryAsserts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            std::atomic<bool> allocated{false};
            std::atomic<bool> nextFrame{false};
            std::thread thread;
            {
                SimulatedFrame frame;
                thread = std::thread([&allocated, &nextFrame] {
                    const FrameJoinedScope joined(true);
                    FrameMemory::Allocate(16);
                    allocated.store(true, std::memory_order_release);
                    YieldUntil(nextFrame);
                });
                YieldUntil(allocated);
            }
            SimulatedFrame next;
            nextFrame.store(true, std::memory_order_release);
            thread.join();
        },
        "a thread exited holding frame memory");
}

// Every Submit body starts clear, so a throwing unit's bit can stay only in the body that ran
// it: this Submit body waits for a joined counter, runs its throwing unit, then allocates.
TEST(FrameMemoryDeathTest, AThrowingUnitRestoresTheWorkersFrameJoinedBit)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            // Declared before the pool: the held job can still read it after the release.
            std::atomic<bool> published{false};
            WorkStealingThreadPool pool(1);
            SimulatedFrame frame;
            std::atomic<bool> started{false};
            JobCounter joined;
            TaskHandle body = pool.Submit([&pool, &started, &published, &joined] {
                started.store(true, std::memory_order_release);
                YieldUntil(published);
                pool.Wait(joined);
                FrameMemory::Allocate(16);
            });
            YieldUntil(started);
            pool.Run([] { throw std::runtime_error("joined unit failed"); }, joined);
            published.store(true, std::memory_order_release);
            while (!body.IsDone())
                std::this_thread::yield();
            body.Wait();
        },
        "frame memory is only for the frame thread and the forks it joins");
}

// A frame that polls its Run counter in a later frame, instead of waiting it in its own frame.
TEST(FrameMemoryDeathTest, AJoinedRunStillOutstandingAtTheFrameAdvanceAsserts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            // Declared before the pool: the held job can still read it after the release.
            std::atomic<bool> release{false};
            WorkStealingThreadPool pool(1);
            JobCounter polled;
            {
                SimulatedFrame frame;
                pool.Run([&release] { YieldUntil(release); }, polled);
            }
            {
                SimulatedFrame next;
                release.store(true, std::memory_order_release);
                pool.Wait(polled);
            }
        },
        "a Run unit that frame code published is still outstanding at the frame advance");
}
#endif

} // namespace
