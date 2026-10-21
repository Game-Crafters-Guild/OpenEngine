// Inline mode (0 workers) and external waits. Inline mode executes on the
// submitting thread. A waiter that is not a pool worker (the main thread, any
// other thread) participates in its own tagged join, as a worker does: it
// runs the jobs of the counter it waits on and no others.

#include "JobSystem/JobCounter.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using JobSystem::JobCounter;
using JobSystem::WorkStealingThreadPool;

TEST(InlineMode, ExecutesAtPublishOnCaller)
{
    WorkStealingThreadPool pool(0);
    EXPECT_TRUE(pool.IsInlineMode());
    EXPECT_EQ(pool.GetWorkerCount(), 0u);

    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> ran{false};
    std::thread::id ranOn{};
    pool.EnqueueWork([&] {
        ranOn = std::this_thread::get_id();
        ran.store(true, std::memory_order_release);
    });
    // Inline mode executes before EnqueueWork returns.
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
    EXPECT_EQ(ranOn, caller);
}

TEST(InlineMode, SubmitCompletesSynchronously)
{
    WorkStealingThreadPool pool(0);
    std::atomic<int> value{0};
    auto handle = pool.Submit([&] { value.store(42, std::memory_order_release); });
    // The task already completed at publish; Wait must not block.
    handle.Wait();
    EXPECT_EQ(value.load(std::memory_order_acquire), 42);
}

TEST(InlineMode, NestedSubmissionRecurses)
{
    WorkStealingThreadPool pool(0);
    std::atomic<int> depth{0};
    pool.EnqueueWork([&] {
        depth.fetch_add(1);
        pool.EnqueueWork([&] {
            depth.fetch_add(1);
            pool.EnqueueWork([&] { depth.fetch_add(1); });
        });
    });
    EXPECT_EQ(depth.load(), 3);
}

TEST(InlineMode, ForkJoinCounterAlreadyZeroAtWait)
{
    WorkStealingThreadPool pool(0);
    JobCounter counter;
    std::atomic<int> sum{0};
    for (int i = 0; i < 8; ++i)
    {
        pool.Run([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); }, counter);
    }
    pool.Wait(counter);
    EXPECT_EQ(sum.load(), 0 + 1 + 2 + 3 + 4 + 5 + 6 + 7);
}

TEST(InlineMode, ParallelForCoversWholeRange)
{
    WorkStealingThreadPool pool(0);
    std::vector<std::atomic<int>> hits(4096);
    JobSystem::ParallelFor(&pool, hits.size(),
                                       [&](size_t begin, size_t end) {
                                           for (size_t i = begin; i < end; ++i)
                                               hits[i].fetch_add(1, std::memory_order_relaxed);
                                       },
                                       64);
    for (size_t i = 0; i < hits.size(); ++i)
    {
        ASSERT_EQ(hits[i].load(), 1) << "index " << i;
    }
}

