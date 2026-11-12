// Each unit of a Run or ParallelFor fork, and each admission hook on a helper, runs with the frame-joined bit of
// the thread that published the fork; every queued body runs with it clear; a unit that throws gives its thread back
// its own bit. A test thread inside FrameJoinedScope{true} stands in for the frame thread in its frame.

#include "HeldWorkers.h"

#include "JobSystem/JobChannel.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Memory/FrameJoinedScope.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <thread>

#if GE_DEBUG_INSTRUMENTATION

using namespace JobSystem;

namespace GameEngine::Tests
{

namespace
{

using Memory::Detail::FrameJoinedScope;
using Memory::Detail::IsFrameJoined;

constexpr auto kTimeout = std::chrono::seconds(10);

// What the bodies of one fork read: how many ran, how many ran frame-joined,
// how many ran off the thread that built this.
struct BodyReadings
{
    std::atomic<uint32_t> Ran{0};
    std::atomic<uint32_t> Joined{0};
    std::atomic<uint32_t> OffCaller{0};
    std::thread::id Caller = std::this_thread::get_id();

    void Record()
    {
        if (IsFrameJoined())
            Joined.fetch_add(1);
        if (std::this_thread::get_id() != Caller)
            OffCaller.fetch_add(1);
        Ran.fetch_add(1);
    }

    // Records, then holds a unit on the forking thread until a helper has
    // recorded one, so the run has units on both sides.
    void RecordBesideAHelper()
    {
        Record();
        if (std::this_thread::get_id() == Caller)
        {
            EXPECT_TRUE(WaitUntil([this] { return OffCaller.load() > 0; }, kTimeout)) << "no helper ran a unit";
        }
    }
};

void ExpectEveryUnitJoinedAndOneOffTheCaller(const BodyReadings& readings, uint32_t units)
{
    EXPECT_EQ(readings.Ran.load(), units);
    EXPECT_EQ(readings.Joined.load(), units) << "a unit of a frame-joined fork ran without the bit";
    EXPECT_GT(readings.OffCaller.load(), 0u);
}

// The units of a two-unit ParallelFor whose helper unit throws. Each side
// runs one unit, whichever claims first: the caller's unit returns only once
// the helper's has thrown, and the helper's unit throws only once the
// caller's has recorded, so the throw never closes the run before the caller
// has claimed its unit.
struct HelperThrowsAfterTheCallerRecords
{
    BodyReadings Readings;
    std::atomic<bool> CallerRecorded{false};
    std::atomic<bool> HelperThrew{false};

