// A state-tree program on agents in a world: AiDecideSystem steps each agent's tree with the registered
// tasks and conditions, AiApplySystem carries out the orders, and AiPathTrace records each path.
#include "AIECS/AiCommand.h"
#include "AIECS/AiProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/AiTestWorld.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeData.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "AIECS/Components/AiPathTrace.h"
#include "AIECS/Components/AiStateTree.h"
#include "ECS/ECSTemplates.h"
#include "ECS/ModuleRegistration.h"
#include "EngineLogCapture.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{

// Work and Rest each run CountTwo, which succeeds on its second tick; each moves to the other on completion.
constexpr const char* kShift = R"({
  "root": { "id": 1, "name": "Shift", "children": [
    { "id": 2, "name": "Work", "tasks": ["CountTwo"], "transitions": [ { "to": 3, "on": "completion" } ] },
    { "id": 3, "name": "Rest", "tasks": ["CountTwo"], "transitions": [ { "to": 2, "on": "completion" } ] }
  ] }
})";

// Calm moves to Alert when the Alarm condition holds.
constexpr const char* kWatch = R"({
  "root": { "id": 1, "name": "Watch", "children": [
    { "id": 2, "name": "Calm", "tasks": ["Report"], "transitions": [ { "to": 3, "on": "condition", "condition": "Alarm" } ] },
    { "id": 3, "name": "Alert", "tasks": ["Report"] }
  ] }
})";

// The root chooses a random child at each start, so the seed decides the paths.
constexpr const char* kWander = R"({
  "root": { "id": 1, "name": "Wander", "policy": "random", "children": [
    { "id": 2, "name": "North", "tasks": ["CountTwo"], "transitions": [ { "to": 1, "on": "completion" } ] },
    { "id": 3, "name": "South", "tasks": ["CountTwo"], "transitions": [ { "to": 1, "on": "completion" } ] },
    { "id": 4, "name": "East", "tasks": ["CountTwo"], "transitions": [ { "to": 1, "on": "completion" } ] }
  ] }
})";

// Walk runs CountTwo, then Stand runs Hold until the agent leaves the program.
constexpr const char* kPatrol = R"({
  "root": { "id": 1, "name": "Patrol", "children": [
    { "id": 2, "name": "Walk", "tasks": ["CountTwo"], "transitions": [ { "to": 3, "on": "completion" } ] },
    { "id": 3, "name": "Stand", "tasks": ["Hold"] }
  ] }
})";

// One state, the root, which runs Measure.
constexpr const char* kMeasure = R"({ "root": { "id": 1, "name": "Measure", "tasks": ["Measure"] } })";

// One state, the root, which runs Report.
constexpr const char* kSolo = R"({ "root": { "id": 1, "name": "Solo", "tasks": ["Report"] } })";

// Nap has a cooldown, so an agent needs one cell for it.
constexpr const char* kNap = R"({ "root": { "id": 1, "name": "Rest", "children": [
  { "id": 2, "name": "Nap", "tasks": ["Report"], "modifiers": [ { "kind": "cooldown", "ticks": 4 } ] } ] } })";

// Calm is entered only when the Spotted condition holds.
constexpr const char* kLookout = R"({ "root": { "id": 1, "name": "Lookout", "children": [
  { "id": 2, "name": "Calm", "tasks": ["Report"], "enterIf": "Spotted" } ] } })";

// The states of kShift, kWatch, kWander and kPatrol in program order: the root, then its children.
constexpr uint16 kRoot = 0;
constexpr uint16 kFirstChild = 1;
constexpr uint16 kSecondChild = 2;

struct TestAlarm
{
    bool Raised = false;
};

// The entity ids that the Report command saw, in the order AiApplySystem ran it.
std::vector<uint32> g_Reports;
// The exit reasons of the Hold tasks, in call order. One thread runs the programs that use Hold.
std::vector<AIECS::AiTaskExitReason> g_HoldExits;
// The exit reasons of the Guard tasks, in call order. One agent runs the programs that use Guard.
std::vector<AIECS::AiTaskExitReason> g_GuardExits;

void EnterCountTwo(AIECS::AiTaskContext& context)
{
    context.GetCells()[0] = 0;
}

AIECS::AiTaskStatus TickCountTwo(AIECS::AiTaskContext& context)
{
    return ++context.GetCells()[0] >= 2 ? AIECS::AiTaskStatus::Succeeded : AIECS::AiTaskStatus::Running;
}

AIECS::AiTaskStatus TickHold(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Running;
}

