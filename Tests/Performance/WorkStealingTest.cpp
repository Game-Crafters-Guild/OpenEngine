#include <gtest/gtest.h>
#include "Logger/Logger.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "JobSystem/Types.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "../Jobs/LatencyBudget.h"
#include "../Jobs/ScopedLogLevel.h"

// Scheduling-contract tests for the post-overhaul pool (issue #242 re-baseline,
// slice 6). The pre-overhaul versions of these tests asserted per-worker work
// SPREAD (every thread does some work, load imbalance < 50%, linear scaling
// efficiency > 70%). That was never the pool's contract, and since slice 1/4
// it is explicitly NOT the behavior: producers issue a BOUNDED number of
// wakes (one idle worker per publish) and worker-side propagation only extends the
// wake front while a backlog remains, so for cheap tasks a few workers
// legitimately drain everything while the rest stay parked. That is the
// design (no thundering herd), not a defect — asserting spread rejected the
// overhaul's central scheduling decision.
//
// What IS the contract, and what these tests pin:
//   1. Every published task executes, promptly — bounded wake + propagation
//      must reach enough width that a burst drains in bounded wall time.
//   2. Throughput has a floor — the wake protocol must not strand a backlog
//      behind parked workers (that failure mode shows up as multi-ms backstop
//      rescues and craters tasks/sec by an order of magnitude).
//
// Budget basis (2026-07-10, slice-6 diet build, DebugFast, 32-thread desktop,
// idle-ish box): 50k mixed-weight Submits complete in ~130-180ms; 20k
// uniform-weight Submits in ~40-70ms end-to-end; per-width 10k-task storms
// 10-30ms. Budgets are set at >=3x the observed worst so scheduler
// regressions (stranded backlogs, lost wakeups) fail loudly while machine
// noise does not. Thresholds re-verified against the measured run recorded
// in the slice-6 PR.

namespace GameEngine::Performance {

namespace {

// An odd 64-bit multiplier (the MMIX linear congruential one).
constexpr std::uint64_t kTaskWorkMultiplier = 6364136223846793005ull;

// The work of one task in the uniform-weight tests.
constexpr size_t kUniformTaskSteps = 1000;

// One task's work: `steps` multiply-add steps on one value. Each step uses the result of the step before.
// In an optimized build, the value stays in a register, and the result is stored once, so the loop stays and its time
// does not change with the stack layout.
void RunTaskWork(size_t steps) {
    // A volatile read gives the start value and the step count at run time, so the compiler cannot fold the call.
    volatile size_t runTimeSteps = steps;
    size_t stepCount = runTimeSteps;
    std::uint64_t value = stepCount;
    for (size_t step = 0; step < stepCount; ++step) {
        value = value * kTaskWorkMultiplier + step;
    }
    volatile std::uint64_t result = value;
    (void)result;
}

} // namespace

class WorkStealingTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_Budget = GameEngine::Tests::ReadLatencyBudget();
    }

    // True when the machine load skips the budgets. A test checks it after its correctness checks.
    bool IsTooLoadedForBudgets() const { return m_Budget.Verdict == GameEngine::Tests::LatencyBudgetVerdict::Skip; }

    // Read in SetUp, before the test starts its pool.
    GameEngine::Tests::LatencyBudget m_Budget;

private:
    // The cases print their [bench] records with std::printf, so a run at this level still shows them.
    GameEngine::Tests::ScopedLogLevel m_LogLevel{Logger::LogLevel::Error};
};