    void RunUnit()
    {
        Readings.Record();
        if (std::this_thread::get_id() == Readings.Caller)
        {
            CallerRecorded.store(true);
            EXPECT_TRUE(WaitUntil([this] { return HelperThrew.load(); }, kTimeout)) << "the helper never threw";
            return;
        }
        EXPECT_TRUE(WaitUntil([this] { return CallerRecorded.load(); }, kTimeout)) << "the caller ran no unit";
        HelperThrew.store(true);
        throw std::runtime_error("helper unit failed");
    }
};

// Publishes one body through every queued path: Submit (both lanes, and with a
// dependency), EnqueueWork, EnqueueWorkBatch, JobChannel::Submit and
// JobChannel::Enqueue. Returns how many bodies it published.
uint32_t PublishEveryQueuedBody(WorkStealingThreadPool& pool, JobChannel& channel, BodyReadings& readings)
{
    const auto record = [&readings] { readings.Record(); };
    TaskHandle normal = pool.Submit(record);
    pool.Submit(record, JobPriority::Background).Wait();
    const std::array<TaskHandle, 1> dependencies{normal};
    pool.Submit(record, std::span<const TaskHandle>(dependencies)).Wait();
    normal.Wait();
    pool.EnqueueWork(record, JobPriority::Background);
    std::array batch{record, record};
    pool.EnqueueWorkBatch(batch.data(), batch.size());
    channel.Submit(record).Wait();
    channel.Enqueue(record);
    return 8;
}

} // namespace

TEST(FrameJoinedCarrier, ARunUnitOfAFrameJoinedForkSeesTheBitOnAWorker)
{
    // Declared before the pool: a job can still use them when an ASSERT returns early.
    BodyReadings readings;
    JobCounter counter;
    WorkStealingThreadPool pool(2);
    constexpr uint32_t kUnits = 8;
    {
        const FrameJoinedScope frame(true);
        for (uint32_t i = 0; i < kUnits; ++i)
            pool.Run([&readings] { readings.Record(); }, counter);
        // The workers' visits run every closure before this thread joins, so
        // none runs on the publishing thread.
        ASSERT_TRUE(WaitUntil([&readings] { return readings.Ran.load() == kUnits; }, kTimeout));
        pool.Wait(counter);
    }
    EXPECT_EQ(readings.OffCaller.load(), kUnits);
    EXPECT_EQ(readings.Joined.load(), kUnits);
}

TEST(FrameJoinedCarrier, ARunUnitRunByAWaiterCarriesItsPublishersBit)
{
    WorkStealingThreadPool pool(1);
    HeldWorkers held(pool);
    constexpr uint32_t kUnits = 4;

    BodyReadings joinedPublisher;
    JobCounter joinedCounter;
    {
        const FrameJoinedScope frame(true);
        for (uint32_t i = 0; i < kUnits; ++i)
            pool.Run([&joinedPublisher] { joinedPublisher.Record(); }, joinedCounter);
    }
    pool.Wait(joinedCounter);

    BodyReadings clearPublisher;
    JobCounter clearCounter;
    for (uint32_t i = 0; i < kUnits; ++i)
        pool.Run([&clearPublisher] { clearPublisher.Record(); }, clearCounter);
    {
        const FrameJoinedScope frame(true);
        pool.Wait(clearCounter);
    }
    held.Release();

    EXPECT_EQ(joinedPublisher.OffCaller.load(), 0u) << "the held worker ran a closure";
    EXPECT_EQ(joinedPublisher.Joined.load(), kUnits) << "a clear waiter ran a joined closure without the bit";
    EXPECT_EQ(clearPublisher.OffCaller.load(), 0u) << "the held worker ran a closure";
    EXPECT_EQ(clearPublisher.Joined.load(), 0u) << "a joined waiter lent its bit to a clear closure";
}

TEST(FrameJoinedCarrier, EveryParallelForUnitSeesTheBit)
{
    WorkStealingThreadPool pool(4);
    BodyReadings readings;
    constexpr uint32_t kUnits = 64;
    {
        const FrameJoinedScope frame(true);
        ParallelFor(&pool, kUnits, [&readings](size_t) { readings.RecordBesideAHelper(); });
    }
    ExpectEveryUnitJoinedAndOneOffTheCaller(readings, kUnits);
}

TEST(FrameJoinedCarrier, EveryChunkOfTheChunkedOverloadSeesTheBit)
{
    WorkStealingThreadPool pool(4);
    BodyReadings readings;
    std::atomic<size_t> covered{0};
    constexpr size_t kElements = 64;
    {
        const FrameJoinedScope frame(true);
        ParallelFor(&pool, kElements,
                    [&readings, &covered](size_t begin, size_t end) {
                        covered.fetch_add(end - begin);
                        readings.RecordBesideAHelper();
                    },
                    1);
    }
    EXPECT_EQ(covered.load(), kElements);
    ExpectEveryUnitJoinedAndOneOffTheCaller(readings, readings.Ran.load());
}

TEST(FrameJoinedCarrier, UnitsOfAJoinedForkWithBackgroundHelpersSeeTheBit)
{
    WorkStealingThreadPool pool(4);
    BodyReadings readings;
    constexpr uint32_t kUnits = 64;
    ParallelForOptions options;
    options.HelperPriority = JobPriority::Background;
    {
        const FrameJoinedScope frame(true);
        ParallelFor(&pool, kUnits, [&readings](size_t) { readings.RecordBesideAHelper(); }, options);
    }
    ExpectEveryUnitJoinedAndOneOffTheCaller(readings, kUnits);
}

TEST(FrameJoinedCarrier, AdmissionHooksOnAHelperSeeTheBit)
{
    WorkStealingThreadPool pool(2);
    BodyReadings units;
    BodyReadings hooks;
    constexpr uint32_t kUnits = 16;
    ParallelForOptions::UnitAdmission admission{[&hooks] {
                                                    hooks.Record();
                                                    return true;
                                                },
                                                [&hooks] { hooks.Record(); }};
    ParallelForOptions options;
    options.Admission = &admission;
    {
        const FrameJoinedScope frame(true);
        ParallelFor(&pool, kUnits, [&units](size_t) { units.RecordBesideAHelper(); }, options);
    }
    ExpectEveryUnitJoinedAndOneOffTheCaller(units, kUnits);
    EXPECT_GT(hooks.OffCaller.load(), 0u) << "no helper ran an admission hook";
    EXPECT_EQ(hooks.Joined.load(), hooks.Ran.load()) << "an admission hook on a helper ran without the bit";
}

TEST(FrameJoinedCarrier, QueuedBodiesRunClearOnAnInlinePool)
{
    WorkStealingThreadPool pool(0);
    JobChannel channel(pool, {.Name = "Frame-joined inline", .MaxRunning = 1});
    BodyReadings readings;
    const FrameJoinedScope frame(true);
    const uint32_t published = PublishEveryQueuedBody(pool, channel, readings);
    EXPECT_EQ(readings.Ran.load(), published) << "an inline pool runs every body at publish";
    EXPECT_EQ(readings.Joined.load(), 0u) << "a queued body ran with its publisher's bit";
    EXPECT_TRUE(IsFrameJoined()) << "a queued body did not give the publishing thread its bit back";
}

TEST(FrameJoinedCarrier, QueuedBodiesRunClearOnWorkersAndBlockingThreads)
{
    // Declared before the pool: a job can still use it when an ASSERT returns early.
    BodyReadings readings;
    WorkStealingThreadPool pool(2);
    JobChannel channel(pool, {.Name = "Frame-joined threaded", .MaxRunning = 1});
    uint32_t published = 0;
    {
        const FrameJoinedScope frame(true);
        published = PublishEveryQueuedBody(pool, channel, readings);
    }
    ASSERT_TRUE(WaitUntil([&readings, published] { return readings.Ran.load() == published; }, kTimeout));
    EXPECT_EQ(readings.Joined.load(), 0u);
}

// After Shutdown, EnqueueWorkBatch runs each body on the publishing thread
// itself, and the body still runs clear there.
TEST(FrameJoinedCarrier, BatchBodiesAfterShutdownRunClear)
{
    WorkStealingThreadPool pool(2);
    pool.Shutdown();
    BodyReadings readings;
    const FrameJoinedScope frame(true);
    const auto record = [&readings] { readings.Record(); };
    std::array batch{record, record};
    pool.EnqueueWorkBatch(batch.data(), batch.size());
    EXPECT_EQ(readings.Ran.load(), 2u);
    EXPECT_EQ(readings.Joined.load(), 0u) << "a batch body after Shutdown ran with its publisher's bit";
    EXPECT_TRUE(IsFrameJoined()) << "a batch body did not give the publishing thread its bit back";
}

TEST(FrameJoinedCarrier, UnitsOfAForkInsideASubmitBodyRunClear)
{
    WorkStealingThreadPool pool(4);
    BodyReadings outer;
    BodyReadings inner;
    const FrameJoinedScope frame(true);
    pool.Submit([&pool, &outer, &inner] {
            ParallelFor(&pool, 8, [&pool, &outer, &inner](size_t) {
                outer.Record();
                JobCounter counter;
                for (int i = 0; i < 4; ++i)
                    pool.Run([&inner] { inner.Record(); }, counter);
                pool.Wait(counter);
            });
        })
        .Wait();
    EXPECT_EQ(outer.Ran.load(), 8u);
    EXPECT_EQ(inner.Ran.load(), 32u);
    EXPECT_EQ(outer.Joined.load(), 0u);
    EXPECT_EQ(inner.Joined.load(), 0u);
}

// A non-RAII restore inside the closure's own catch would keep the thrower's
// bit on the waiter that ran it, for the rest of the waiter's body.
TEST(FrameJoinedCarrier, AWaiterThatRunsAThrowingJoinedUnitGetsItsOwnBitBack)
{
    // Declared before the pool: the held unit can still use them when an ASSERT returns early.
    BodyReadings readings;
    JobCounter counter;
    WorkStealingThreadPool pool(1);
    HeldWorkers held(pool);
    {
        const FrameJoinedScope frame(true);
        pool.Run([&readings] {
                readings.Record();
                throw std::runtime_error("joined unit failed");
            },
            counter);
    }
    ASSERT_FALSE(IsFrameJoined());
    pool.Wait(counter);
    EXPECT_FALSE(IsFrameJoined()) << "the throwing unit's bit stayed on the waiter that ran it";
    held.Release();

    EXPECT_TRUE(counter.HasAnyFailed());
    EXPECT_EQ(readings.OffCaller.load(), 0u) << "the held worker ran the closure";
    EXPECT_EQ(readings.Joined.load(), 1u);
}

TEST(FrameJoinedCarrier, AThrowingParallelForUnitLeavesTheCallerJoinedAndTheWorkerClear)
{
    WorkStealingThreadPool pool(1);
    HelperThrowsAfterTheCallerRecords units;
    ParallelForOptions options;
    options.Helpers = 1;
    const FrameJoinedScope frame(true);
    EXPECT_THROW(ParallelFor(&pool, 2, [&units](size_t) { units.RunUnit(); }, options), std::runtime_error);
    EXPECT_TRUE(IsFrameJoined()) << "the rethrow took the caller's bit";
    EXPECT_EQ(units.Readings.OffCaller.load(), 1u);
    EXPECT_EQ(units.Readings.Joined.load(), 2u);

    bool workerJoined = true;
    pool.Submit([&workerJoined] { workerJoined = IsFrameJoined(); }).Wait();
    EXPECT_FALSE(workerJoined) << "the helper's throw left the bit on its worker";
}

TEST(FrameJoinedCarrier, NestedForksInheritTheBit)
{
    WorkStealingThreadPool pool(4);
    BodyReadings outer;
    BodyReadings innerRun;
    BodyReadings innerParallelFor;
    constexpr uint32_t kOuterUnits = 8;
    {
        const FrameJoinedScope frame(true);
        ParallelFor(&pool, kOuterUnits, [&pool, &outer, &innerRun, &innerParallelFor](size_t) {
            outer.RecordBesideAHelper();
            JobCounter counter;
            for (int i = 0; i < 4; ++i)
                pool.Run([&innerRun] { innerRun.Record(); }, counter);
            pool.Wait(counter);
            ParallelFor(&pool, 8, [&innerParallelFor](size_t) { innerParallelFor.Record(); });
        });
    }
    ExpectEveryUnitJoinedAndOneOffTheCaller(outer, kOuterUnits);
    EXPECT_EQ(innerRun.Ran.load(), kOuterUnits * 4);
    EXPECT_EQ(innerRun.Joined.load(), kOuterUnits * 4);
    EXPECT_EQ(innerParallelFor.Ran.load(), kOuterUnits * 8);
    EXPECT_EQ(innerParallelFor.Joined.load(), kOuterUnits * 8);
}

// The frame thread holds the scope for its frame; a fork it makes outside the
// frame (between ticks) runs clear.
TEST(FrameJoinedCarrier, AFrameThreadsForksCarryTheBitOnlyInsideTheFrame)
{
    WorkStealingThreadPool pool(2);
    BodyReadings insideFrame;
    BodyReadings betweenFrames;
    constexpr uint32_t kUnits = 16;
    {
        const FrameJoinedScope frame(true);
        EXPECT_TRUE(IsFrameJoined());
        ParallelFor(&pool, kUnits, [&insideFrame](size_t) { insideFrame.RecordBesideAHelper(); });
        JobCounter counter;
        for (uint32_t i = 0; i < kUnits; ++i)
            pool.Run([&insideFrame] { insideFrame.Record(); }, counter);
        pool.Wait(counter);
    }
    EXPECT_FALSE(IsFrameJoined());
    // An unstarted queue visit of a Run holds one count of the fork budget, and
    // a fork gets no helper while no count is free. Wait until the workers run
    // every queued task, so the next fork gets its helpers.
    ASSERT_TRUE(WaitUntil(
        [&pool] { return pool.GetPendingTasksApprox() == 0 && pool.GetApproximateQueueSize() == 0; }, kTimeout))
        << "the queued tasks did not run: pending=" << pool.GetPendingTasksApprox()
        << " queued=" << pool.GetApproximateQueueSize();
    ParallelFor(&pool, kUnits, [&betweenFrames](size_t) { betweenFrames.RecordBesideAHelper(); });

    EXPECT_EQ(insideFrame.Ran.load(), 2 * kUnits);
    EXPECT_EQ(insideFrame.Joined.load(), 2 * kUnits);
    EXPECT_GT(insideFrame.OffCaller.load(), 0u);
    EXPECT_EQ(betweenFrames.Ran.load(), kUnits);
    EXPECT_EQ(betweenFrames.Joined.load(), 0u);
}

} // namespace GameEngine::Tests

#endif