void ExitHold(AIECS::AiTaskContext&, AIECS::AiTaskExitReason reason)
{
    g_HoldExits.push_back(reason);
}

void ExitGuard(AIECS::AiTaskContext&, AIECS::AiTaskExitReason reason)
{
    g_GuardExits.push_back(reason);
}

// The cell counts that the Measure task saw at Enter and at Exit, in call order.
std::vector<size_t> g_MeasureCells;

void EnterMeasure(AIECS::AiTaskContext& context)
{
    g_MeasureCells.push_back(context.GetCells().size());
}

void ExitMeasure(AIECS::AiTaskContext& context, AIECS::AiTaskExitReason)
{
    g_MeasureCells.push_back(context.GetCells().size());
}

AIECS::AiTaskStatus TickReport(AIECS::AiTaskContext& context)
{
    context.IssueCommand({context.GetAgent(), 0, 0});
    return AIECS::AiTaskStatus::Running;
}

void ApplyReport(ECS::World&, ECS::EntityHandle agent, const AIECS::AiCommand&)
{
    g_Reports.push_back(agent.id);
}

void ReadAlarm(const ECS::World& world, std::span<const ECS::EntityHandle> agents, std::span<uint8> out)
{
    for (size_t index = 0; index < agents.size(); ++index)
        out[index] = world.GetComponent<TestAlarm>(agents[index])->Raised ? 1 : 0;
}

// A registry with the CountTwo, Report, Hold and Measure tasks, the Report command (id 0) and the Alarm condition.
void RegisterTestEntries(AIECS::AiRegistry& registry)
{
    ASSERT_EQ(registry.RegisterCommand(&ApplyReport), 0);
    registry.RegisterTask({&EnterCountTwo, &TickCountTwo, nullptr, "CountTwo"});
    registry.RegisterTask({nullptr, &TickReport, nullptr, "Report"});
    registry.RegisterTask({nullptr, &TickHold, &ExitHold, "Hold"});
    registry.RegisterTask({&EnterMeasure, &TickHold, &ExitMeasure, "Measure"});
    registry.RegisterCondition(&ReadAlarm, "Alarm");
}

uint16 Compile(AIECS::AiRegistry& registry, const char* text)
{
    AIECS::AiStateTreeData data;
    std::string error;
    EXPECT_TRUE(AIECS::ParseAiStateTree(text, data, error)) << error;
    return registry.CompileStateTree(data);
}

// A utility program whose option o runs tasks[o] and scores (o + 1) / tasks.size(), so the last option wins.
uint16 AddUtility(AIECS::AiRegistry& registry, std::initializer_list<uint16> tasks)
{
    AIECS::AiProgram program;
    for (const uint16 task : tasks)
    {
        const float32 weight = static_cast<float32>(program.Options.Options.size() + 1) / static_cast<float32>(tasks.size());
        program.Options.Options.push_back({.Weight = weight, .Action = task});
    }
    return registry.AddProgram(std::move(program));
}

ECS::EntityHandle AddAgent(ECS::World& world, uint16 program)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::AiAgent agent;
    agent.Program = program;
    world.AddComponentImmediate(entity, agent);
    world.AddComponentImmediate(entity, Components::AiStateTree{});
    world.AddComponentImmediate(entity, Components::AiCellsSmall{});
    world.AddComponentImmediate(entity, Components::AiPathTrace{});
    world.AddComponentImmediate(entity, TestAlarm{});
    return entity;
}

// The agent's active states from the root down.
std::vector<uint16> PathOf(const ECS::World& world, ECS::EntityHandle entity)
{
    const Components::AiAgent& agent = *world.GetComponent<Components::AiAgent>(entity);
    return {agent.ActivePath.begin(), agent.ActivePath.begin() + agent.PathDepth};
}

} // namespace

TEST(AiStateTreeBinding, AnAgentFollowsItsProgramTickByTick)
{
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    const uint16 program = Compile(registry, kShift);
    ASSERT_NE(program, Components::AiAgent::kNoProgram);
    EXPECT_NE(program & Components::AiAgent::kStateTreeProgramBit, 0);
    AIECS::AiTestWorld test(registry, 7);
    const ECS::EntityHandle agent = AddAgent(test.GetWorld(), program);

    // CountTwo succeeds on its second tick, and a completion moves to the other child on that tick.
    const std::array<uint16, 8> golden{kFirstChild,  kSecondChild, kSecondChild, kFirstChild,
                                       kFirstChild,  kSecondChild, kSecondChild, kFirstChild};
    for (size_t tick = 0; tick < golden.size(); ++tick)
    {
        test.Step();
        EXPECT_EQ(PathOf(test.GetWorld(), agent), (std::vector<uint16>{kRoot, golden[tick]})) << "tick " << tick;
    }
}

