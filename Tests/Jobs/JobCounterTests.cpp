// Slice-5 JobCounter + participating-Wait coverage.
//
// Covers: zero-crossing memory ordering, exception-decrements, the
// stack-lifetime (UAF-pattern) churn, reuse across waves, filtered
// participation (workers execute ONLY same-counter tasks), nested
// counters, external-thread joins, Run-vs-Shutdown, and the slice-1-pattern
// strand detector across the new consumption path (500ms backstop disabled —
// a lost wakeup cannot hide inside the scheduler tail).

#include "JobSystem/JobCounter.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "JobSystem/TaskSlabPool.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Memory/AllocationCountScope.h"

#include "HeldWorkers.h"
#include "TestPlatform.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace GameEngine::Tests {

namespace {

using SteadyClock = std::chrono::steady_clock;

double ElapsedUs(SteadyClock::time_point t0, SteadyClock::time_point t1)
{
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

void BusyWaitUs(int us)
{
    if (us <= 0)
        return;
    const auto deadline = SteadyClock::now() + std::chrono::microseconds(us);
    while (SteadyClock::now() < deadline)
    {
        std::this_thread::yield();
    }
}

// Deadline for one parked-waiter handshake. A genuine lost wakeup pins the
// waiter forever; a saturated CI box merely stretches legitimate iterations
// (join latency is scheduling-bound, condvar-shaped — see the PR #342 wedge
// triage), so the deadline scales with GE_TEST_DEADLINE_SCALE (integer
// multiplier, default 1).
std::chrono::seconds ParkedWaiterDeadline()
{
    long scale = 1;
    if (const char* env = std::getenv("GE_TEST_DEADLINE_SCALE"))
    {
        scale = std::max(1L, std::strtol(env, nullptr, 10));
    }
    return std::chrono::seconds(30 * scale);
}

} // namespace

// F16: task writes happen-before Wait() returns (fetch_sub acq_rel + waiter
// acquire over the release sequence). Every slot written by a task must be
// visible to the joining thread, across many waves.
TEST(JobCounterTest, ZeroCrossingPublishesTaskWrites)
{
    JobSystem::WorkStealingThreadPool pool(4);

    constexpr int kWaves = 500;
    constexpr int kTasks = 16;
    std::vector<uint64_t> slots(kTasks, 0);

    for (int wave = 1; wave <= kWaves; ++wave)
    {
        JobSystem::JobCounter counter;
        for (int t = 0; t < kTasks; ++t)
        {
            // Plain non-atomic writes: visibility is carried by the counter.
            pool.Run([&slots, t, wave] { slots[t] = static_cast<uint64_t>(wave); }, counter);
        }
        pool.Wait(counter);
        for (int t = 0; t < kTasks; ++t)
        {
            ASSERT_EQ(slots[t], static_cast<uint64_t>(wave))
                << "task write not visible after Wait (wave " << wave << ", slot " << t << ")";
        }
        EXPECT_TRUE(counter.IsZero());
        EXPECT_FALSE(counter.HasAnyFailed());
    }
}

// F16: exceptions ALWAYS decrement (scope guard in the envelope). A throwing
// task must release the waiter, and F19's sticky flag must be set by the time
// the Wait it releases returns.
TEST(JobCounterTest, ThrowingTaskStillReleasesWaiter)
{
    JobSystem::WorkStealingThreadPool pool(4);

    JobSystem::JobCounter counter;
    std::atomic<int> ran{0};
    for (int t = 0; t < 8; ++t)
    {
        pool.Run(
            [&ran, t]
            {
                ran.fetch_add(1, std::memory_order_relaxed);
                if ((t % 2) == 0)
                {
                    throw std::runtime_error("intentional test failure");
                }
            },
            counter);
    }
    pool.Wait(counter); // hangs here if an exception skipped a decrement
    EXPECT_EQ(ran.load(), 8);
    EXPECT_TRUE(counter.IsZero());
    EXPECT_TRUE(counter.HasAnyFailed()); // F19 sticky, no per-task attribution
}

// Decrement contract: once per counted unit, on any thread, after the
// matching Add. A driver's completion thread that is not a pool thread
// releases the waiter exactly as a pool task's envelope does.
TEST(JobCounterTest, DecrementFromAForeignThreadReleasesWait)
{
    JobSystem::WorkStealingThreadPool pool(2);

    constexpr int kUnits = 3;
    JobSystem::JobCounter counter;
    counter.Add(kUnits);

    int completed = 0; // plain write: visibility is carried by the counter
    std::thread driver(
        [&counter, &completed]
        {
            for (int unit = 0; unit < kUnits; ++unit)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                ++completed;
                counter.Decrement();
            }
        });

    pool.Wait(counter); // parks until the driver's last Decrement
    EXPECT_EQ(completed, kUnits);
    EXPECT_TRUE(counter.IsZero());
    driver.join();
}

// The stack-lifetime UAF pattern (binding rationale carried over from the
// pre-slice-5 ParallelAlgorithms barrier): the counter is destroyed the
// instant Wait() returns, so the last decrementer must already be out of the
// counter's mutex by then. A violation shows up as a crash / assert / ASan
// report under this churn.
TEST(JobCounterTest, StackLifetimeChurn)
{
    JobSystem::WorkStealingThreadPool pool(4);

    constexpr int kIterations = 20000;
    std::atomic<uint64_t> sink{0};
    for (int i = 0; i < kIterations; ++i)
    {
        JobSystem::JobCounter counter; // stack-owned, destroyed immediately after Wait
        for (int t = 0; t < 2; ++t)
        {
            pool.Run([&sink] { sink.fetch_add(1, std::memory_order_relaxed); }, counter);
        }
        pool.Wait(counter);
    }
    EXPECT_EQ(sink.load(), static_cast<uint64_t>(kIterations) * 2);
}