namespace
{
// Holds every worker of a pool inside a job until Open(), or until the
// backstop expires, so a test can tell work its waiter ran from work a worker
// ran. The backstop only ends a failing run: a waiter that parks instead of
// participating is released by it and the test fails on what ran where.
class WorkerGate
{
  public:
    WorkerGate(WorkStealingThreadPool& pool, int workers) : m_Workers(workers)
    {
        for (int i = 0; i < workers; ++i)
        {
            pool.EnqueueWork([this] {
                m_Entered.fetch_add(1, std::memory_order_acq_rel);
                while (!m_Open.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < m_Backstop)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                m_Left.fetch_add(1, std::memory_order_acq_rel);
            });
        }
    }
    ~WorkerGate()
    {
        Open();
        while (m_Left.load(std::memory_order_acquire) < m_Entered.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Whether every worker is inside the gate before the deadline.
    bool AllInside() const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (m_Entered.load(std::memory_order_acquire) < m_Workers && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return m_Entered.load(std::memory_order_acquire) == m_Workers;
    }
    void Open() { m_Open.store(true, std::memory_order_release); }
    bool IsOpen() const { return m_Open.load(std::memory_order_acquire); }

  private:
    const int m_Workers;
    const std::chrono::steady_clock::time_point m_Backstop = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::atomic<int> m_Entered{0};
    std::atomic<int> m_Left{0};
    std::atomic<bool> m_Open{false};
};
} // namespace

// The waiting thread runs its own fork's jobs (design decision D1): with
// every worker held, a native thread that is not a worker finishes the whole
// fork itself, before any worker is free, on every platform, and each job it
// runs is a job body on that thread.
TEST(ExternalWait, TheMainThreadRunsItsTaggedJobsWhileWaiting)
{
    WorkStealingThreadPool pool(2);
    WorkerGate gate(pool, 2);
    ASSERT_TRUE(gate.AllInside()) << "workers never entered the gate";

    const std::thread::id waiter = std::this_thread::get_id();
    std::atomic<int> ranOnWaiter{0};
    std::atomic<int> ranAsABody{0};
    std::atomic<int> ran{0};
    JobCounter counter;
    constexpr int kJobs = 16;
    for (int i = 0; i < kJobs; ++i)
    {
        pool.Run(
            [&] {
                ran.fetch_add(1, std::memory_order_relaxed);
                if (std::this_thread::get_id() == waiter)
                    ranOnWaiter.fetch_add(1, std::memory_order_relaxed);
                if (WorkStealingThreadPool::GetCurrentJobForTests() != nullptr)
                    ranAsABody.fetch_add(1, std::memory_order_relaxed);
            },
            counter);
    }

    pool.Wait(counter);
    const bool workersFreedBeforeTheJoin = gate.IsOpen();
    gate.Open();

    EXPECT_EQ(ran.load(), kJobs);
    EXPECT_EQ(ranOnWaiter.load(), kJobs);
    EXPECT_EQ(ranAsABody.load(), kJobs) << "a tagged job the waiter ran was not a job body";
    EXPECT_FALSE(workersFreedBeforeTheJoin) << "the waiter parked until the backstop freed the workers";
}

// Participation is filtered (the design's ownership rule): a waiter runs only
// jobs tagged to the counter it waits on, never a job another counter owns
// and never a bare pool job, even when it has run every job of its own and
// must park for a unit another thread holds. Each foreign job here waits for
// this Wait to return, so a waiter that ran one would wedge itself; the
// bounded wait in the foreign body turns that wedge into a failure instead of
// a hang. The foreign jobs run on the workers once they are free.
TEST(ExternalWait, AWaiterNeverRunsAJobItsCounterDoesNotOwn)
{
    WorkStealingThreadPool pool(2);
    WorkerGate gate(pool, 2);
    ASSERT_TRUE(gate.AllInside()) << "workers never entered the gate";

    const std::thread::id waiter = std::this_thread::get_id();
    std::atomic<bool> waitReturned{false};
    std::atomic<int> foreignRan{0};
    std::atomic<int> foreignOnWaiter{0};
    std::atomic<int> foreignWedged{0};
    auto foreignBody = [&] {
        // The test's own Wait(otherCounter) below may run the other
        // counter's job on this thread; only a run inside Wait(counter) counts.
        if (std::this_thread::get_id() == waiter && !waitReturned.load(std::memory_order_acquire))
            foreignOnWaiter.fetch_add(1, std::memory_order_relaxed);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!waitReturned.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!waitReturned.load(std::memory_order_acquire))
            foreignWedged.fetch_add(1, std::memory_order_relaxed);
        foreignRan.fetch_add(1, std::memory_order_release);
    };
    // Published before the waiter's own fork, so they sit ahead of it in the
    // pool's queues: a bare job on each lane and a job of another counter.
    pool.EnqueueWork(foreignBody);
    pool.EnqueueWork(foreignBody, JobSystem::JobPriority::Background);
    JobCounter otherCounter;
    pool.Run(foreignBody, otherCounter);

    std::atomic<int> ownOnWaiter{0};
    JobCounter counter;
    constexpr int kJobs = 8;
    for (int i = 0; i < kJobs; ++i)
    {
        pool.Run(
            [&] {
                if (std::this_thread::get_id() == waiter)
                    ownOnWaiter.fetch_add(1, std::memory_order_relaxed);
            },
            counter);
    }

    // One unit the waiter cannot run: counted in by hand and released from a
    // thread of the test's own, so the waiter runs out of its own jobs while
    // the count is above zero and the foreign jobs are still queued.
    counter.Add(1);
    std::thread lateUnit([&counter] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        counter.Decrement();
    });

    pool.Wait(counter);
    waitReturned.store(true, std::memory_order_release);
    lateUnit.join();
    const int foreignBeforeTheGateOpened = foreignRan.load(std::memory_order_acquire);
    const bool workersFreedBeforeTheJoin = gate.IsOpen();
    gate.Open();
    pool.Wait(otherCounter);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (foreignRan.load(std::memory_order_acquire) < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    EXPECT_EQ(ownOnWaiter.load(), kJobs);
    EXPECT_FALSE(workersFreedBeforeTheJoin) << "the waiter parked until the backstop freed the workers";
    EXPECT_EQ(foreignBeforeTheGateOpened, 0) << "a foreign job ran inside the waiter's Wait";
    EXPECT_EQ(foreignOnWaiter.load(), 0);
    EXPECT_EQ(foreignWedged.load(), 0);
    EXPECT_EQ(foreignRan.load(), 3) << "foreign jobs are never dropped";
}