TEST(AiStateTreeBinding, ARegisteredConditionGatesATransition)
{
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    const uint16 program = Compile(registry, kWatch);
    AIECS::AiTestWorld test(registry, 7);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle raised = AddAgent(world, program);
    const ECS::EntityHandle quiet = AddAgent(world, program);

    test.Step(3);
    EXPECT_EQ(PathOf(world, raised), (std::vector<uint16>{kRoot, kFirstChild}));
    world.GetComponentForWrite<TestAlarm>(raised)->Raised = true;
    test.Step();
    EXPECT_EQ(PathOf(world, raised), (std::vector<uint16>{kRoot, kSecondChild}));
    EXPECT_EQ(PathOf(world, quiet), (std::vector<uint16>{kRoot, kFirstChild}));
}

// An agent moved to another state-tree program exits its old path and starts the new tree at its root.
TEST(AiStateTreeBinding, AProgramSwitchStartsTheNewTree)
{
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    const uint16 patrol = Compile(registry, kPatrol);
    const uint16 solo = Compile(registry, kSolo);
    AIECS::AiTestWorld test(registry, 7);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle agent = AddAgent(world, patrol);
    test.Step(3);
    ASSERT_EQ(PathOf(world, agent), (std::vector<uint16>{kRoot, kSecondChild}));
    g_HoldExits.clear();

    world.GetComponentForWrite<Components::AiAgent>(agent)->Program = solo;
    test.Step();

    EXPECT_EQ(PathOf(world, agent), (std::vector<uint16>{kRoot}));
    EXPECT_EQ(world.GetComponent<Components::AiAgent>(agent)->PathProgram, solo);
    EXPECT_EQ(g_HoldExits, (std::vector<AIECS::AiTaskExitReason>{AIECS::AiTaskExitReason::Preempted}));
}

// A task of the old program exits with the cells it entered with, when the new program keeps more cooldowns.
TEST(AiStateTreeBinding, ASwitchExitsTheOldTasksWithTheOldProgramsCells)
{
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    const uint16 measure = Compile(registry, kMeasure);
    const uint16 nap = Compile(registry, kNap);
    AIECS::AiTestWorld test(registry, 7);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle agent = AddAgent(world, measure);
    g_MeasureCells.clear();
    test.Step();
    ASSERT_EQ(g_MeasureCells.size(), 1u);

    world.GetComponentForWrite<Components::AiAgent>(agent)->Program = nap;
    test.Step();

    ASSERT_EQ(g_MeasureCells.size(), 2u);
    EXPECT_EQ(g_MeasureCells[1], g_MeasureCells[0]);
}

// A switch from a state tree to a utility program exits the tree's tasks; no option of the utility
// program exits before it enters.
TEST(AiStateTreeBinding, ASwitchToAUtilityProgramExitsTheTreePath)
{
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    const uint16 patrol = Compile(registry, kPatrol);
    const uint16 guard = registry.RegisterTask({nullptr, &TickHold, &ExitGuard});
    const uint16 idle = registry.RegisterTask({nullptr, &TickHold, nullptr});
    const uint16 utility = AddUtility(registry, {guard, idle});
    AIECS::AiTestWorld test(registry, 7);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle agent = AddAgent(world, patrol);
    test.Step(3);
    ASSERT_EQ(PathOf(world, agent), (std::vector<uint16>{kRoot, kSecondChild}));
    g_HoldExits.clear();
    g_GuardExits.clear();

    world.GetComponentForWrite<Components::AiAgent>(agent)->Program = utility;
    test.Step();

    EXPECT_EQ(g_HoldExits, (std::vector<AIECS::AiTaskExitReason>{AIECS::AiTaskExitReason::Preempted}));
    EXPECT_TRUE(g_GuardExits.empty());
    EXPECT_EQ(PathOf(world, agent), (std::vector<uint16>{1}));
    EXPECT_EQ(world.GetComponent<Components::AiAgent>(agent)->PathProgram, utility);
}