// Reuse across waves: one counter, many Add/Run/Wait cycles, including
// Add()/Decrement() used directly as a bare barrier (the ParallelAlgorithms
// shape) between Run-based waves.
TEST(JobCounterTest, ReuseAcrossWaves)
{
    JobSystem::WorkStealingThreadPool pool(4);

    JobSystem::JobCounter counter;
    std::atomic<int> total{0};

    for (int wave = 0; wave < 100; ++wave)
    {
        const int fanout = 1 + (wave % 7);
        for (int t = 0; t < fanout; ++t)
        {
            pool.Run([&total] { total.fetch_add(1, std::memory_order_relaxed); }, counter);
        }
        pool.Wait(counter);
        ASSERT_TRUE(counter.IsZero());

        // Bare-barrier wave through the same counter (Add before publish, F16).
        counter.Add(1);
        pool.EnqueueWork(
            [&total, &counter]
            {
                total.fetch_add(1, std::memory_order_relaxed);
                counter.Decrement();
            });
        pool.Wait(counter);
        ASSERT_TRUE(counter.IsZero());
    }
    EXPECT_GT(total.load(), 0);
}

// Every ECS wave and parallel query forks on a fresh stack counter. Its tagged
// queue is recycled whole, so after warmup a fork on a fresh counter
// allocates nothing. The allocation counter sees the queue object and, through
// JobSystem::QueueAllocationTraits, the queue's blocks, producer and block
// index, so a fork that constructs or grows a queue reads nonzero.
//
// A thread's first slab release builds its producer in the slab freelist, a
// one-time allocation per thread. The waiting test thread runs most of its own
// jobs, so a worker can reach its first release late; the warm-up lasts until
// every worker has released a slab, which its jobs do before marking it primed.
TEST(JobCounterTest, ForkOnAFreshCounterAllocatesNothingAfterWarmup)
{
    constexpr int kWarmup = 1000;
    constexpr int kForks = 10000;
    constexpr int kFanout = 8;
    constexpr size_t kWorkers = 4;

    JobSystem::WorkStealingThreadPool pool(kWorkers);
    std::atomic<uint64_t> sink{0};
    std::array<std::atomic<bool>, kWorkers> primed{};
    auto fork = [&pool, &sink, &primed](bool prime)
    {
        JobSystem::JobCounter counter;
        for (int t = 0; t < kFanout; ++t)
        {
            pool.Run([&pool, &sink, &primed, prime]
                     {
                         sink.fetch_add(1, std::memory_order_relaxed);
                         if (const size_t worker = pool.GetCurrentWorkerId(); prime && worker < primed.size())
                         {
                             JobSystem::Detail::ReleaseTaskSlab(JobSystem::Detail::AcquireTaskSlab());
                             primed[worker].store(true, std::memory_order_release);
                         }
                     },
                     counter);
        }
        pool.Wait(counter);
    };
    const auto everyWorkerPrimed = [&primed]
    { return std::all_of(primed.begin(), primed.end(), [](const std::atomic<bool>& p) { return p.load(std::memory_order_acquire); }); };

    int warmups = 0;
    const SteadyClock::time_point deadline = SteadyClock::now() + ParkedWaiterDeadline();
    while (warmups < kWarmup || (!everyWorkerPrimed() && SteadyClock::now() < deadline))
    {
        fork(true);
        ++warmups;
    }
    ASSERT_TRUE(everyWorkerPrimed()) << "a worker ran none of the warm-up's jobs";
    std::uint64_t forkAllocations = 0;
    {
        const Memory::AllocationCountScope allocations(Memory::CountWindow::Process);
        for (int i = 0; i < kForks; ++i)
        {
            fork(false);
        }
        forkAllocations = allocations.Count();
    }

    const double allocsPerFork = static_cast<double>(forkAllocations) / kForks;
    std::printf("[JobCounterAlloc] %.3f heap allocs per fresh-counter fork of %d\n", allocsPerFork,
                kFanout);
    EXPECT_EQ(sink.load(), static_cast<uint64_t>(warmups + kForks) * kFanout);
    EXPECT_EQ(forkAllocations, 0u);
}

namespace {

// The shared pool's slot count (kSharedTaggedJobsSlots in JobCounter.cpp).
constexpr int kSharedTaggedJobsSlots = 64;

// Forks once on each of kSharedTaggedJobsSlots counters held alive together,
// so they take this thread's cached queue and every pooled one before any is
// constructed, then releases them all; returns the operator-new calls made on
// every thread meanwhile.
// Each counter's last user drops here: the first released is cached on this
// thread and the rest are pooled.
unsigned long long ForkOnEveryPooledQueue(JobSystem::WorkStealingThreadPool& pool)
{
    constexpr auto kStubRetireWait = std::chrono::milliseconds(50);

    const Memory::AllocationCountScope allocations(Memory::CountWindow::Process);
    {
        std::array<JobSystem::JobCounter, kSharedTaggedJobsSlots> counters;
        for (JobSystem::JobCounter& counter : counters)
        {
            pool.Run([] {}, counter);
            pool.Wait(counter);
        }
        std::this_thread::sleep_for(kStubRetireWait);
    }
    return allocations.Count();
}

} // namespace

