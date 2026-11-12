#include "AIECS/AiRegistry.h"
#include "ECS/SystemScheduling.h"
#include "ECS/SystemWaveTrace.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr size_t kWaveNotFound = SIZE_MAX;

size_t FindWaveOf(const ECS::SystemManager& manager, std::string_view name)
{
    const auto& plan = manager.GetExecutionPlan();
    for (size_t wave = 0; wave < plan.Waves.size(); ++wave)
    {
        for (size_t index : plan.Waves[wave].SystemIndices)
        {
            const char* systemName = manager.GetSequentialSystemName(index);
            if (systemName != nullptr && name == systemName)
                return wave;
        }
    }
    return kWaveNotFound;
}

// A Simulation system that does nothing, placed in the wave of the AI system under test.
class SimulationProbe : public ECS::ISystem
{
public:
    static constexpr ECS::SystemCadence Cadence = ECS::SystemCadence::Simulation;

    void Update(ECS::World&, float32) override {}
    const char* GetName() const override { return "SimulationProbe"; }
};

// Puts a probe in the wave of the named AI system, runs one step on job threads and expects the
// AI system to run on the calling thread with nothing beside it.
void ExpectRunsAloneOnTheCallingThread(std::string_view system, int probeOrder,
                                       std::vector<ECS::SystemDependency> probeDependencies)
{
    JobSystem::WorkStealingThreadPool jobs(2);
    AIECS::AiRegistry registry;
    ECS::SystemScheduleBuilder builder;
    registry.AddSystemsToSchedule(builder);
    builder.Add<SimulationProbe>("SimulationProbe", ECS::SystemPhase::Early, probeOrder,
                                 std::move(probeDependencies));
    ECS::SystemManager manager(&jobs);
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Complete);
    ASSERT_EQ(FindWaveOf(manager, "SimulationProbe"), FindWaveOf(manager, system));

    ECS::World world;
    manager.GetWaveTrace().SetEnabled(true);
    manager.Update(world, static_cast<float32>(ECS::SimulationClock::kDefaultFixedDeltaSeconds));

    std::vector<ECS::SystemWaveTrace::Frame> frames;
    manager.GetWaveTrace().CopyFrames(frames, 1);
    ASSERT_EQ(frames.size(), 1u);
    const ECS::SystemWaveTrace::Frame& frame = frames.front();
    bool found = false;
    for (uint16 index = 0; index < frame.SystemCount; ++index)
    {
        const ECS::SystemWaveTrace::SystemSample& sample = frame.Systems[index];
        if (std::string_view(manager.GetSequentialSystemName(sample.Slot)) != system)
            continue;
        found = true;
        EXPECT_EQ(sample.PublishNs, 0u);
        EXPECT_EQ(sample.Worker, ECS::SystemWaveTrace::kCallerThread);
    }
    EXPECT_TRUE(found);
}

} // namespace

// Each AI system reads what the one before it wrote in the same step, so each runs in a later
// wave than the one before it, and every one runs once per simulation step.
TEST(AiScheduleTests, SystemsRunInOrderOnTheSimulationCadence)
{
    AIECS::AiRegistry registry;
    ECS::SystemScheduleBuilder builder;
    registry.AddSystemsToSchedule(builder);
    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Complete);

    constexpr std::array<std::string_view, 4> kOrder{"AiTickSystem", "AiKnowledgeSystem", "AiDecideSystem",
                                                     "AiApplySystem"};
    const auto& plan = manager.GetExecutionPlan();
    size_t previousWave = kWaveNotFound;
    for (std::string_view name : kOrder)
    {
        const size_t wave = FindWaveOf(manager, name);
        ASSERT_NE(wave, kWaveNotFound) << name;
        EXPECT_EQ(plan.Waves[wave].Cadence, ECS::SystemCadence::Simulation) << name;
        if (previousWave != kWaveNotFound)
            EXPECT_GT(wave, previousWave) << name;
        previousWave = wave;
    }
}

// A sensor takes World&, so AiKnowledgeSystem runs alone on the calling thread, even when
// another system shares its wave and the waves run on job threads.
TEST(AiScheduleTests, KnowledgeSystemRunsAloneOnTheCallingThread)
{
    ExpectRunsAloneOnTheCallingThread("AiKnowledgeSystem", 12, {"AiTickSystem"});
}

// World::Clear removes every singleton, so AiTickSystem creates the clock when it is missing; a
// singleton write may rehash the map that another system reads, so the tick system runs alone.
TEST(AiScheduleTests, TickSystemRunsAloneOnTheCallingThread)
{
    ExpectRunsAloneOnTheCallingThread("AiTickSystem", 10, {});
}

// A job-thread input or a task reads the world while the decide system runs, so no other system
// runs beside it.
TEST(AiScheduleTests, DecideSystemRunsAloneOnTheCallingThread)
{
    ExpectRunsAloneOnTheCallingThread("AiDecideSystem", 14, {"AiKnowledgeSystem"});
}
