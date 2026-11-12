// get_ai_trace's reply (BuildAiTraceReport): the fields an agent reads from a traced agent's decisions.

#include <gtest/gtest.h>

#include "AIECS/AiCommand.h"
#include "AIECS/AiProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/AiTestWorld.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiPathTrace.h"
#include "AIECS/Components/AiTrace.h"
#include "DebugServer/AI/AiDebugHandlers.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

namespace
{

using namespace GameEngine;

uint16 g_MarkCommand = 0;
constexpr int32 kMarkArgument = 7;

AIECS::AiTaskStatus TickMark(AIECS::AiTaskContext& context)
{
    context.IssueCommand({context.GetAgent(), kMarkArgument, g_MarkCommand});
    return AIECS::AiTaskStatus::Running;
}

void ApplyNothing(ECS::World&, ECS::EntityHandle, const AIECS::AiCommand&)
{
}

} // namespace

// Three decisions of one agent; the report keeps the newest two, oldest first, with each field set.
TEST(AiTraceReportTests, ReportsTheNewestDecisionsOfATracedAgent)
{
    AIECS::AiRegistry registry;
    g_MarkCommand = registry.RegisterCommand(&ApplyNothing);
    AIECS::AiProgram program;
    program.Options.Options.push_back({.Action = registry.RegisterTask({nullptr, &TickMark, nullptr})});
    const uint16 programId = registry.AddProgram(std::move(program));
    AIECS::AiTestWorld test(registry, 1);
    Components::AiAgent agent;
    agent.Program = programId;
    agent.Cadence = 1;
    const ECS::EntityHandle entity = test.GetWorld().Create(agent, Components::AiTrace{}).GetHandle();

    test.Step(3);
    const nlohmann::json report =
        Editor::BuildAiTraceReport(*test.GetWorld().GetComponent<Components::AiTrace>(entity), 2);

    EXPECT_EQ(report["recorded"], 3u);
    ASSERT_EQ(report["records"].size(), 2u);
    const nlohmann::json& older = report["records"][0];
    const nlohmann::json& newer = report["records"][1];
    EXPECT_EQ(newer["tick"].get<uint32>(), older["tick"].get<uint32>() + 1);
    EXPECT_EQ(newer["program"], programId);
    EXPECT_EQ(newer["previous"], 0);
    EXPECT_EQ(newer["chosen"], 0);
    EXPECT_EQ(newer["enteredTick"], older["enteredTick"]);
    EXPECT_EQ(newer["outcome"], "running");
    EXPECT_EQ(newer["commandCount"], 1);
    ASSERT_EQ(newer["commands"].size(), 1u);
    EXPECT_EQ(newer["commands"][0]["type"], g_MarkCommand);
    EXPECT_EQ(newer["commands"][0]["targetEntityId"], entity.id);
    EXPECT_EQ(newer["commands"][0]["argument"], kMarkArgument);
}

// A state-tree agent's path records: the report keeps the newest two, oldest first, with the path's states.
TEST(AiTraceReportTests, ReportsTheNewestPathsOfATracedStateTreeAgent)
{
    Components::AiPathTrace trace;
    for (uint32 tick = 10; tick < 13; ++tick)
    {
        AIECS::AiPathTraceRecord& record = trace.Records[trace.Count++];
        record.Tick = tick;
        record.Program = 0x8001;
        record.Depth = 2;
        record.States[0] = 0;
        record.States[1] = static_cast<uint16>(tick - 9);
    }
    trace.Records[2].Result = AI::StateTreeTaskStatus::Succeeded;

    const nlohmann::json report = Editor::BuildAiPathTraceReport(trace, 2);

    EXPECT_EQ(report["pathRecorded"], 3u);
    ASSERT_EQ(report["pathRecords"].size(), 2u);
    const nlohmann::json& older = report["pathRecords"][0];
    const nlohmann::json& newer = report["pathRecords"][1];
    EXPECT_EQ(older["tick"], 11u);
    EXPECT_EQ(older["result"], "running");
    EXPECT_EQ(older["states"], nlohmann::json::array({0, 2}));
    EXPECT_EQ(newer["program"], 0x8001);
    EXPECT_EQ(newer["result"], "succeeded");
    EXPECT_EQ(newer["states"], nlohmann::json::array({0, 3}));
}

// ticks is refused at 0, above the trace capacity and at 2^32 + 5, which a 32-bit read truncates to 5.
TEST(AiTraceReportTests, RefusesTicksOutsideOneToTheTraceCapacity)
{
    const uint32 capacity = Components::AiTrace::kCapacity;
    EXPECT_EQ(Editor::ReadAiTraceTicks(nlohmann::json::object()), capacity);
    EXPECT_EQ(Editor::ReadAiTraceTicks({{"ticks", capacity}}), capacity);
    EXPECT_FALSE(Editor::ReadAiTraceTicks({{"ticks", 0}}));
    EXPECT_FALSE(Editor::ReadAiTraceTicks({{"ticks", capacity + 1}}));
    EXPECT_FALSE(Editor::ReadAiTraceTicks({{"ticks", (uint64{1} << 32) + 5}}));
    EXPECT_FALSE(Editor::ReadAiTraceTicks({{"ticks", -1}}));
}