// A thread that forked keeps one queue cached until it exits, then returns it
// to the shared pool. The first thread leaves the pool one short of full and
// its cached queue in hand; the second thread's forks need every pooled queue
// plus that one, so they construct none only if the exit returned it.
TEST(JobCounterTest, ExitingThreadReturnsItsCachedQueueToThePool)
{
    JobSystem::WorkStealingThreadPool pool(2);
    std::thread([&pool] { ForkOnEveryPooledQueue(pool); }).join();
    unsigned long long secondThreadNews = 0;
    std::thread([&pool, &secondThreadNews] { secondThreadNews = ForkOnEveryPooledQueue(pool); }).join();

    std::printf("[JobCounterThreadExit] %llu operator-new calls on the second thread\n", secondThreadNews);
    EXPECT_EQ(secondThreadNews, 0u) << "the exiting thread's cached queue did not reach the shared pool";
}

// With every worker held, the forking thread runs every job of its forks
// itself, and the tasks the forks leave queued for the workers stay within
// the pool's budget of unstarted fork tasks instead of one per job. Those
// tasks never keep a fork's queue from being recycled, so the forks construct
// nothing. Once the workers are free the queued tasks drain and give their
// budget back: a fork publishes a task for each of its jobs again.
TEST(JobCounterTest, ForksWhileEveryWorkerIsHeldStayBoundedAndAllocationFree)
{
    constexpr int kForks = 2000;
    constexpr int kFanout = 8;

    JobSystem::WorkStealingThreadPool pool(4);
    const size_t bound = JobSystem::WorkStealingThreadPool::kMaxUnstartedForkTasksPerWorker * pool.GetWorkerCount();
    std::atomic<uint64_t> sink{0};
    auto publish = [&pool, &sink](JobSystem::JobCounter& counter)
    {
        for (int t = 0; t < kFanout; ++t)
        {
            pool.Run([&sink] { sink.fetch_add(1, std::memory_order_relaxed); }, counter);
        }
    };
    auto fork = [&pool, &publish]
    {
        JobSystem::JobCounter counter;
        publish(counter);
        pool.Wait(counter);
    };

    HeldWorkers held(pool);
    fork();
    std::uint64_t forkAllocations = 0;
    {
        const Memory::AllocationCountScope allocations(Memory::CountWindow::Process);
        for (int i = 0; i < kForks; ++i)
        {
            fork();
        }
        forkAllocations = allocations.Count();
    }
    const size_t queuedWhileHeld = pool.GetApproximateQueueSize();
    held.Release();
    std::printf("[JobCounterHeld] %llu allocations over %d forks, %zu tasks queued (bound %zu)\n",
                static_cast<unsigned long long>(forkAllocations), kForks, queuedWhileHeld, bound);
    EXPECT_EQ(forkAllocations, 0u);
    EXPECT_LE(queuedWhileHeld, bound);

    ASSERT_TRUE(WaitUntil([&pool] { return pool.GetPendingTasksApprox() == 0; }, ParkedWaiterDeadline()));
    HeldWorkers heldAgain(pool);
    size_t publishedAfterTheDrain = 0;
    {
        JobSystem::JobCounter counter;
        publish(counter);
        publishedAfterTheDrain = pool.GetApproximateQueueSize();
        pool.Wait(counter);
    }
    heldAgain.Release();
    EXPECT_EQ(publishedAfterTheDrain, static_cast<size_t>(kFanout)) << "the drained tasks did not give their budget back";
    EXPECT_EQ(sink.load(), static_cast<uint64_t>(kForks + 2) * kFanout);
}