// Mixed-weight burst: every task completes, and end-to-end throughput stays
// above a floor that a stranded backlog cannot meet. (Successor of the old
// "RingBasedWorkStealingEfficiency" — same workload, contract-shaped
// assertions.)
TEST_F(WorkStealingTest, MixedWeightBurstDrainsPromptly) {
    const size_t numThreads = std::thread::hardware_concurrency();
    const size_t numTasks = 50000;

    JobSystem::WorkStealingThreadPool pool(numThreads);

    std::vector<JobSystem::TaskHandle> handles;
    handles.reserve(numTasks);
    std::atomic<size_t> completedTasks{0};

    auto startTime = std::chrono::high_resolution_clock::now();

    // Variable work per task (every 10th is 100x heavier): the shape that
    // needs redistribution — heavy tasks pin some workers while the cheap
    // backlog must keep flowing through the rest.
    for (size_t i = 0; i < numTasks; ++i) {
        handles.push_back(pool.Submit([&completedTasks, i]() {
            size_t workAmount = (i % 10 == 0) ? 10000 : 100;
            RunTaskWork(workAmount);
            completedTasks.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& handle : handles) {
        handle.Wait();
    }

    const auto interval = std::chrono::high_resolution_clock::now() - startTime;
    auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(interval).count();

    const double elapsedMs = std::chrono::duration<double, std::milli>(interval).count();
    const double tasksPerSecond = numTasks * 1000.0 / std::max(elapsedMs, 1e-3);
    std::printf("[bench] MixedWeightBurst: %zu tasks in %.2fms (%.0f tasks/sec)\n", numTasks, elapsedMs,
                tasksPerSecond);

    EXPECT_EQ(completedTasks.load(), numTasks) << "every published task must execute";
    // Basis: measured ~130-180ms (280-380k tasks/sec). A stranded backlog
    // (lost wakeup, dead propagation chain) runs into 1ms-per-task backstop
    // territory and misses this floor by orders of magnitude.
    if (IsTooLoadedForBudgets()) {
        GTEST_SKIP() << m_Budget.Describe();
    }
    EXPECT_LT(static_cast<double>(durationMs), m_Budget.Scale(600.0))
        << "burst did not drain promptly - wake/propagation regression; " << m_Budget.Describe();
}

// Bounded-wake propagation: one external producer, a wide uniform burst.
// The producer issues at most a bounded number of kernel wakes per publish;
// worker-side propagation must still carry the wake front wide enough that
// the burst completes in bounded time. Deliberately NO per-worker spread
// assertion (see file header). (Successor of the old
// "LoadBalancingEffectiveness".)
TEST_F(WorkStealingTest, BoundedWakePropagationCompletesPromptly) {
    const size_t numThreads = std::thread::hardware_concurrency();
    const size_t numTasks = 20000;

    JobSystem::WorkStealingThreadPool pool(numThreads);

    std::atomic<size_t> completedTasks{0};
    std::vector<JobSystem::TaskHandle> handles;
    handles.reserve(numTasks);

    auto startTime = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < numTasks; ++i) {
        handles.push_back(pool.Submit([&completedTasks]() {
            RunTaskWork(kUniformTaskSteps);
            completedTasks.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& handle : handles) {
        handle.Wait();
    }

    const auto interval = std::chrono::high_resolution_clock::now() - startTime;
    auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(interval).count();

    const double elapsedMs = std::chrono::duration<double, std::milli>(interval).count();
    std::printf("[bench] BoundedWakePropagation: %zu tasks in %.2fms\n", numTasks, elapsedMs);

    EXPECT_EQ(completedTasks.load(), numTasks) << "every published task must execute";
    // Basis: measured ~40-70ms. See file header for the x3+ budget rationale.
    // The queues must be fully drained once every handle returned.
    EXPECT_EQ(pool.GetApproximateQueueSize(), 0u);
    if (IsTooLoadedForBudgets()) {
        GTEST_SKIP() << m_Budget.Describe();
    }
    EXPECT_LT(static_cast<double>(durationMs), m_Budget.Scale(300.0))
        << "propagation stalled - backlog waited on the backstop; " << m_Budget.Describe();
}

// Per-width promptness: the pool completes a fixed storm within budget at
// every worker count. No scaling-efficiency assertion: 10k trivial Submits
// are publication-bound, not execution-bound, so added workers do not (and
// must not be expected to) produce linear speedups — the old >70% efficiency
// gate asserted exactly that and always failed post-slice-1. (Successor of
// the old "ScalabilityComparison".)
TEST_F(WorkStealingTest, StormCompletesWithinBudgetAtEveryWidth) {
    const size_t numTasks = 10000;
    std::vector<size_t> threadCounts = {1, 2, 4, 8, std::thread::hardware_concurrency()};

    for (size_t numThreads : threadCounts) {
        if (numThreads == 0 || numThreads > std::thread::hardware_concurrency()) {
            continue;
        }

        JobSystem::WorkStealingThreadPool pool(numThreads);
        std::atomic<size_t> completedTasks{0};

        auto startTime = std::chrono::high_resolution_clock::now();

        std::vector<JobSystem::TaskHandle> handles;
        handles.reserve(numTasks);
        for (size_t i = 0; i < numTasks; ++i) {
            handles.push_back(pool.Submit([&completedTasks]() {
                RunTaskWork(kUniformTaskSteps);
                completedTasks.fetch_add(1, std::memory_order_relaxed);
            }));
        }

        for (auto& handle : handles) {
            handle.Wait();
        }

        const auto interval = std::chrono::high_resolution_clock::now() - startTime;
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(interval).count();

        const double elapsedMs = std::chrono::duration<double, std::milli>(interval).count();
        std::printf("[bench] Storm width %zu: %zu tasks in %.2fms\n", numThreads, numTasks, elapsedMs);

        EXPECT_EQ(completedTasks.load(), numTasks)
            << "width " << numThreads << ": every published task must execute";
        // Basis: measured 10-30ms per width. Budget x3+ (see file header).
        if (!IsTooLoadedForBudgets()) {
            EXPECT_LT(static_cast<double>(durationMs), m_Budget.Scale(150.0))
                << "width " << numThreads << ": storm exceeded budget - scheduling regression; " << m_Budget.Describe();
        }
    }
    if (IsTooLoadedForBudgets()) {
        GTEST_SKIP() << m_Budget.Describe();
    }
}

} // namespace GameEngine::Performance
