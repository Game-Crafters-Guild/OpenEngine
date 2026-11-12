// The world's SimulationClock turns frame deltas into whole fixed steps, and
// SystemManager runs a Simulation system once per step and a Frame system once
// per frame with the delta of its SystemTime.

#include <gtest/gtest.h>

#include "ECS/SimulationClock.h"
#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "ECS/World.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::ECS;

namespace
{
constexpr float64 kStep = SimulationClock::kDefaultFixedDeltaSeconds;

struct RunRecord
{
    std::string Id;
    float32 DeltaTime = 0.0f;
    uint64 Tick = 0;
};

class FrameRecordingSystem : public ISystem
{
public:
    FrameRecordingSystem(std::vector<RunRecord>* log, const char* id) : m_Log(log), m_Id(id) {}
    void Update(World& world, float32 deltaTime) override
    {
        m_Log->push_back({m_Id, deltaTime, world.GetSimulationClock().GetTick()});
    }
    const char* GetName() const override { return m_Id.c_str(); }

private:
    std::vector<RunRecord>* m_Log;
    std::string m_Id;
};

class SimulationRecordingSystem : public FrameRecordingSystem
{
public:
    static constexpr SystemCadence Cadence = SystemCadence::Simulation;
    using FrameRecordingSystem::FrameRecordingSystem;
};

class RealRecordingSystem : public FrameRecordingSystem
{
public:
    static constexpr SystemTime Time = SystemTime::Real;
    using FrameRecordingSystem::FrameRecordingSystem;
};

int64_t Count(const std::vector<RunRecord>& log, const std::string& id)
{
    return std::count_if(log.begin(), log.end(), [&id](const RunRecord& record) { return record.Id == id; });
}

size_t FirstIndexOf(const std::vector<RunRecord>& log, const std::string& id)
{
    for (size_t i = 0; i < log.size(); ++i)
        if (log[i].Id == id)
            return i;
    return log.size();
}

size_t LastIndexOf(const std::vector<RunRecord>& log, const std::string& id)
{
    for (size_t i = log.size(); i > 0; --i)
        if (log[i - 1].Id == id)
            return i - 1;
    return log.size();
}
} // namespace

TEST(SimulationClock, StepsAreTheWholeStepsTheFrameDeltasHold)
{
    SimulationClock clock;
    clock.Advance(kStep);
    EXPECT_EQ(clock.GetStepsThisFrame(), 1u);
    clock.Advance(kStep * 0.5);
    EXPECT_EQ(clock.GetStepsThisFrame(), 0u);
    EXPECT_NEAR(clock.GetAlpha(), 0.5f, 1.0e-6f);
    clock.Advance(kStep * 0.5);
    EXPECT_EQ(clock.GetStepsThisFrame(), 1u);
    EXPECT_NEAR(clock.GetAlpha(), 0.0f, 1.0e-6f);
    clock.Advance(kStep * 2.5);
    EXPECT_EQ(clock.GetStepsThisFrame(), 2u);
    EXPECT_NEAR(clock.GetAlpha(), 0.5f, 1.0e-6f);
}

TEST(SimulationClock, FloatFrameDeltaJustUnderWholeStepsRunsTheWholeSteps)
{
    // A scene stores the step as the float 1.0f / 60.0f. Three such steps are a
    // little more than the float frame delta 3.0f / 60.0f.
    SimulationClock clock;
    clock.SetFixedDeltaSeconds(1.0f / 60.0f);
    clock.Advance(static_cast<float64>(3.0f / 60.0f));
    EXPECT_EQ(clock.GetStepsThisFrame(), 3u);
    EXPECT_EQ(clock.GetAlpha(), 0.0f);
}

TEST(SimulationClock, LongFrameAddsAtMostAQuarterSecond)
{
    SimulationClock clock;
    clock.SetMaxStepsPerFrame(100);
    clock.Advance(1.0);
    EXPECT_EQ(clock.GetStepsThisFrame(), 15u); // 0.25 s at 60 steps per second
}

TEST(SimulationClock, HitchRunsAtMostTheStepCapAndLeavesNoBacklog)
{
    SimulationClock clock;
    clock.Advance(SimulationClock::kMaxFrameDeltaSeconds);
    std::vector<uint32> steps{clock.GetStepsThisFrame()};
    for (int frame = 0; frame < 3; ++frame)
    {
        clock.Advance(kStep);
        steps.push_back(clock.GetStepsThisFrame());
    }
    EXPECT_EQ(steps, (std::vector<uint32>{4u, 1u, 1u, 1u}));
}

TEST(SimulationClock, SustainedSlowFramesKeepNoBacklog)
{
    SimulationClock clock;
    for (int frame = 0; frame < 600; ++frame) // 60 s at 10 fps
        clock.Advance(0.1);
    for (int frame = 0; frame < 10; ++frame)
    {
        clock.Advance(kStep);
        EXPECT_EQ(clock.GetStepsThisFrame(), 1u) << "frame " << frame << " at 60 fps";
    }
}