// A Run the budget refuses still has its job run by a worker when nobody
// waits on its counter: a caller that publishes and polls IsZero (the scene
// TLAS request) is never stranded. The workers are held while another
// counter's Runs take the whole budget, then the polled counter's three
// Runs are refused; the workers are freed and nobody waits.
TEST(JobCounterTest, ARefusedRunStillRunsWithoutAWaiter)
{
    JobSystem::WorkStealingThreadPool pool(2);
    const size_t bound = JobSystem::WorkStealingThreadPool::kMaxUnstartedForkTasksPerWorker * pool.GetWorkerCount();
    std::atomic<int> ran{0};
    JobSystem::JobCounter spent;
    JobSystem::JobCounter polled;

    HeldWorkers held(pool);
    for (size_t i = 0; i < bound; ++i)
    {
        pool.Run([] {}, spent);
    }
    for (int i = 0; i < 3; ++i)
    {
        pool.Run([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, polled);
    }
    held.Release();
    const bool ranWithoutAWaiter = WaitUntil([&polled] { return polled.IsZero(); }, ParkedWaiterDeadline());
    pool.Wait(polled);
    pool.Wait(spent);

    EXPECT_TRUE(ranWithoutAWaiter) << "a refused Run's job waited for a waiter";
    EXPECT_EQ(ran.load(), 3);
}

#if GE_DEBUG_INSTRUMENTATION
namespace {

// A hook that holds the first tagged-queue visit reaching ProvedEmpty until
// released.
struct VisitHold
{
    static inline std::atomic<bool> Held{false};
    static inline std::atomic<bool> Released{false};

    static void Hook(JobSystem::WorkStealingThreadPool::TaggedVisitPoint)
    {
        bool expected = false;
        if (!Held.compare_exchange_strong(expected, true))
            return;
        while (!Released.load())
            std::this_thread::yield();
    }

    VisitHold()
    {
        Held.store(false);
        Released.store(false);
        JobSystem::WorkStealingThreadPool::SetTaggedVisitHookForTests(&VisitHold::Hook);
    }
    ~VisitHold()
    {
        Released.store(true);
        JobSystem::WorkStealingThreadPool::SetTaggedVisitHookForTests(nullptr);
    }
    void Release() { Released.store(true); }
};

} // namespace
#endif

// A visit that has drained its queue and is leaving as a refused Run
// publishes into that queue never strands the job: either the Run sees no
// visit pending and publishes one, or the leaving visit sees the job and
// stays. The only worker's visit is held between proving the queue empty and
// re-reading its job count while another counter's Runs take the whole
// budget, then the queue's counter forks once more; nobody waits on it.
TEST(JobCounterTest, AVisitLeavingAsAJobArrivesStays)
{
#if !GE_DEBUG_INSTRUMENTATION
    GTEST_SKIP() << "needs the Debug-only tagged-visit hook (GE_DEBUG_INSTRUMENTATION)";
#else
    JobSystem::WorkStealingThreadPool pool(1);
    const size_t bound = JobSystem::WorkStealingThreadPool::kMaxUnstartedForkTasksPerWorker * pool.GetWorkerCount();
    std::atomic<int> ran{0};
    JobSystem::JobCounter leaving;
    JobSystem::JobCounter spent;
    bool ranWithoutAWaiter = false;
    {
        VisitHold hold;
        pool.Run([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, leaving);
        ASSERT_TRUE(WaitUntil([] { return VisitHold::Held.load(); }, ParkedWaiterDeadline()))
            << "the visit never proved its queue empty";
        for (size_t i = 0; i < bound; ++i)
        {
            pool.Run([] {}, spent);
        }
        pool.Run([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, leaving);
        hold.Release();
        ranWithoutAWaiter = WaitUntil([&leaving] { return leaving.IsZero(); }, ParkedWaiterDeadline());
    }
    pool.Wait(leaving);
    pool.Wait(spent);

    EXPECT_TRUE(ranWithoutAWaiter) << "the job published as the visit left was stranded";
    EXPECT_EQ(ran.load(), 2);
#endif
}

// A queue whose counter is gone while tasks the counter's Runs published are
// still queued is recycled at once; when the cache and the shared pool are
// full it is deleted, but only after the last of those tasks has run. One
// more counter than the pool's slots and a thread's cache hold together
// forks while the workers are held, so at least one queue is released into
// full slots with its tasks still queued (ASan reads the use after free).
TEST(JobCounterTest, AnOrphanedQueueOutlivesItsQueuedTasks)
{
    constexpr int kCounters = 66; // kSharedTaggedJobsSlots, this thread's cache, one more

    JobSystem::WorkStealingThreadPool pool(2);
    HeldWorkers held(pool);
    {
        std::array<JobSystem::JobCounter, kCounters> counters;
        for (JobSystem::JobCounter& counter : counters)
        {
            pool.Run([] {}, counter);
            pool.Wait(counter);
        }
    }
    held.Release();
    EXPECT_TRUE(WaitUntil([&pool] { return pool.GetPendingTasksApprox() == 0; }, ParkedWaiterDeadline()));
}

// A visit queued for one counter runs the jobs of the counter now using its
// recycled queue, and that counter's counts come out exact. While the only
// worker is held, counters forked and joined one after another on this
// thread reuse its cached queue and leave the whole budget of visits queued
// for it; the next counter on that queue has its Runs refused with a visit
// pending, so they publish nothing. Only the stale visits can run its jobs
// before anyone waits.
TEST(JobCounterTest, AStaleVisitRunsTheJobsOfTheCounterNowUsingItsQueue)
{
    constexpr int kLaterJobs = 3;

    JobSystem::WorkStealingThreadPool pool(1);
    const size_t bound = JobSystem::WorkStealingThreadPool::kMaxUnstartedForkTasksPerWorker * pool.GetWorkerCount();
    std::atomic<int> ran{0};
    std::atomic<int> ranOnWorker{0};
    JobSystem::JobCounter later;

    HeldWorkers held(pool);
    for (size_t i = 0; i < bound; ++i)
    {
        JobSystem::JobCounter earlier;
        pool.Run([] {}, earlier);
        pool.Wait(earlier);
    }
    const uint64_t publishedBefore = pool.GetPublishedJobCount();
    for (int i = 0; i < kLaterJobs; ++i)
    {
        pool.Run(
            [&]
            {
                if (pool.GetCurrentWorkerId() < pool.GetWorkerCount())
                    ranOnWorker.fetch_add(1, std::memory_order_relaxed);
                ran.fetch_add(1, std::memory_order_relaxed);
            },
            later);
    }
    const uint64_t publishedByLater = pool.GetPublishedJobCount() - publishedBefore;
    held.Release();
    const bool ranWithoutAWaiter = WaitUntil([&later] { return later.IsZero(); }, ParkedWaiterDeadline());
    pool.Wait(later);

    EXPECT_EQ(publishedByLater, 0u) << "the later counter's Runs published a visit of their own";
    EXPECT_TRUE(ranWithoutAWaiter) << "no stale visit ran the later counter's jobs";
    EXPECT_EQ(ranOnWorker.load(), kLaterJobs);
    EXPECT_EQ(ran.load(), kLaterJobs);
    EXPECT_TRUE(later.IsZero());
}

// Reset() re-arms a reused counter: wave 1 fails and sets the sticky flag,
// Reset clears it at quiescence, and a clean wave 2 reads false — without
// Reset, reuse-across-waves inherits wave 1's failure forever (F19).
TEST(JobCounterTest, ResetClearsStickyFailureBetweenWaves)
{
    JobSystem::WorkStealingThreadPool pool(4);

    JobSystem::JobCounter counter;

    // Wave 1: one task throws.
    for (int t = 0; t < 4; ++t)
    {
        pool.Run(
            [t]
            {
                if (t == 2)
                {
                    throw std::runtime_error("intentional wave-1 failure");
                }
            },
            counter);
    }
    pool.Wait(counter);
    ASSERT_TRUE(counter.IsZero());
    ASSERT_TRUE(counter.HasAnyFailed());

    counter.Reset();
    EXPECT_FALSE(counter.HasAnyFailed());

    // Wave 2: clean — must not read wave 1's failure.
    std::atomic<int> ran{0};
    for (int t = 0; t < 4; ++t)
    {
        pool.Run([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, counter);
    }
    pool.Wait(counter);
    EXPECT_EQ(ran.load(), 4);
    EXPECT_TRUE(counter.IsZero());
    EXPECT_FALSE(counter.HasAnyFailed());

    // A failing wave AFTER a Reset still flips the flag (Reset clears state,
    // not the mechanism).
    pool.Run([] { throw std::runtime_error("post-reset failure"); }, counter);
    pool.Wait(counter);
    EXPECT_TRUE(counter.HasAnyFailed());
}

#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
// Reset is legal only at quiescence: count != 0 trips the Debug assert.
TEST(JobCounterDeathTest, ResetWithOutstandingTasksAsserts)
{
    JobSystem::JobCounter counter;
    counter.Add(1);
    EXPECT_DEATH(
        {
            GameEngine::Tests::DisableAbortDialogs();
            counter.Reset();
        },
        "Reset with outstanding tasks");
    counter.Decrement(); // keep the parent counter's dtor assert satisfied
}
#endif

// F17: a worker-thread waiter executes ONLY tasks tagged with the same
// counter. A single-worker pool pins the scenario: while the sole worker is
// parked/participating inside Wait(counter), planted FOREIGN tasks (published
// before the tagged ones) must remain untouched — arbitrary drain would
// execute them from inside the Wait. They run only after the worker task
// finishes, in publication order.
TEST(ParticipatingWaitTest, WorkerWaiterExecutesOnlySameCounterTasks)
{
    JobSystem::WorkStealingThreadPool pool(1);

    std::atomic<bool> workerTaskStarted{false};
    std::atomic<bool> foreignPlanted{false};
    std::atomic<int> foreignRan{0};
    std::atomic<int> taggedRan{0};
    std::atomic<int> foreignSeenDuringWait{-1};
    std::atomic<int> taggedSeenAfterWait{-1};
    std::atomic<bool> done{false};

    pool.EnqueueWork(
        [&]
        {
            workerTaskStarted.store(true, std::memory_order_release);
            while (!foreignPlanted.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            // The foreign tasks now sit in the pool queues ahead of anything
            // below. Fork tagged work and join it from this WORKER thread.
            JobSystem::JobCounter counter;
            for (int t = 0; t < 8; ++t)
            {
                pool.Run([&taggedRan] { taggedRan.fetch_add(1, std::memory_order_relaxed); },
                         counter);
            }
            pool.Wait(counter); // participates: consumes ONLY the 8 tagged tasks

            foreignSeenDuringWait.store(foreignRan.load(std::memory_order_relaxed),
                                        std::memory_order_relaxed);
            taggedSeenAfterWait.store(taggedRan.load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
            done.store(true, std::memory_order_release);
        });

    while (!workerTaskStarted.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    for (int f = 0; f < 4; ++f)
    {
        pool.EnqueueWork([&foreignRan] { foreignRan.fetch_add(1, std::memory_order_relaxed); });
    }
    foreignPlanted.store(true, std::memory_order_release);

    const auto deadline = SteadyClock::now() + std::chrono::seconds(30);
    while (!done.load(std::memory_order_acquire) && SteadyClock::now() < deadline)
    {
        std::this_thread::yield();
    }
    ASSERT_TRUE(done.load()) << "participating wait deadlocked on the single-worker pool";

    EXPECT_EQ(foreignSeenDuringWait.load(), 0)
        << "participating Wait executed foreign (non-counter) tasks — arbitrary drain";
    EXPECT_EQ(taggedSeenAfterWait.load(), 8);

    // The foreign tasks were never dropped: the worker reaches them after the
    // waiting task returns.
    const auto foreignDeadline = SteadyClock::now() + std::chrono::seconds(30);
    while (foreignRan.load(std::memory_order_relaxed) < 4 && SteadyClock::now() < foreignDeadline)
    {
        std::this_thread::yield();
    }
    EXPECT_EQ(foreignRan.load(), 4);
}

// F17 nested counters: a task counted into A forks counter-B work and waits
// on B from the worker — on a single-worker pool this deadlocks unless the
// waiter participates in B.
TEST(ParticipatingWaitTest, NestedCountersNoDeadlock)
{
    JobSystem::WorkStealingThreadPool pool(1);

    std::atomic<int> innerRan{0};
    std::atomic<bool> outerDone{false};

    JobSystem::JobCounter counterA;
    pool.Run(
        [&]
        {
            JobSystem::JobCounter counterB;
            for (int t = 0; t < 4; ++t)
            {
                pool.Run([&innerRan] { innerRan.fetch_add(1, std::memory_order_relaxed); },
                         counterB);
            }
            pool.Wait(counterB); // worker waits on B while executing an A task
            outerDone.store(true, std::memory_order_release);
        },
        counterA);

    pool.Wait(counterA); // external join
    EXPECT_TRUE(outerDone.load());
    EXPECT_EQ(innerRan.load(), 4);
}

// A waiter that is not a worker joins only at the last decrement: it runs
// the gated jobs it can claim itself (participation, design decision D1) and
// parks for the ones the workers hold, so Wait returns with every job done.
TEST(ParticipatingWaitTest, ExternalWaiterJoinsUntilLastDecrement)
{
    JobSystem::WorkStealingThreadPool pool(2);

    std::atomic<bool> release{false};
    std::atomic<int> ran{0};
    JobSystem::JobCounter counter;
    for (int t = 0; t < 4; ++t)
    {
        pool.Run(
            [&release, &ran]
            {
                while (!release.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                ran.fetch_add(1, std::memory_order_relaxed);
            },
            counter);
    }

    std::thread releaser(
        [&release]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            release.store(true, std::memory_order_release);
        });

    pool.Wait(counter); // the tasks are gated: returns only after the last decrement
    EXPECT_EQ(ran.load(), 4);
    EXPECT_TRUE(counter.IsZero());
    releaser.join();
}

// Run() racing / following Shutdown(): the stub inherits EnqueueWork's F13c
// never-drop contract, so the envelope always executes (on a worker, in
// Shutdown's drain, or synchronously on the caller) and Wait() always
// returns with the counter fully drained.
TEST(ParticipatingWaitTest, RunVsShutdownNeverStrandsWaiter)
{
    for (int round = 0; round < 20; ++round)
    {
        JobSystem::WorkStealingThreadPool pool(2);
        JobSystem::JobCounter counter;
        std::atomic<int> ran{0};

        std::thread shutdowner([&pool] { pool.Shutdown(); });
        for (int t = 0; t < 64; ++t)
        {
            pool.Run([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, counter);
        }
        pool.Wait(counter);
        shutdowner.join();

        EXPECT_EQ(ran.load(), 64) << "a Run() racing Shutdown() dropped work (round "
                                  << round << ")";
        EXPECT_TRUE(counter.IsZero());
    }

    // Post-shutdown Run: executes synchronously on the caller, counter drains.
    JobSystem::WorkStealingThreadPool pool(2);
    pool.Shutdown();
    JobSystem::JobCounter counter;
    std::atomic<int> ran{0};
    pool.Run([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, counter);
    pool.Wait(counter);
    EXPECT_EQ(ran.load(), 1);
    EXPECT_TRUE(counter.IsZero());
}

// Slice-5 audit findings 1+6: a pool worker PARKED inside a participating
// Wait must be woken by a cross-thread Run() on the same counter — the
// publish-side notify (inside Run's critical section) is the only wake arrow
// that reaches it, and it had zero coverage. Driven deterministically: one
// long-running envelope occupies the other worker (tagged occupancy == 0,
// count > 0 -> the waiter parks), then an EXTERNAL thread Runs a fresh task
// into the counter while the waiter is parked.
//
// The finding-1 UAF shape rides on top: the counter is destroyed IMMEDIATELY
// after the waiter's Wait() returns, while the external Run() may still be in
// its post-publish tail. Post-fix, Run's last counter touch is inside its
// publish critical section, so this is safe; pre-fix, the trailing
// NotifyPublished locks the destroyed counter's mutex (crash/deadlock —
// manifests under the loop without ASan). A main-thread pin (bare Add,
// dropped by the late task itself) keeps the counter provably alive for the
// external Run() call itself without serializing away the racy tail.
TEST(ParticipatingWaitTest, ParkedParticipatingWaiterWokenByCrossThreadRun)
{
    JobSystem::WorkStealingThreadPool pool(2);

    constexpr int kIterations = 1000;
    constexpr int kProgressStride = 100;
    for (int i = 0; i < kIterations; ++i)
    {
        if (i % kProgressStride == 0)
        {
            // Progress heartbeat: under CI load an iteration legitimately
            // stretches to 100s of ms (scheduler-bound), and a silent
            // 1000-iteration loop is externally indistinguishable from a
            // wedge (PR #342 triage).
            std::printf("[ParkedWaiter] iteration %d/%d\n", i, kIterations);
            std::fflush(stdout);
        }

        std::atomic<bool> releaseLong{false};
        std::atomic<bool> longRan{false};
        std::atomic<bool> lateRan{false};
        std::atomic<bool> waiterEntered{false};
        std::atomic<bool> waiterDone{false};
        std::atomic<bool> trueZeroAtWaitReturn{false};

        std::thread runner;
        {
            JobSystem::JobCounter counter;
            counter.Add(1); // pin: dropped by the late task's body

            // Occupies one worker; holds count > 0 while its envelope has
            // already been consumed (tagged occupancy 0 — the parked-
            // participant precondition).
            pool.Run(
                [&releaseLong, &longRan]
                {
                    while (!releaseLong.load(std::memory_order_acquire))
                    {
                        std::this_thread::yield();
                    }
                    longRan.store(true, std::memory_order_release);
                },
                counter);

            // The other worker becomes the participating waiter.
            pool.EnqueueWork(
                [&]
                {
                    waiterEntered.store(true, std::memory_order_release);
                    pool.Wait(counter);
                    // Wait returned => true zero: every counted task (long,
                    // late, pin) already completed, and the counter's
                    // acq_rel decrement chain publishes their writes.
                    trueZeroAtWaitReturn.store(longRan.load(std::memory_order_acquire) &&
                                                   lateRan.load(std::memory_order_acquire),
                                               std::memory_order_release);
                    waiterDone.store(true, std::memory_order_release);
                });

            const auto enterDeadline = SteadyClock::now() + ParkedWaiterDeadline();
            while (!waiterEntered.load(std::memory_order_acquire))
            {
                if (SteadyClock::now() >= enterDeadline)
                {
                    ADD_FAILURE() << "waiter task never started (iter " << i << ")";
                    // Unwedge without destructing live state: release the
                    // long task, drop the pin ourselves (the late Run was
                    // never submitted on this path), then drain the pool so
                    // the queued waiter task executes — against still-alive
                    // captures — and returns on the zeroed counter. Only
                    // then may the counter leave scope. `runner` was never
                    // started (not joinable), so its destructor is safe.
                    releaseLong.store(true, std::memory_order_release);
                    counter.Decrement();
                    pool.Shutdown();
                    return;
                }
                std::this_thread::yield();
            }
            BusyWaitUs(100); // let the waiter actually park (count>0, occupancy 0)

            // EXTERNAL thread publishes into the counter while the waiter is
            // parked. Both workers are busy (long + waiter), so the parked
            // participant is the consumer that must wake for this envelope.
            // The late task drops the main-thread pin, so the counter's zero
            // crossing races the external Run()'s post-publish tail.
            runner = std::thread(
                [&pool, &counter, &lateRan]
                {
                    pool.Run(
                        [&lateRan, &counter]
                        {
                            lateRan.store(true, std::memory_order_release);
                            counter.Decrement(); // drop the main-thread pin
                        },
                        counter);
                });

            releaseLong.store(true, std::memory_order_release);

            const auto deadline = SteadyClock::now() + ParkedWaiterDeadline();
            while (!waiterDone.load(std::memory_order_acquire) && SteadyClock::now() < deadline)
            {
                std::this_thread::yield();
            }
            if (!waiterDone.load(std::memory_order_acquire))
            {
                // Deadline tripped. Do NOT fall off the test body with a
                // live counter: worker B may still be parked inside
                // Wait(counter) — destructing the counter under it is UB on
                // the condvar, trips the dtor's count!=0 Debug assert (a
                // modal dialog without the suite's CRT report-mode fix), and
                // would destruct `runner` joinable (std::terminate). This
                // exact sequence was the ~12-minute "wedge" in the PR #342
                // triage. Record the failure, then unwedge in dependency
                // order: join the runner (its Run is published; F13c — the
                // late task WILL execute and zero the counter), then wait for
                // the waiter to leave Wait() before the counter leaves scope.
                ADD_FAILURE() << "parked participating waiter did not wake within "
                              << ParkedWaiterDeadline().count() << "s (iter " << i
                              << ") — lost wakeup, or set GE_TEST_DEADLINE_SCALE on a "
                                 "saturated box";
                runner.join();
                int waitedMs = 0;
                while (!waiterDone.load(std::memory_order_acquire))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    waitedMs += 100;
                    if (waitedMs % 5000 == 0)
                    {
                        std::fprintf(stderr,
                                     "[ParkedWaiter] still draining the parked waiter (%ds)\n",
                                     waitedMs / 1000);
                        std::fflush(stderr);
                    }
                }
                return; // counter destructs only now, drained and zero
            }
        } // counter destroyed IMMEDIATELY after Wait returned (finding-1 shape),
          // while the external Run may still be in its post-publish tail
        runner.join();

        // Verified OUTSIDE the counter scope and after the join: an
        // early-returning ASSERT here must not destruct `runner` joinable or
        // race the counter teardown.
        ASSERT_TRUE(trueZeroAtWaitReturn.load())
            << "Wait returned before all counted tasks completed (iter " << i << ")";
        ASSERT_TRUE(lateRan.load());
        ASSERT_TRUE(longRan.load());
    }
}

// Finding-1 accelerant: leftover no-op stubs — stubs whose envelopes were
// eaten by a participating waiter — coexist in the pool queues with FRESH
// Runs on the same counter. A leftover stub can consume and COMPLETE a fresh
// envelope the instant its enqueue commits, which is exactly what makes the
// producer's post-publish window lethal pre-fix: zero-crossing, waiter
// release, and counter destruction can all happen while the producer is
// still inside Run(). The pin (dropped by the last fresh task) keeps the
// counter alive for the producer's Run() calls themselves while leaving the
// post-publish tail of the LAST Run racing the destruction.
TEST(JobCounterStressTest, LeftoverStubsCoexistWithFreshRuns)
{
    JobSystem::WorkStealingThreadPool pool(4);

    constexpr int kIterations = 2000;
    constexpr int kPhase1Fanout = 8;
    constexpr int kFreshRuns = 4;
    std::atomic<uint64_t> sink{0};

    for (int i = 0; i < kIterations; ++i)
    {
        std::thread producer;
        {
            JobSystem::JobCounter counter;

            // Phase 1: fork tagged work and join it from a WORKER. The
            // participating waiter races the stubs for the envelopes, so
            // some stubs are left behind to no-op — they are still draining
            // through the pool queues when phase 2 publishes.
            std::atomic<bool> joined{false};
            pool.EnqueueWork(
                [&pool, &counter, &sink, &joined]
                {
                    for (int t = 0; t < kPhase1Fanout; ++t)
                    {
                        pool.Run([&sink] { sink.fetch_add(1, std::memory_order_relaxed); },
                                 counter);
                    }
                    pool.Wait(counter); // participates: eats envelopes ahead of their stubs
                    joined.store(true, std::memory_order_release);
                });
            while (!joined.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            // Phase 2: fresh Runs on the SAME counter from another thread,
            // racing the leftover stubs. The last fresh task drops the pin.
            counter.Add(1); // pin
            producer = std::thread(
                [&pool, &counter, &sink]
                {
                    for (int t = 0; t < kFreshRuns - 1; ++t)
                    {
                        pool.Run([&sink] { sink.fetch_add(1, std::memory_order_relaxed); },
                                 counter);
                    }
                    pool.Run(
                        [&sink, &counter]
                        {
                            sink.fetch_add(1, std::memory_order_relaxed);
                            counter.Decrement(); // drop the pin
                        },
                        counter);
                });

            pool.Wait(counter); // external join
        } // counter destroyed immediately; the producer's last Run may still be in its tail
        producer.join();
    }

    EXPECT_EQ(sink.load(),
              static_cast<uint64_t>(kIterations) * (kPhase1Fanout + kFreshRuns));
}

// Strand detector across the NEW consumption path (slice-1 pattern): with the
// ~1ms backstop rescue raised to 500ms, a wake lost anywhere in the
// Run/stub/participate/park pipeline pins a fork at backstop scale. No fork
// sample may exceed 100ms.
TEST(JobCounterStrandTest, NoBackstopRescueUnderStorm)
{
    JobSystem::WorkStealingThreadPool pool(4);
    pool.SetSleepBackstopForTest(500000);

    constexpr int kIterations = 30000;
    std::mt19937 rng(0x5EED5);
    std::uniform_int_distribution<int> gapUs(0, 200);

    std::atomic<uint64_t> sink{0};
    double worstUs = 0.0;
    int over100ms = 0;
    for (int i = 0; i < kIterations; ++i)
    {
        const auto t0 = SteadyClock::now();
        JobSystem::JobCounter counter;
        pool.Run([&sink] { sink.fetch_add(1, std::memory_order_relaxed); }, counter);
        pool.Run([&sink] { sink.fetch_add(1, std::memory_order_relaxed); }, counter);
        pool.Wait(counter);
        const double us = ElapsedUs(t0, SteadyClock::now());
        worstUs = std::max(worstUs, us);
        if (us > 100000.0)
        {
            ++over100ms;
        }
        BusyWaitUs(gapUs(rng));
    }

    std::printf("[JobCounterStrand] iters=%d worst=%.1fus over100ms=%d\n", kIterations, worstUs,
                over100ms);
    EXPECT_EQ(over100ms, 0)
        << "a fork sample >100ms with the rescue disabled is a lost wakeup; worst = " << worstUs
        << "us";
    EXPECT_EQ(sink.load(), static_cast<uint64_t>(kIterations) * 2);
}

namespace {

double Percentile(std::vector<double>& sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    const double idx = p * static_cast<double>(sorted.size() - 1);
    return sorted[static_cast<size_t>(idx + 0.5)];
}

} // namespace

// JobCounter fork-join round-trip bench (Run x8 + Wait, warm storm shape —
// same gaps as BENCHMARK_WarmStormFork so the rows are comparable). The
// primitive pays one envelope + one stub per task versus ParallelFor's
// single batch publish; the row quantifies that delta. The hard gate is
// deliberately loose (sanity against strands); the <20us warm-storm gate
// lives on the ParallelFor fork bench (WakeProtocolAndSpinTests).
TEST(JobSystemBench, BENCHMARK_JobCounterForkJoin)
{
    JobSystem::WorkStealingThreadPool pool(8);
    pool.SetSpinConfigForTest(100, 2);

    constexpr int kIterations = 3000;
    constexpr int kFanout = 8;
    std::mt19937 rng(0xBE7C);
    std::uniform_int_distribution<int> gapUs(20, 50);

    std::atomic<uint64_t> sink{0};
    auto fork = [&]
    {
        JobSystem::JobCounter counter;
        for (int t = 0; t < kFanout; ++t)
        {
            pool.Run([&sink] { sink.fetch_add(1, std::memory_order_relaxed); }, counter);
        }
        pool.Wait(counter);
    };

    for (int i = 0; i < 100; ++i) // warmup, not measured
    {
        fork();
    }

    std::vector<double> samples;
    samples.reserve(kIterations);
    for (int i = 0; i < kIterations; ++i)
    {
        BusyWaitUs(gapUs(rng));
        const auto t0 = SteadyClock::now();
        fork();
        samples.push_back(ElapsedUs(t0, SteadyClock::now()));
    }

    std::sort(samples.begin(), samples.end());
    std::printf("[bench] jobcounter-forkjoin x%d    median=%8.2fus p99=%8.2fus p99.9=%8.2fus worst=%8.2fus\n",
                kFanout, Percentile(samples, 0.5), Percentile(samples, 0.99), Percentile(samples, 0.999),
                samples.back());
    EXPECT_LT(Percentile(samples, 0.5), 100.0);
}

// The ParallelFor fork at the shape of the JobCounter bench above: 8 chunks of
// one element on an 8-worker pool (the caller and 7 helpers), 20-50 us apart,
// so a fork often meets workers that just went idle.
TEST(JobSystemBench, BENCHMARK_ParallelFor8)
{
    JobSystem::WorkStealingThreadPool pool(8);
    pool.SetSpinConfigForTest(100, 2);

    constexpr int kIterations = 2000;
    constexpr size_t kChunks = 8;
    std::mt19937 rng(0xBE7D);
    std::uniform_int_distribution<int> gapUs(20, 50);

    std::atomic<uint64_t> sink{0};
    auto fork = [&]
    {
        JobSystem::ParallelFor(&pool, kChunks,
                               [&sink](size_t begin, size_t end) { sink.fetch_add(end - begin, std::memory_order_relaxed); },
                               1);
    };

    for (int i = 0; i < 100; ++i) // warmup, not measured
    {
        fork();
    }

    std::vector<double> samples;
    samples.reserve(kIterations);
    for (int i = 0; i < kIterations; ++i)
    {
        BusyWaitUs(gapUs(rng));
        const auto t0 = SteadyClock::now();
        fork();
        samples.push_back(ElapsedUs(t0, SteadyClock::now()));
    }

    std::sort(samples.begin(), samples.end());
    std::printf("[bench] parallelfor x%zu            median=%8.2fus p99=%8.2fus p99.9=%8.2fus worst=%8.2fus\n",
                kChunks, Percentile(samples, 0.5), Percentile(samples, 0.99), Percentile(samples, 0.999),
                samples.back());
    EXPECT_EQ(sink.load(), (100 + kIterations) * kChunks);
    EXPECT_LT(Percentile(samples, 0.5), 100.0);
}

} // namespace GameEngine::Tests