// A switch from a utility program to a state tree exits the active option, and the tree starts at its
// root: the path from before the utility program does not continue.
TEST(AiStateTreeBinding, ASwitchBackToATreeExitsTheOptionAndStartsAtTheRoot)
{
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    const uint16 patrol = Compile(registry, kPatrol);
    const uint16 utility = AddUtility(registry, {registry.RegisterTask({nullptr, &TickHold, &ExitGuard})});
    AIECS::AiTestWorld test(registry, 7);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle agent = AddAgent(world, patrol);
    test.Step(3);
    ASSERT_EQ(PathOf(world, agent), (std::vector<uint16>{kRoot, kSecondChild}));
    world.GetComponentForWrite<Components::AiAgent>(agent)->Program = utility;
    test.Step();
    g_GuardExits.clear();

    world.GetComponentForWrite<Components::AiAgent>(agent)->Program = patrol;
    test.Step();

    EXPECT_EQ(g_GuardExits, (std::vector<AIECS::AiTaskExitReason>{AIECS::AiTaskExitReason::Preempted}));
    EXPECT_EQ(PathOf(world, agent), (std::vector<uint16>{kRoot, kFirstChild}));
}

// A restart request exits the running tasks, starts the tree again and is cleared.
TEST(AiStateTreeBinding, ARestartExitsTheRunningTasks)
{
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    const uint16 patrol = Compile(registry, kPatrol);
    AIECS::AiTestWorld test(registry, 7);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle agent = AddAgent(world, patrol);
    test.Step(3);
    ASSERT_EQ(PathOf(world, agent), (std::vector<uint16>{kRoot, kSecondChild}));
    g_HoldExits.clear();

    world.GetComponentForWrite<Components::AiAgent>(agent)->RestartRequested = true;
    test.Step();

    EXPECT_EQ(g_HoldExits, (std::vector<AIECS::AiTaskExitReason>{AIECS::AiTaskExitReason::Preempted}));
    EXPECT_EQ(PathOf(world, agent), (std::vector<uint16>{kRoot, kFirstChild}));
    EXPECT_FALSE(world.GetComponent<Components::AiAgent>(agent)->RestartRequested);
}

// A restart and a program change in one decision exit the tasks of the old program and start the new tree
// at its root.
TEST(AiStateTreeBinding, ARestartWithAProgramChangeExitsTheOldProgramsTasks)
{
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    const uint16 patrol = Compile(registry, kPatrol);
    const uint16 solo = Compile(registry, kSolo);
    AIECS::AiTestWorld test(registry, 7);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle agent = AddAgent(world, patrol);
    test.Step(3);
    ASSERT_EQ(PathOf(world, agent), (std::vector<uint16>{kRoot, kSecondChild}));
    g_HoldExits.clear();

    Components::AiAgent& state = *world.GetComponentForWrite<Components::AiAgent>(agent);
    state.Program = solo;
    state.RestartRequested = true;
    test.Step();

    EXPECT_EQ(g_HoldExits, (std::vector<AIECS::AiTaskExitReason>{AIECS::AiTaskExitReason::Preempted}));
    EXPECT_EQ(PathOf(world, agent), (std::vector<uint16>{kRoot}));
    EXPECT_EQ(world.GetComponent<Components::AiAgent>(agent)->PathProgram, solo);
}

// A path with a state that its recorded program does not have exits no task, and the agent's program starts
// at its root.
TEST(AiStateTreeBinding, APathOutOfRangeForItsProgramExitsNothing)
{
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    const uint16 patrol = Compile(registry, kPatrol);
    const uint16 solo = Compile(registry, kSolo);
    AIECS::AiTestWorld test(registry, 7);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle agent = AddAgent(world, patrol);
    test.Step(3);
    ASSERT_EQ(PathOf(world, agent), (std::vector<uint16>{kRoot, kSecondChild}));
    g_HoldExits.clear();

    world.GetComponentForWrite<Components::AiAgent>(agent)->PathProgram = solo;
    test.Step();

    EXPECT_TRUE(g_HoldExits.empty());
    EXPECT_EQ(PathOf(world, agent), (std::vector<uint16>{kRoot, kFirstChild}));
    EXPECT_EQ(world.GetComponent<Components::AiAgent>(agent)->PathProgram, patrol);
}

