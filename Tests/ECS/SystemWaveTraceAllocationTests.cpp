// The wave runner and the wave trace allocate nothing per frame once warm: an
// untraced call of a forking wave makes no allocation, and with tracing on a
// frame of a forking plan makes exactly as many allocations as with tracing off.
// Its own target because the window is process-wide: forked bodies run on
// workers and on the caller, which runs its own forks' jobs while it waits.

#include <gtest/gtest.h>

#include "ECS/SystemWaveTrace.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Memory/AllocationCountScope.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

using namespace GameEngine;
using namespace GameEngine::ECS;

namespace
{
class EmptySystem : public ISystem
{
public:
    void Update(World&, float32) override {}
    const char* GetName() const override { return "Empty"; }
};

// A body with enough work that the wave's other forks reach the workers.
class SpinSystem : public ISystem
{
public:
    void Update(World&, float32) override
    {
        std::uint64_t value = 1;
        for (int i = 0; i < 2000; ++i)
            value = value * 6364136223846793005ull + 1442695040888963407ull;
        g_SpinSink.fetch_add(value & 1u, std::memory_order_relaxed);
    }
    const char* GetName() const override { return "Spin"; }

    static inline std::atomic<std::uint64_t> g_SpinSink{0};
};

class ExclusiveSystem : public EmptySystem
{
public:
    bool RequiresExclusiveUpdate() const override { return true; }
};

constexpr int kWarmFrames = 200;
constexpr int kMeasuredFrames = 100;

// Three forked waves, an inline wave and a wave an exclusive system splits.
void BuildPlan(SystemManager& manager)
{
    for (int i = 0; i < 9; ++i)
        manager.AddSystem<EmptySystem>();
    manager.AddSystem<ExclusiveSystem>();
    SystemExecutionPlan plan;
    plan.Waves.push_back({{0, 1, 2}});
    plan.Waves.push_back({{3, 4}});
    plan.Waves.push_back({{5}});
    plan.Waves.push_back({{9, 6, 7}});
    plan.Waves.push_back({{8}});
    manager.SetExecutionPlan(std::move(plan));
}

std::uint64_t CountFrameAllocations(SystemManager& manager, World& world)
{
    for (int i = 0; i < kWarmFrames; ++i)
        manager.Update(world, 0.016f);
    Memory::AllocationCountScope scope(Memory::CountWindow::Process);
    for (int i = 0; i < kMeasuredFrames; ++i)
        manager.Update(world, 0.016f);
    return scope.Count();
}
} // namespace

TEST(SystemWaveTraceAllocation, TracingAddsNoAllocationsPerFrame)
{
    JobSystem::WorkStealingThreadPool pool(2);
    World world(nullptr);
    SystemManager manager(&pool);
    BuildPlan(manager);

    const std::uint64_t untraced = CountFrameAllocations(manager, world);
    manager.GetWaveTrace().SetEnabled(true);
    const std::uint64_t traced = CountFrameAllocations(manager, world);
    std::printf("allocations over %d frames: untraced %llu, traced %llu\n", kMeasuredFrames,
                static_cast<unsigned long long>(untraced), static_cast<unsigned long long>(traced));
    EXPECT_EQ(traced, untraced);
    EXPECT_GE(manager.GetWaveTrace().GetFramesRecorded(), static_cast<std::uint64_t>(kMeasuredFrames));
}

// One wave of eight forked systems on an eight-worker pool: after 100 warm-up
// calls, a call allocates nothing on any thread.
TEST(SystemWaveTraceAllocation, WaveUpdateAllocatesNothingAfterWarmup)
{
    constexpr int kWarmupCalls = 100;
    constexpr int kMeasuredCalls = 100;
    JobSystem::WorkStealingThreadPool pool(8);
    World world(&pool);
    SystemManager manager(&pool);
    SystemExecutionPlan plan;
    SystemExecutionPlan::Wave wave;
    for (size_t i = 0; i < 8; ++i)
    {
        manager.AddSystem<SpinSystem>();
        wave.SystemIndices.push_back(i);
    }
    plan.Waves.push_back(wave);
    manager.SetExecutionPlan(std::move(plan));

    for (int i = 0; i < kWarmupCalls; ++i)
        manager.Update(world, 0.016f);
    Memory::AllocationCountScope scope(Memory::CountWindow::Process);
    for (int i = 0; i < kMeasuredCalls; ++i)
        manager.Update(world, 0.016f);
    const std::uint64_t allocations = scope.Count();
    std::printf("allocations over %d one-wave calls of eight systems: %llu\n", kMeasuredCalls,
                static_cast<unsigned long long>(allocations));
    EXPECT_EQ(allocations, 0u);
}
