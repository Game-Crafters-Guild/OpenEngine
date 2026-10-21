// The JobSystem's queues take their memory from the global operator new
// (JobSystem::QueueAllocationTraits), so the allocation counter sees a queue growing:
// with moodycamel's default traits its blocks come from std::malloc and a counted
// fork, submit or freelist path reads zero for them. The first pin proves the alias
// allocates through operator new; the two pool pins prove the pool's own queues use it,
// so a declaration that goes back to moodycamel's default traits reads red.

#include "JobSystem/ConcurrentQueue.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/TaskSlabPool.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Memory/AllocationCountScope.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace
{
using namespace GameEngine;

using TaskQueue = JobSystem::ConcurrentQueue<void*>;

constexpr std::size_t kBlocks = 4;

// The shared pool's slot count (kSharedTaggedJobsSlots in JobCounter.cpp).
constexpr std::size_t kSharedTaggedJobsSlots = 64;

// Blocks the JobCounter pin fills in one tagged queue that starts with one.
constexpr std::size_t kTaggedBlocks = 64;
constexpr std::size_t kTaggedTasks = kTaggedBlocks * TaskQueue::BLOCK_SIZE;

// A worker's local queue starts with an initial pool of 32 blocks (moodycamel's
// default capacity); the local-queue pin publishes kLocalGrowthBlocks beyond it.
constexpr std::size_t kLocalInitialBlocks = 32;
constexpr std::size_t kLocalGrowthBlocks = 256;
constexpr std::size_t kLocalTasks = (kLocalInitialBlocks + kLocalGrowthBlocks) * TaskQueue::BLOCK_SIZE;

void YieldUntil(const std::atomic<bool>& flag)
{
    while (!flag.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
}

void YieldUntilCount(const std::atomic<std::size_t>& count, std::size_t target)
{
    while (count.load(std::memory_order_acquire) < target)
    {
        std::this_thread::yield();
    }
}

// Holds every worker of a pool inside one task each until Release(), so nothing
// dequeues while a test fills a queue. A new Hold() and the destructor first wait for
// every earlier gate task to leave, so a worker still inside a released task is never
// held again by the reset, and no task outlives the gate.
class WorkerGate
{
  public:
    explicit WorkerGate(JobSystem::WorkStealingThreadPool& pool) : m_Pool(pool) {}

    ~WorkerGate() { YieldUntilCount(m_Exited, m_Issued); }

    WorkerGate(const WorkerGate&) = delete;
    WorkerGate& operator=(const WorkerGate&) = delete;

    void Hold()
    {
        YieldUntilCount(m_Exited, m_Issued);
        m_Open.store(false, std::memory_order_release);
        m_Held.store(0, std::memory_order_release);
        for (std::size_t i = 0; i < m_Pool.GetWorkerCount(); ++i)
        {
            ++m_Issued;
            m_Pool.EnqueueWork([this] { HoldOneWorker(); });
        }
        YieldUntilCount(m_Held, m_Pool.GetWorkerCount());
    }

    void Release() { m_Open.store(true, std::memory_order_release); }

  private:
    void HoldOneWorker()
    {
        m_Held.fetch_add(1, std::memory_order_acq_rel);
        YieldUntil(m_Open);
        m_Exited.fetch_add(1, std::memory_order_acq_rel);
    }

    JobSystem::WorkStealingThreadPool& m_Pool;
    std::atomic<bool> m_Open{false};
    std::atomic<std::size_t> m_Held{0};
    std::atomic<std::size_t> m_Exited{0};
    // Gate tasks published, read and written only by the thread that calls Hold().
    std::size_t m_Issued = 0;
};

// A queue built with no initial block pool allocates every block its producer fills.
// The producer and its block index are allocated with the token, before the window, so
// the window holds the blocks alone.
TEST(TaskQueueAllocation, TaskQueueBlockAllocationIsSeenByTheCounter)
{
    TaskQueue queue(0);
    moodycamel::ProducerToken producer(queue);

    std::uint64_t allocations = 0;
    {
        const Memory::AllocationCountScope scope(Memory::CountWindow::Process);
        for (std::size_t i = 0; i < kBlocks * TaskQueue::BLOCK_SIZE; ++i)
        {
            ASSERT_TRUE(queue.enqueue(producer, nullptr));
        }
        allocations = scope.Count();
    }

    EXPECT_EQ(queue.size_approx(), kBlocks * TaskQueue::BLOCK_SIZE);
    EXPECT_EQ(allocations, kBlocks);
}

struct TaggedQueueGrowth
{
    std::uint64_t Allocations = 0;
    std::uint64_t SlabHeapAllocations = 0;
};

// Runs on a thread that has never forked, so its tagged-queue cache is empty, with every
// worker held, so nothing dequeues. A wave of plain tasks first warms this thread's
// global-queue producer for the stubs. kSharedTaggedJobsSlots counters held together
// then empty the shared pool, so the measured counter constructs a fresh queue of one
// block, which the window fills to kTaggedBlocks.
TaggedQueueGrowth ForkPastTheTaggedQueueBlock(JobSystem::WorkStealingThreadPool& pool, WorkerGate& gate)
{
    constexpr std::size_t kWarmTasks = kTaggedTasks + kSharedTaggedJobsSlots + 1;
    std::atomic<std::size_t> warmed{0};
    gate.Hold();
    for (std::size_t i = 0; i < kWarmTasks; ++i)
    {
        pool.EnqueueWork([&warmed] { warmed.fetch_add(1, std::memory_order_acq_rel); });
    }
    gate.Release();
    YieldUntilCount(warmed, kWarmTasks);

    gate.Hold();
    std::array<JobSystem::JobCounter, kSharedTaggedJobsSlots> pooled;
    for (JobSystem::JobCounter& held : pooled)
    {
        pool.Run([] {}, held);
    }
    JobSystem::JobCounter counter;
    pool.Run([] {}, counter);

    TaggedQueueGrowth growth;
    const std::uint64_t slabHeapBefore = JobSystem::Detail::GetTaskSlabStatsForTests().HeapAllocs;
    {
        const Memory::AllocationCountScope scope(Memory::CountWindow::Process);
        for (std::size_t i = 1; i < kTaggedTasks; ++i)
        {
            pool.Run([] {}, counter);
        }
        growth.Allocations = scope.Count();
    }
    growth.SlabHeapAllocations = JobSystem::Detail::GetTaskSlabStatsForTests().HeapAllocs - slabHeapBefore;

    gate.Release();
    for (JobSystem::JobCounter& held : pooled)
    {
        pool.Wait(held);
    }
    pool.Wait(counter);
    return growth;
}

// JobCounter's tagged queue grows through the counter's allocations. Envelopes and stubs
// that miss the slab freelist go to the heap; the slab pool counts those, and they are
// subtracted. What remains is at least the tagged queue's blocks and its block-index growth;
// about 34 other allocations stay even with the queue on default traits, so the bound sits above them.
TEST(TaskQueueAllocation, JobCounterQueueGrowthIsSeenByTheCounter)
{
    JobSystem::WorkStealingThreadPool pool(2);
    WorkerGate gate(pool);

    TaggedQueueGrowth growth;
    std::thread([&pool, &gate, &growth] { growth = ForkPastTheTaggedQueueBlock(pool, gate); }).join();

    ASSERT_GE(growth.Allocations, growth.SlabHeapAllocations);
    const std::uint64_t queueAllocations = growth.Allocations - growth.SlabHeapAllocations;
    std::printf("[TaskQueueAllocation] tagged queue: %llu allocations (%llu counted, %llu slab heap)\n",
                static_cast<unsigned long long>(queueAllocations),
                static_cast<unsigned long long>(growth.Allocations),
                static_cast<unsigned long long>(growth.SlabHeapAllocations));
    EXPECT_GE(queueAllocations, kTaggedBlocks - 1);
}

// A graph task with no dependency, submitted on a worker, goes to that worker's local
// queue.
class CountedGraphTask : public JobSystem::Task
{
  public:
    CountedGraphTask(JobSystem::WorkStealingThreadPool* pool, std::atomic<std::size_t>* ran)
        : Task(pool), m_Ran(ran)
    {
    }

    void Execute() override { m_Ran->fetch_add(1, std::memory_order_acq_rel); }

  private:
    std::atomic<std::size_t>* m_Ran;
};

struct LocalWave
{
    std::uint64_t Allocations = 0;
    std::uint64_t LocalPushes = 0;
};

// The pool's only worker submits kLocalTasks graph tasks and holds until the window
// closes, so its local queue holds all of them and none runs inside the window.
LocalWave PublishOnTheWorker(JobSystem::WorkStealingThreadPool& pool)
{
    std::atomic<std::size_t> ran{0};
    std::atomic<bool> published{false};
    std::atomic<bool> release{false};
    const std::uint64_t localPushesBefore = pool.GetStatistics().LocalPushes;

    LocalWave wave;
    {
        const Memory::AllocationCountScope scope(Memory::CountWindow::Process);
        pool.EnqueueWork(
            [&pool, &ran, &published, &release]
            {
                for (std::size_t i = 0; i < kLocalTasks; ++i)
                {
                    pool.Submit(JobSystem::MakeUnique<CountedGraphTask>(&pool, &ran));
                }
                published.store(true, std::memory_order_release);
                YieldUntil(release);
            });
        YieldUntil(published);
        wave.Allocations = scope.Count();
    }
    release.store(true, std::memory_order_release);
    YieldUntilCount(ran, kLocalTasks);
    wave.LocalPushes = pool.GetStatistics().LocalPushes - localPushesBefore;
    return wave;
}

// A worker's local queue grows through the counter's allocations. A wave on another pool
// first fills the process-wide TaskData freelist. The measured pool's first wave then
// grows its worker's local queue, and an identical second wave reuses those blocks. The
// per-task allocations (the task, its TaskData, its registry and graph entries) cancel
// between the two waves, so the difference is the queue's growth plus the one-time
// growth of the pool's own containers.
TEST(TaskQueueAllocation, WorkerLocalQueueGrowthIsSeenByTheCounter)
{
    {
        JobSystem::WorkStealingThreadPool warm(1);
        PublishOnTheWorker(warm);
    }

    JobSystem::WorkStealingThreadPool pool(1);
    const LocalWave growing = PublishOnTheWorker(pool);
    const LocalWave reusing = PublishOnTheWorker(pool);

    std::printf("[TaskQueueAllocation] worker local queue: %llu allocations growing, %llu reusing\n",
                static_cast<unsigned long long>(growing.Allocations),
                static_cast<unsigned long long>(reusing.Allocations));
    ASSERT_EQ(growing.LocalPushes, kLocalTasks) << "the graph tasks did not reach the worker's local queue";
    ASSERT_EQ(reusing.LocalPushes, kLocalTasks) << "the graph tasks did not reach the worker's local queue";
    ASSERT_GE(growing.Allocations, reusing.Allocations);
    EXPECT_GE(growing.Allocations - reusing.Allocations, kLocalGrowthBlocks);
}

} // namespace