// An agent with no AiStateTree, with no cell for its program's cooldown, or on a program whose condition
// its module removed does not decide; one error for each cause names the fix.
TEST(AiStateTreeBinding, AnAgentThatCannotRunItsTreeDoesNotDecide)
{
    constexpr const char* kModule = "FakeConditionModule";
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    ECS::SetActiveRegistrationModule(kModule, 1);
    registry.RegisterCondition(&ReadAlarm, "Spotted");
    ECS::ClearActiveRegistrationModule();
    const uint16 lookout = Compile(registry, kLookout);
    registry.PurgeModuleRegistrations(kModule, 1);
    AIECS::AiTestWorld test(registry, 7);
    ECS::World& world = test.GetWorld();
    Components::AiAgent agent;
    agent.Program = Compile(registry, kWatch);
    const ECS::EntityHandle noTree = world.Create(agent).GetHandle();
    agent.Program = Compile(registry, kNap);
    const ECS::EntityHandle noCells = world.Create(agent, Components::AiStateTree{}).GetHandle();
    const ECS::EntityHandle removed = AddAgent(world, lookout);

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        test.Step(2);
        Logger::Log::Flush();
    }

    EXPECT_EQ(TestLog::CountLinesContaining(lines, "Add AiStateTree to each agent"), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "Give them a larger cell class"), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "Load the module that registers it"), 1u);
    for (const ECS::EntityHandle entity : {noTree, noCells, removed})
        EXPECT_TRUE(PathOf(world, entity).empty()) << "entity " << entity.id;
}

// Equal agents and seeds give the same paths on one thread and on a pool of workers.
TEST(AiStateTreeBinding, TwoWorldsWithEqualInputsRecordEqualTraces)
{
    constexpr uint32 kAgents = 1000;
    constexpr uint32 kTicks = 32;
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    const uint16 program = Compile(registry, kWander);
    JobSystem::WorkStealingThreadPool pool(4);
    AIECS::AiTestWorld serial(registry, 99);
    AIECS::AiTestWorld parallel(registry, 99, &pool);
    std::vector<ECS::EntityHandle> serialAgents;
    std::vector<ECS::EntityHandle> parallelAgents;
    for (uint32 index = 0; index < kAgents; ++index)
    {
        serialAgents.push_back(AddAgent(serial.GetWorld(), program));
        parallelAgents.push_back(AddAgent(parallel.GetWorld(), program));
    }
    serial.Step(kTicks);
    parallel.Step(kTicks);

    std::array<uint32, 3> chosen{};
    for (uint32 index = 0; index < kAgents; ++index)
    {
        const auto& left = *serial.GetWorld().GetComponent<Components::AiPathTrace>(serialAgents[index]);
        const auto& right = *parallel.GetWorld().GetComponent<Components::AiPathTrace>(parallelAgents[index]);
        ASSERT_EQ(left.Count, kTicks);
        ASSERT_EQ(std::memcmp(&left, &right, sizeof(left)), 0) << "agent " << index;
        ++chosen[left.Records[0].States[1] - kFirstChild];
    }
    // The draws differ between agents: each child starts some of them.
    for (const uint32 count : chosen)
        EXPECT_GT(count, kAgents / 6);
}

TEST(AiStateTreeBinding, TaskOrdersRunInEntityOrderAcrossPrograms)
{
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    const uint16 watch = Compile(registry, kWatch);
    const uint16 copy = Compile(registry, kWatch);
    AIECS::AiTestWorld test(registry, 7);
    std::vector<uint32> created;
    for (uint32 index = 0; index < 6; ++index)
        created.push_back(AddAgent(test.GetWorld(), index % 2 == 0 ? copy : watch).id);
    g_Reports.clear();

    test.Step();

    EXPECT_EQ(g_Reports, created);
}

TEST(AiStateTreeBinding, ThePathTraceRecordsEachDecision)
{
    AIECS::AiRegistry registry;
    RegisterTestEntries(registry);
    const uint16 program = Compile(registry, kShift);
    AIECS::AiTestWorld test(registry, 7);
    const ECS::EntityHandle agent = AddAgent(test.GetWorld(), program);

    test.Step(2);

    const Components::AiPathTrace& trace = *test.GetWorld().GetComponent<Components::AiPathTrace>(agent);
    ASSERT_EQ(trace.Count, 2u);
    const AIECS::AiPathTraceRecord& second = trace.Records[1];
    EXPECT_EQ(second.Tick, trace.Records[0].Tick + 1);
    EXPECT_EQ(second.Program, program);
    EXPECT_EQ(second.Depth, 2);
    EXPECT_EQ(second.Result, AI::StateTreeTaskStatus::Running);
    EXPECT_EQ(second.States[0], kRoot);
    EXPECT_EQ(second.States[1], kSecondChild);
    EXPECT_EQ(second.States[2], AIECS::AiPathTraceRecord::kNoState);
}
