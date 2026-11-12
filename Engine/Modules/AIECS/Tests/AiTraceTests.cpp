#include "AIECS/AiProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "AIECS/Components/AiTrace.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "ECS/World.h"

#include <gtest/gtest.h>

#include <utility>

using namespace GameEngine;

namespace
{

constexpr float32 kOneAiTickSeconds = static_cast<float32>(3.0 * ECS::SimulationClock::kDefaultFixedDeltaSeconds);
constexpr int32 kOrderArgument = 7;

// The first command a registry registers gets id 0.
constexpr uint16 kOrderType = 0;

void IgnoreCommand(ECS::World&, ECS::EntityHandle, const AIECS::AiCommand&)
{
}

// Orders the agent to itself and succeeds at once.
AIECS::AiTaskStatus TickOrder(AIECS::AiTaskContext& context)
{
    context.IssueCommand({context.GetAgent(), kOrderArgument, kOrderType});
    return AIECS::AiTaskStatus::Succeeded;
}

AIECS::AiTaskStatus TickIdle(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Running;
}

// Option 0 (weight 1) orders once and cools down for 100 ticks in cell 0; option 1 idles at 0.5.
uint16 MakeOrderProgram(AIECS::AiRegistry& registry)
{
    EXPECT_EQ(registry.RegisterCommand(&IgnoreCommand), kOrderType);
    AIECS::AiProgram program;
    program.Options.Options.push_back({1.0f, 0, 0, registry.RegisterTask({nullptr, &TickOrder, nullptr}), 0, 100});
    program.Options.Options.push_back({0.5f, 0, 0, registry.RegisterTask({nullptr, &TickIdle, nullptr}), 0});
    program.CooldownCell = 0;
    return registry.AddProgram(std::move(program));
}

class AiTraceTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Components::AiAgent agent;
        agent.Program = MakeOrderProgram(m_Registry);
        m_Traced = m_World.Create(agent, Components::AiCellsSmall{}, Components::AiTrace{}).GetHandle();
        ECS::SystemScheduleBuilder builder;
        m_Registry.AddSystemsToSchedule(builder);
        builder.BuildAndRegisterWithWaves(m_Manager, ECS::ScheduleCompleteness::Complete);
    }

    AIECS::AiRegistry m_Registry;
    ECS::World m_World;
    ECS::SystemManager m_Manager;
    ECS::EntityHandle m_Traced;
};

} // namespace

// Each decision writes one record: the option before and after, the score, the task's outcome
// and its orders.
TEST_F(AiTraceTests, EachDecisionWritesOneRecord)
{
    m_Manager.Update(m_World, kOneAiTickSeconds);
    m_Manager.Update(m_World, kOneAiTickSeconds);

    const Components::AiTrace& trace = *m_World.GetComponent<Components::AiTrace>(m_Traced);
    ASSERT_EQ(trace.Count, 2u);
    const AIECS::AiTraceRecord& first = trace.Records[0];
    EXPECT_EQ(first.Previous, AIECS::AiTraceRecord::kNoOption);
    EXPECT_EQ(first.Chosen, 0);
    EXPECT_EQ(first.Score, 1.0f);
    EXPECT_EQ(first.EnteredTick, first.Tick);
    EXPECT_EQ(first.Outcome, AIECS::AiTraceOutcome::Succeeded);
    ASSERT_EQ(first.CommandCount, 1);
    EXPECT_EQ(first.CommandTargets[0], m_Traced);
    EXPECT_EQ(first.CommandArguments[0], kOrderArgument);
    EXPECT_EQ(first.CommandTypes[0], kOrderType);

    const AIECS::AiTraceRecord& second = trace.Records[1];
    EXPECT_EQ(second.Tick, first.Tick + 1);
    EXPECT_EQ(second.Previous, AIECS::AiTraceRecord::kNoOption);
    EXPECT_EQ(second.Chosen, 1);
    EXPECT_EQ(second.Score, 0.5f);
    EXPECT_EQ(second.Outcome, AIECS::AiTraceOutcome::Running);
    EXPECT_EQ(second.CommandCount, 0);
}

// The ring keeps the last kCapacity decisions: the next record replaces the oldest.
TEST_F(AiTraceTests, TraceKeepsTheLastDecisions)
{
    for (uint32 step = 0; step <= Components::AiTrace::kCapacity; ++step)
        m_Manager.Update(m_World, kOneAiTickSeconds);

    const Components::AiTrace& trace = *m_World.GetComponent<Components::AiTrace>(m_Traced);
    ASSERT_EQ(trace.Count, Components::AiTrace::kCapacity + 1);
    EXPECT_EQ(trace.Records[0].Tick, trace.Records[Components::AiTrace::kCapacity - 1].Tick + 1);
    EXPECT_EQ(trace.Records[0].Previous, 1);
    EXPECT_EQ(trace.Records[1].Tick, trace.Records[2].Tick - 1);
}
