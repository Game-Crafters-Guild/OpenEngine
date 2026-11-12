#include "AIECS/AiCommand.h"
#include "AIECS/AiProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/AiTestWorld.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "AIECS/Components/AiTrace.h"
#include "ECS/ECSTemplates.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/Interpolation.h"

#include <gtest/gtest.h>

#include <span>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr uint32 kAgentCount = 10000;
// At most AiTrace::kCapacity, so each trace holds every decision of the run.
constexpr uint32 kTickCount = 48;
// The first command a registry registers gets id 0.
constexpr uint16 kHungerCommand = 0;
// The first program a registry adds gets id 0.
constexpr uint16 kProgram = 0;

struct TestHunger
{
    float32 Value = 0.0f;
};

void ReadHunger(const ECS::World& world, std::span<const ECS::EntityHandle> agents, std::span<float32> out)
{
    for (size_t index = 0; index < agents.size(); ++index)
        out[index] = world.GetComponent<TestHunger>(agents[index])->Value;
}

// Adds Argument hundredths to the agent's hunger, kept in [0, 1].
void ApplyHunger(ECS::World& world, ECS::EntityHandle, const AIECS::AiCommand& command)
{
    float32& hunger = world.GetComponentForWrite<TestHunger>(command.Target)->Value;
    hunger = Math::Clamp01(hunger + static_cast<float32>(command.Argument) * 0.01f);
}

AIECS::AiTaskStatus TickEat(AIECS::AiTaskContext& context)
{
    context.IssueCommand({context.GetAgent(), -30, kHungerCommand});
    return AIECS::AiTaskStatus::Succeeded;
}

AIECS::AiTaskStatus TickWander(AIECS::AiTaskContext& context)
{
    context.IssueCommand({context.GetAgent(), 7, kHungerCommand});
    return AIECS::AiTaskStatus::Running;
}

AIECS::AiTaskStatus TickRest(AIECS::AiTaskContext& context)
{
    context.IssueCommand({context.GetAgent(), 3, kHungerCommand});
    return AIECS::AiTaskStatus::Succeeded;
}

// Eat scores the hunger; Wander and Rest score flat. The noise margin makes the seed choose
// among close options, the cooldown keeps Rest out for 4 ticks in cells 0 to 2.
void MakeProgram(AIECS::AiRegistry& registry)
{
    EXPECT_EQ(registry.RegisterCommand(&ApplyHunger), kHungerCommand);
    AIECS::AiProgram program;
    program.ColumnInputs = {registry.RegisterInput(&ReadHunger, AIECS::AiInputThreading::JobThreads)};
    program.Options.Curves.push_back(AI::ResponseCurve::FromPreset(AI::CurvePreset::Linear, 1.0f, 0.5f));
    program.Options.Considerations.push_back({0.0f, 1.0f, 0, 0});
    program.Options.Options.push_back({1.0f, 0, 1, registry.RegisterTask({nullptr, &TickEat, nullptr}), 0});
    program.Options.Options.push_back({0.45f, 0, 0, registry.RegisterTask({nullptr, &TickWander, nullptr}), 0});
    program.Options.Options.push_back({0.4f, 0, 0, registry.RegisterTask({nullptr, &TickRest, nullptr}), 0, 4});
    program.Options.NoiseMargin = 0.15f;
    program.Options.CommitmentMargin = 0.05f;
    program.CooldownCell = 0;
    EXPECT_EQ(registry.AddProgram(std::move(program)), kProgram);
}

// Creates the agents with hungers spread over [0, 1) and steps the world for kTickCount AI ticks.
std::vector<ECS::EntityHandle> CreateAndStep(AIECS::AiTestWorld& world)
{
    std::vector<ECS::EntityHandle> agents;
    agents.reserve(kAgentCount);
    for (uint32 index = 0; index < kAgentCount; ++index)
    {
        const float32 hunger = static_cast<float32>(index * 37 % 100) * 0.01f;
        Components::AiAgent agent;
        agent.Program = kProgram;
        agents.push_back(
            world.GetWorld().Create(agent, Components::AiCellsSmall{}, Components::AiTrace{}, TestHunger{hunger}).GetHandle());
    }
    world.Step(kTickCount);
    return agents;
}

// The number of agents whose traces differ between the two worlds.
uint32 CountDifferentTraces(AIECS::AiTestWorld& left, AIECS::AiTestWorld& right, std::span<const ECS::EntityHandle> agents)
{
    uint32 different = 0;
    for (const ECS::EntityHandle agent : agents)
    {
        const Components::AiTrace& leftTrace = *left.GetWorld().GetComponent<Components::AiTrace>(agent);
        const Components::AiTrace& rightTrace = *right.GetWorld().GetComponent<Components::AiTrace>(agent);
        EXPECT_EQ(leftTrace.Count, kTickCount);
        if (!AIECS::AiTestWorld::SameTrace(leftTrace, rightTrace))
            ++different;
    }
    return different;
}

} // namespace

// Two runs with one seed make the same decisions, tick by tick and byte for byte, whether the
// decisions run on the calling thread or on every worker; another seed makes other decisions.
TEST(AiDeterminismTests, OneSeedGivesTheSameDecisionsOnOneAndAllWorkers)
{
    AIECS::AiRegistry registry;
    MakeProgram(registry);
    JobSystem::WorkStealingThreadPool jobs;
    ASSERT_GT(jobs.GetWorkerCount(), 1u);

    AIECS::AiTestWorld serial(registry, 7);
    AIECS::AiTestWorld parallel(registry, 7, &jobs);
    AIECS::AiTestWorld otherSeed(registry, 8, &jobs);
    const std::vector<ECS::EntityHandle> agents = CreateAndStep(serial);
    ASSERT_EQ(CreateAndStep(parallel), agents);
    ASSERT_EQ(CreateAndStep(otherSeed), agents);

    EXPECT_EQ(CountDifferentTraces(serial, parallel, agents), 0u);
    EXPECT_GT(CountDifferentTraces(serial, otherSeed, agents), 0u);
}