TEST(SimulationClock, StoppedClockAfterAHitchRunsNoStep)
{
    SimulationClock clock;
    clock.Advance(SimulationClock::kMaxFrameDeltaSeconds);
    clock.SetTimeScale(0.0);
    clock.Advance(kStep);
    EXPECT_EQ(clock.GetStepsThisFrame(), 0u);
}

TEST(SimulationClock, BeginStepCountsTheTickAndTheStepInFrame)
{
    SimulationClock clock;
    clock.Advance(kStep * 2.0);
    EXPECT_EQ(clock.GetStepInFrame(), 0u);
    ASSERT_TRUE(clock.BeginStep());
    EXPECT_EQ(clock.GetStepInFrame(), 1u);
    EXPECT_EQ(clock.GetTick(), 1u);
    ASSERT_TRUE(clock.BeginStep());
    EXPECT_EQ(clock.GetStepInFrame(), 2u);
    EXPECT_EQ(clock.GetTick(), 2u);
    EXPECT_FALSE(clock.BeginStep());
    EXPECT_EQ(clock.GetTick(), 2u);
}

TEST(SimulationClock, RestoreSetsTheTickAndEndsTheStepsOfTheFrame)
{
    SimulationClock clock;
    clock.Advance(kStep * 2.0);
    ASSERT_TRUE(clock.BeginStep());
    clock.Restore(5000);
    EXPECT_EQ(clock.GetTick(), 5000u);
    EXPECT_FALSE(clock.BeginStep());
    clock.Advance(kStep);
    ASSERT_TRUE(clock.BeginStep());
    EXPECT_EQ(clock.GetTick(), 5001u);
}

TEST(SimulationClock, TimeScaleZeroRunsNoStepAndKeepsTheAlpha)
{
    SimulationClock clock;
    clock.Advance(kStep * 1.5);
    clock.SetTimeScale(0.0);
    clock.Advance(kStep * 3.0);
    EXPECT_EQ(clock.GetStepsThisFrame(), 0u);
    EXPECT_NEAR(clock.GetAlpha(), 0.5f, 1.0e-6f);
    clock.SetTimeScale(1.0);
    clock.Advance(kStep * 0.5);
    EXPECT_EQ(clock.GetStepsThisFrame(), 1u);
}

TEST(SimulationClock, TimeScaleMultipliesTheFrameDelta)
{
    SimulationClock clock;
    clock.SetTimeScale(2.0);
    clock.Advance(kStep);
    EXPECT_EQ(clock.GetStepsThisFrame(), 2u);
}

TEST(SimulationClock, SameFrameDeltasGiveTheSameTicks)
{
    const std::vector<float64> deltas{0.016, 0.0331, 0.0072, 0.3, 0.0, 0.0169, 0.05, 0.001};
    SimulationClock a;
    SimulationClock b;
    for (const float64 delta : deltas)
    {
        a.Advance(delta);
        b.Advance(delta);
        while (a.BeginStep())
            ASSERT_TRUE(b.BeginStep());
        EXPECT_FALSE(b.BeginStep());
        EXPECT_EQ(a.GetTick(), b.GetTick());
        EXPECT_EQ(a.GetAlpha(), b.GetAlpha());
    }
    EXPECT_GT(a.GetTick(), 0u);
}

// The simulation block runs once per clock step with the fixed delta, between
// the Frame system it depends on and the Frame system that depends on it.
TEST(SimulationCadence, PlannedSimulationSystemRunsOncePerStep)
{
    World world(nullptr);
    SystemManager manager; // no job system: waves run inline, so the order is observable
    std::vector<RunRecord> log;
    SystemScheduleBuilder builder;
    builder.Add<FrameRecordingSystem>("Input", SystemPhase::Early, 0, {}, &log, "Input");
    builder.Add<SimulationRecordingSystem>("Step", SystemPhase::Early, 0, {"Input"}, &log, "Step");
    builder.Add<FrameRecordingSystem>("Writeback", SystemPhase::Early, 0, {"Step"}, &log, "Writeback");
    builder.BuildAndRegisterWithWaves(manager);

    manager.Update(world, 3.0f / 60.0f);
    EXPECT_EQ(Count(log, "Step"), 3);
    EXPECT_EQ(Count(log, "Input"), 1);
    EXPECT_EQ(Count(log, "Writeback"), 1);
    EXPECT_LT(FirstIndexOf(log, "Input"), FirstIndexOf(log, "Step"));
    EXPECT_LT(LastIndexOf(log, "Step"), FirstIndexOf(log, "Writeback"));
    for (const RunRecord& run : log)
        EXPECT_FLOAT_EQ(run.DeltaTime, run.Id == "Step" ? static_cast<float32>(kStep) : 3.0f / 60.0f);

    log.clear();
    manager.Update(world, 1.0f / 120.0f);
    EXPECT_EQ(Count(log, "Step"), 0);
    EXPECT_EQ(Count(log, "Input"), 1);
    EXPECT_EQ(Count(log, "Writeback"), 1);
}

// A Frame system between two Simulation systems splits them into two runs. Each run
// steps the clock's steps of the frame, so the second run sees the same ticks.
TEST(SimulationCadence, SimulationFrameSimulationChainRunsInDependencyOrder)
{
    World world(nullptr);
    SystemManager manager;
    std::vector<RunRecord> log;
    SystemScheduleBuilder builder;
    builder.Add<SimulationRecordingSystem>("Step", SystemPhase::Early, 0, {}, &log, "Step");
    builder.Add<FrameRecordingSystem>("Writeback", SystemPhase::Early, 0, {"Step"}, &log, "Writeback");
    builder.Add<SimulationRecordingSystem>("Think", SystemPhase::Early, 0, {"Writeback"}, &log, "Think");
    builder.BuildAndRegisterWithWaves(manager);

    manager.Update(world, 2.0f / 60.0f);
    ASSERT_EQ(Count(log, "Step"), 2);
    ASSERT_EQ(Count(log, "Writeback"), 1);
    ASSERT_EQ(Count(log, "Think"), 2);
    EXPECT_LT(LastIndexOf(log, "Step"), FirstIndexOf(log, "Writeback"));
    EXPECT_LT(FirstIndexOf(log, "Writeback"), FirstIndexOf(log, "Think"));
    std::vector<uint64> stepTicks;
    std::vector<uint64> thinkTicks;
    for (const RunRecord& run : log)
    {
        if (run.Id == "Step")
            stepTicks.push_back(run.Tick);
        if (run.Id == "Think")
            thinkTicks.push_back(run.Tick);
    }
    EXPECT_EQ(stepTicks, (std::vector<uint64>{1u, 2u}));
    EXPECT_EQ(thinkTicks, stepTicks);
    EXPECT_EQ(world.GetSimulationClock().GetTick(), 2u);
}

// A wave inserted into a built plan, as the bench scene inserts one at the front, runs once per
// frame, and the Simulation waves after it still run once per step.
TEST(SimulationCadence, WaveInsertedIntoAPlanRunsOncePerFrame)
{
    World world(nullptr);
    SystemManager manager;
    std::vector<RunRecord> log;
    manager.AddSystem<SimulationRecordingSystem>(&log, "Step");
    manager.AddSystem<FrameRecordingSystem>(&log, "Writeback");
    manager.AddSystem<FrameRecordingSystem>(&log, "Inserted");
    SystemExecutionPlan plan;
    plan.Waves.push_back({{0}, SystemCadence::Simulation});
    plan.Waves.push_back({{1}});
    SystemExecutionPlan::Wave inserted;
    inserted.SystemIndices.push_back(2);
    plan.Waves.insert(plan.Waves.begin(), inserted);
    manager.SetExecutionPlan(std::move(plan));

    manager.Update(world, 2.0f / 60.0f);
    EXPECT_EQ(Count(log, "Inserted"), 1);
    EXPECT_EQ(Count(log, "Step"), 2);
    EXPECT_EQ(Count(log, "Writeback"), 1);
    EXPECT_EQ(FirstIndexOf(log, "Inserted"), 0u);
}

TEST(SimulationCadence, UnplannedSimulationSystemRunsOncePerStep)
{
    World world(nullptr);
    SystemManager manager;
    std::vector<RunRecord> log;
    manager.AddSystem<SimulationRecordingSystem>(&log, "Step");
    manager.AddSystem<FrameRecordingSystem>(&log, "Frame");

    manager.Update(world, 2.0f / 60.0f);
    EXPECT_EQ(Count(log, "Step"), 2);
    EXPECT_EQ(Count(log, "Frame"), 1);
}

// A Gameplay Frame system gets the clock's frame seconds: 0 while paused, at most a quarter second
// after a hitch. A Real Frame system gets the frame delta.
TEST(SystemTime, GameplaySystemGetsTheClockFrameSecondsAndRealSystemTheFrameDelta)
{
    World world(nullptr);
    SystemManager manager;
    std::vector<RunRecord> log;
    SystemScheduleBuilder builder;
    builder.Add<FrameRecordingSystem>("Gameplay", SystemPhase::Early, 0, {}, &log, "Gameplay");
    builder.Add<RealRecordingSystem>("Real", SystemPhase::Early, 0, {}, &log, "Real");
    builder.BuildAndRegisterWithWaves(manager);

    world.GetSimulationClock().SetTimeScale(0.0);
    manager.Update(world, 0.1f);
    ASSERT_EQ(log.size(), 2u);
    for (const RunRecord& run : log)
        EXPECT_FLOAT_EQ(run.DeltaTime, run.Id == "Gameplay" ? 0.0f : 0.1f) << run.Id << " while paused";

    log.clear();
    world.GetSimulationClock().SetTimeScale(1.0);
    manager.Update(world, 1.0f);
    ASSERT_EQ(log.size(), 2u);
    for (const RunRecord& run : log)
        EXPECT_FLOAT_EQ(run.DeltaTime, run.Id == "Gameplay" ? 0.25f : 1.0f) << run.Id << " after a hitch";
}
