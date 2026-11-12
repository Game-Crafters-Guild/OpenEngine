#include "AIECS/AiCommand.h"
#include "AIECS/AiProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/AiTestWorld.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsMedium.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "AIECS/Components/AiClock.h"
#include "AIECS/Components/AiTrace.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "EngineLogCapture.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{

struct TestHunger
{
    float32 Value = 0.0f;
};

struct TestMeals
{
    int32 Count = 0;
};

// Registry ids of the test's command, set once in MakeProgram.
uint16 g_EatCommand = 0;

void ReadHunger(const ECS::World& world, std::span<const ECS::EntityHandle> agents, std::span<float32> out)
{
    for (size_t index = 0; index < agents.size(); ++index)
        out[index] = world.GetComponent<TestHunger>(agents[index])->Value;
}

AIECS::AiTaskStatus TickEat(AIECS::AiTaskContext& context)
{
    context.IssueCommand({context.GetAgent(), 1, g_EatCommand});
    return AIECS::AiTaskStatus::Running;
}

AIECS::AiTaskStatus TickIdle(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Running;
}

void ApplyEat(ECS::World& world, ECS::EntityHandle, const AIECS::AiCommand& command)
{
    world.GetComponentForWrite<TestMeals>(command.Target)->Count += command.Argument;
}

// Option 0 eats with a score equal to the hunger; option 1 idles at a flat 0.5.
uint16 MakeProgram(AIECS::AiRegistry& registry)
{
    g_EatCommand = registry.RegisterCommand(&ApplyEat);
    const uint16 hunger = registry.RegisterInput(&ReadHunger, AIECS::AiInputThreading::CallingThread);
    const uint16 eat = registry.RegisterTask({nullptr, &TickEat, nullptr});
    const uint16 idle = registry.RegisterTask({nullptr, &TickIdle, nullptr});

    AIECS::AiProgram program;
    program.ColumnInputs = {hunger};
    program.Options.Curves.push_back(AI::ResponseCurve::FromPreset(AI::CurvePreset::Linear, 1.0f, 0.5f));
    program.Options.Considerations.push_back({0.0f, 1.0f, 0, 0});
    program.Options.Options.push_back({1.0f, 0, 1, eat, 0});
    program.Options.Options.push_back({0.5f, 0, 0, idle, 0});
    return registry.AddProgram(std::move(program));
}

// The targets of the record commands, in the order AiApplySystem applies them.
std::vector<uint32> g_Applied;
uint16 g_RecordCommand = 0;
constexpr uint16 kUnregisteredCommand = 7;
constexpr uint16 kSecondUnregisteredCommand = 9;

struct TestTag
{
};

AIECS::AiTaskStatus TickRecord(AIECS::AiTaskContext& context)
{
    context.IssueCommand({context.GetAgent(), 0, g_RecordCommand});
    return AIECS::AiTaskStatus::Running;
}

AIECS::AiTaskStatus TickUnregistered(AIECS::AiTaskContext& context)
{
    context.IssueCommand({context.GetAgent(), 0, kUnregisteredCommand});
    context.IssueCommand({context.GetAgent(), 0, kSecondUnregisteredCommand});
    return AIECS::AiTaskStatus::Running;
}

void ApplyRecord(ECS::World&, ECS::EntityHandle, const AIECS::AiCommand& command)
{
    g_Applied.push_back(command.Target.id);
}

// A program with one option that always runs `task`.
uint16 MakeSingleTaskProgram(AIECS::AiRegistry& registry, uint16 task)
{
    AIECS::AiProgram program;
    program.Options.Options.push_back({.Action = task});
    return registry.AddProgram(std::move(program));
}

// Runs one AI tick: three steps at 60 steps per second and 20 AI ticks per second.
void StepOneAiTick(AIECS::AiRegistry& registry, ECS::World& world)
{
    ECS::SystemScheduleBuilder builder;
    registry.AddSystemsToSchedule(builder);
    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Complete);
    manager.Update(world, static_cast<float32>(3.0 * ECS::SimulationClock::kDefaultFixedDeltaSeconds));
}

// Creates an agent with a trace that runs `program` every `cadence` AI ticks.
ECS::EntityHandle CreateTracedAgent(ECS::World& world, uint16 program, uint16 cadence)
{
    Components::AiAgent agent;
    agent.Program = program;
    agent.Cadence = cadence;
    return world.Create(agent, Components::AiTrace{}).GetHandle();
}

uint32 CountDecisions(ECS::World& world, ECS::EntityHandle agent)
{
    return world.GetComponent<Components::AiTrace>(agent)->Count;
}

// Counted by the inputs of InputsRunOnTheThreadTheirRegistrationNames.
std::atomic<uint32> g_JobInputCalls{0};
std::atomic<size_t> g_JobInputRows{0};
std::thread::id g_CallingInputThread;
size_t g_CallingInputRows = 0;

void ReadOnJobs(const ECS::World&, std::span<const ECS::EntityHandle> agents, std::span<float32> out)
{
    g_JobInputCalls.fetch_add(1);
    g_JobInputRows.fetch_add(agents.size());
    std::fill(out.begin(), out.end(), 1.0f);
}

void ReadOnCallingThread(const ECS::World&, std::span<const ECS::EntityHandle> agents, std::span<float32> out)
{
    g_CallingInputThread = std::this_thread::get_id();
    g_CallingInputRows += agents.size();
    std::fill(out.begin(), out.end(), 1.0f);
}

void ReadOne(const ECS::World&, std::span<const ECS::EntityHandle>, std::span<float32> out)
{
    std::fill(out.begin(), out.end(), 1.0f);
}

uint16 g_DestroyCommand = 0;

void ApplyDestroy(ECS::World& world, ECS::EntityHandle, const AIECS::AiCommand& command)
{
    world.DestroyEntityImmediate(command.Target);
}

// Destroys its agent with the first order and records it with the second.
AIECS::AiTaskStatus TickDestroyThenRecord(AIECS::AiTaskContext& context)
{
    context.IssueCommand({context.GetAgent(), 0, g_DestroyCommand});
    context.IssueCommand({context.GetAgent(), 0, g_RecordCommand});
    return AIECS::AiTaskStatus::Running;
}

} // namespace

// An agent with kNoProgram, the default, is never scheduled and never decides.
TEST(AiDecideTests, AgentWithNoProgramIsSkipped)
{
    AIECS::AiRegistry registry;
    MakeProgram(registry);
    ECS::World world;
    const ECS::EntityHandle entity = world.Create(Components::AiAgent{}, TestHunger{0.9f}, TestMeals{}).GetHandle();

    StepOneAiTick(registry, world);

    const Components::AiAgent* agent = world.GetComponent<Components::AiAgent>(entity);
    EXPECT_FALSE(agent->Scheduled);
    EXPECT_EQ(agent->NextTick, 0u);
    EXPECT_EQ(world.GetComponent<TestMeals>(entity)->Count, 0);
}

// A task that issues two unregistered command types in one decision gets one error for each
// type, however many agents issue them on however many ticks, and nothing is applied.
TEST(AiDecideTests, UnregisteredCommandTypeIsRefused)
{
    AIECS::AiRegistry registry;
    g_RecordCommand = registry.RegisterCommand(&ApplyRecord);
    Components::AiAgent agent;
    agent.Program = MakeSingleTaskProgram(registry, registry.RegisterTask({nullptr, &TickUnregistered, nullptr}));
    AIECS::AiTestWorld test(registry, 1);
    test.GetWorld().Create(agent);
    test.GetWorld().Create(agent);
    g_Applied.clear();

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        test.Step(3);
        Logger::Log::Flush();
    }
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "command type 7 is not registered"), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "command type 9 is not registered"), 1u);
    EXPECT_TRUE(g_Applied.empty());
}

// AiApplySystem applies the commands in entity order, whatever the program and the archetype
// of each agent.
TEST(AiDecideTests, ApplyRunsInEntityOrderAcrossPrograms)
{
    AIECS::AiRegistry registry;
    g_RecordCommand = registry.RegisterCommand(&ApplyRecord);
    const uint16 task = registry.RegisterTask({nullptr, &TickRecord, nullptr});
    Components::AiAgent first;
    first.Program = MakeSingleTaskProgram(registry, task);
    Components::AiAgent second;
    second.Program = MakeSingleTaskProgram(registry, task);
    ECS::World world;
    const ECS::EntityHandle a = world.Create(second, TestTag{}).GetHandle();
    const ECS::EntityHandle b = world.Create(first).GetHandle();
    const ECS::EntityHandle c = world.Create(second).GetHandle();
    const ECS::EntityHandle d = world.Create(first, TestTag{}).GetHandle();
    g_Applied.clear();

    StepOneAiTick(registry, world);

    EXPECT_EQ(g_Applied, (std::vector<uint32>{a.id, b.id, c.id, d.id}));
}

// At 60 steps per second and 20 AI ticks per second, three steps make one AI tick. Each agent
// decides once on it: the hungry one eats through the applied command, the other idles.
TEST(AiDecideTests, TwoAgentsDecideAndApplyOncePerAiTick)
{
    AIECS::AiRegistry registry;
    const uint16 program = MakeProgram(registry);
    ECS::World world;
    Components::AiAgent agent;
    agent.Program = program;
    const ECS::EntityHandle hungry = world.Create(agent, TestHunger{0.9f}, TestMeals{}).GetHandle();
    const ECS::EntityHandle full = world.Create(agent, TestHunger{0.2f}, TestMeals{}).GetHandle();

    ECS::SystemScheduleBuilder builder;
    registry.AddSystemsToSchedule(builder);
    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Complete);

    manager.Update(world, static_cast<float32>(3.0 * ECS::SimulationClock::kDefaultFixedDeltaSeconds));

    const Components::AiClock* clock = world.GetSingleton<Components::AiClock>();
    ASSERT_NE(clock, nullptr);
    EXPECT_EQ(clock->Divider, 3u);
    EXPECT_EQ(clock->Tick, 1u);

    EXPECT_EQ(world.GetComponent<TestMeals>(hungry)->Count, 1);
    EXPECT_EQ(world.GetComponent<TestMeals>(full)->Count, 0);
    const Components::AiAgent* hungryAgent = world.GetComponent<Components::AiAgent>(hungry);
    const Components::AiAgent* fullAgent = world.GetComponent<Components::AiAgent>(full);
    EXPECT_EQ(hungryAgent->ActivePath[0], 0);
    EXPECT_NEAR(hungryAgent->ActiveScore, 0.9f, 1.0e-3f);
    EXPECT_EQ(fullAgent->ActivePath[0], 1);
    EXPECT_EQ(hungryAgent->NextTick, 2u);

    manager.Update(world, static_cast<float32>(3.0 * ECS::SimulationClock::kDefaultFixedDeltaSeconds));
    EXPECT_EQ(world.GetComponent<TestMeals>(hungry)->Count, 2);
    EXPECT_EQ(world.GetComponent<TestMeals>(full)->Count, 0);
}

// An agent whose program id is not registered is skipped like kNoProgram, with one error.
TEST(AiDecideTests, AgentWithUnregisteredProgramIsSkippedWithOneError)
{
    AIECS::AiRegistry registry;
    MakeSingleTaskProgram(registry, registry.RegisterTask({nullptr, &TickIdle, nullptr}));
    AIECS::AiTestWorld test(registry, 1);
    const ECS::EntityHandle first = CreateTracedAgent(test.GetWorld(), 5, 1);
    const ECS::EntityHandle second = CreateTracedAgent(test.GetWorld(), 5, 1);

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        test.Step(3);
        Logger::Log::Flush();
    }
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "program 5 is not registered"), 1u);
    for (const ECS::EntityHandle agent : {first, second})
    {
        EXPECT_FALSE(test.GetWorld().GetComponent<Components::AiAgent>(agent)->Scheduled);
        EXPECT_EQ(CountDecisions(test.GetWorld(), agent), 0u);
    }
}

// An agent with cadence 3 decides on every third AI tick.
TEST(AiDecideTests, AgentDecidesOncePerCadence)
{
    AIECS::AiRegistry registry;
    const uint16 program = MakeSingleTaskProgram(registry, registry.RegisterTask({nullptr, &TickIdle, nullptr}));
    AIECS::AiTestWorld test(registry, 1);
    const ECS::EntityHandle agent = CreateTracedAgent(test.GetWorld(), program, 3);

    test.Step(12);

    const Components::AiTrace& trace = *test.GetWorld().GetComponent<Components::AiTrace>(agent);
    ASSERT_EQ(trace.Count, 4u);
    for (uint32 index = 1; index < trace.Count; ++index)
        EXPECT_EQ(trace.Records[index].Tick - trace.Records[index - 1].Tick, 3u);
}

namespace
{

// The exit reasons of ExitHeld, in call order. One agent runs the program that uses it.
std::vector<AIECS::AiTaskExitReason> g_HeldExits;

void ExitHeld(AIECS::AiTaskContext&, AIECS::AiTaskExitReason reason)
{
    g_HeldExits.push_back(reason);
}

} // namespace

// A switch to a program with fewer options exits the active option of the old program, and the new
// program chooses from its own options.
TEST(AiDecideTests, ASwitchToAProgramWithFewerOptionsExitsTheOldOption)
{
    AIECS::AiRegistry registry;
    const uint16 idle = registry.RegisterTask({nullptr, &TickIdle, nullptr});
    AIECS::AiProgram twoOptions;
    twoOptions.Options.Options.push_back({.Weight = 0.5f, .Action = idle});
    twoOptions.Options.Options.push_back({.Weight = 1.0f, .Action = registry.RegisterTask({nullptr, &TickIdle, &ExitHeld})});
    const uint16 first = registry.AddProgram(std::move(twoOptions));
    const uint16 second = MakeSingleTaskProgram(registry, idle);
    AIECS::AiTestWorld test(registry, 1);
    const ECS::EntityHandle agent = CreateTracedAgent(test.GetWorld(), first, 1);
    test.Step();
    ASSERT_EQ(test.GetWorld().GetComponent<Components::AiAgent>(agent)->ActivePath[0], 1);
    g_HeldExits.clear();

    test.GetWorld().GetComponentForWrite<Components::AiAgent>(agent)->Program = second;
    test.Step();

    EXPECT_EQ(g_HeldExits, (std::vector<AIECS::AiTaskExitReason>{AIECS::AiTaskExitReason::Preempted}));
    const Components::AiAgent& state = *test.GetWorld().GetComponent<Components::AiAgent>(agent);
    EXPECT_EQ(state.PathDepth, 1);
    EXPECT_EQ(state.ActivePath[0], 0);
    EXPECT_EQ(state.PathProgram, second);
}

// A restart request exits the active option, the agent chooses again, and the request is cleared.
TEST(AiDecideTests, ARestartRequestExitsTheActiveOption)
{
    AIECS::AiRegistry registry;
    const uint16 program = MakeSingleTaskProgram(registry, registry.RegisterTask({nullptr, &TickIdle, &ExitHeld}));
    AIECS::AiTestWorld test(registry, 1);
    const ECS::EntityHandle agent = CreateTracedAgent(test.GetWorld(), program, 1);
    test.Step();
    g_HeldExits.clear();

    test.GetWorld().GetComponentForWrite<Components::AiAgent>(agent)->RestartRequested = true;
    test.Step();

    EXPECT_EQ(g_HeldExits, (std::vector<AIECS::AiTaskExitReason>{AIECS::AiTaskExitReason::Preempted}));
    const Components::AiAgent& state = *test.GetWorld().GetComponent<Components::AiAgent>(agent);
    EXPECT_EQ(state.PathDepth, 1);
    EXPECT_FALSE(state.RestartRequested);
}

// Agents with one cadence make their first decision on the tick their entity index modulo the
// cadence names, so a population does not decide all on one tick.
TEST(AiDecideTests, CadenceSpreadsFirstDecisionsByEntityIndex)
{
    AIECS::AiRegistry registry;
    const uint16 program = MakeSingleTaskProgram(registry, registry.RegisterTask({nullptr, &TickIdle, nullptr}));
    AIECS::AiTestWorld test(registry, 1);
    std::vector<ECS::EntityHandle> agents;
    for (uint32 index = 0; index < 8; ++index)
        agents.push_back(CreateTracedAgent(test.GetWorld(), program, 4));

    test.Step(1);
    for (const ECS::EntityHandle agent : agents)
        EXPECT_EQ(CountDecisions(test.GetWorld(), agent), agent.index % 4 == 0 ? 1u : 0u) << agent.index;
    test.Step(3);
    for (const ECS::EntityHandle agent : agents)
        EXPECT_EQ(CountDecisions(test.GetWorld(), agent), 1u) << agent.index;
    test.Step(4);
    for (const ECS::EntityHandle agent : agents)
        EXPECT_EQ(CountDecisions(test.GetWorld(), agent), 2u) << agent.index;
}

// A woken agent decides on the next AI tick, whatever its cadence, and then keeps its cadence.
TEST(AiDecideTests, WokenAgentDecidesOnTheNextTick)
{
    AIECS::AiRegistry registry;
    const uint16 program = MakeSingleTaskProgram(registry, registry.RegisterTask({nullptr, &TickIdle, nullptr}));
    AIECS::AiTestWorld test(registry, 1);
    const ECS::EntityHandle agent = CreateTracedAgent(test.GetWorld(), program, 10);
    for (uint32 tick = 0; tick < 10 && CountDecisions(test.GetWorld(), agent) == 0; ++tick)
        test.Step(1);
    ASSERT_EQ(CountDecisions(test.GetWorld(), agent), 1u);

    test.GetWorld().GetComponentForWrite<Components::AiAgent>(agent)->Wake();
    test.Step(1);
    ASSERT_EQ(CountDecisions(test.GetWorld(), agent), 2u);
    const uint32 wokenTick = test.GetWorld().GetComponent<Components::AiTrace>(agent)->Records[1].Tick;

    test.Step(9);
    EXPECT_EQ(CountDecisions(test.GetWorld(), agent), 2u);
    test.Step(1);
    ASSERT_EQ(CountDecisions(test.GetWorld(), agent), 3u);
    EXPECT_EQ(test.GetWorld().GetComponent<Components::AiTrace>(agent)->Records[2].Tick, wokenTick + 10);
}

// Agents woken before their first decision get a draw seed each, so their noise draws differ.
TEST(AiDecideTests, AgentsWokenBeforeTheirFirstDecisionGetDifferentDrawSeeds)
{
    AIECS::AiRegistry registry;
    const uint16 program = MakeSingleTaskProgram(registry, registry.RegisterTask({nullptr, &TickIdle, nullptr}));
    AIECS::AiTestWorld test(registry, 1);
    const ECS::EntityHandle first = CreateTracedAgent(test.GetWorld(), program, 4);
    const ECS::EntityHandle second = CreateTracedAgent(test.GetWorld(), program, 4);
    test.GetWorld().GetComponentForWrite<Components::AiAgent>(first)->Wake();
    test.GetWorld().GetComponentForWrite<Components::AiAgent>(second)->Wake();

    test.Step(1);

    const uint32 firstSeed = test.GetWorld().GetComponent<Components::AiAgent>(first)->DrawSeed;
    const uint32 secondSeed = test.GetWorld().GetComponent<Components::AiAgent>(second)->DrawSeed;
    EXPECT_NE(firstSeed, 0u);
    EXPECT_NE(secondSeed, 0u);
    EXPECT_NE(firstSeed, secondSeed);
}

// Ticks past the world's AI tick, as an additive scene load gives, are set to the AI tick with one
// warning, each ready tick is set to at most the AI tick plus the cooldown, and the agent decides
// within its cadence.
TEST(AiDecideTests, TicksPastTheAiTickAreSetToTheAiTickWithOneWarning)
{
    constexpr uint16 kCadence = 4;
    constexpr uint32 kCooldown = 2;
    AIECS::AiRegistry registry;
    AIECS::AiProgram cooldownProgram;
    cooldownProgram.Options.Options.push_back(
        {.Action = registry.RegisterTask({nullptr, &TickIdle, nullptr}), .CooldownTicks = kCooldown});
    cooldownProgram.CooldownCell = 0;
    const uint16 program = registry.AddProgram(std::move(cooldownProgram));
    AIECS::AiTestWorld test(registry, 1);
    std::vector<ECS::EntityHandle> agents;
    for (uint32 index = 0; index < 2; ++index)
    {
        Components::AiAgent agent;
        agent.Program = program;
        agent.Cadence = kCadence;
        agent.EnteredTick = 1000;
        agent.NextTick = 5000;
        agent.Scheduled = true;
        Components::AiCellsSmall cells;
        cells.Values[0] = 5000;
        agents.push_back(test.GetWorld().Create(agent, cells, Components::AiTrace{}).GetHandle());
    }

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        test.Step(1);
        Logger::Log::Flush();
    }
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "entered its state after the AI tick"), 1u);
    const uint32 tick = test.GetWorld().GetSingleton<Components::AiClock>()->Tick;
    for (const ECS::EntityHandle entity : agents)
    {
        const Components::AiAgent& agent = *test.GetWorld().GetComponent<Components::AiAgent>(entity);
        EXPECT_LE(agent.EnteredTick, tick);
        EXPECT_LE(agent.NextTick, tick + kCadence);
        EXPECT_LE(test.GetWorld().GetComponent<Components::AiCellsSmall>(entity)->Values[0], tick + kCooldown);
    }
    test.Step(kCadence);
    for (const ECS::EntityHandle entity : agents)
        EXPECT_EQ(CountDecisions(test.GetWorld(), entity), 1u);
}

// On the tick of a program switch the path still belongs to the old program, so the clamp of the ticks past
// the AI tick reads the old program's cooldown cells and leaves the new program's cells alone.
TEST(AiDecideTests, TheClampOnASwitchTickReadsTheCooldownCellsOfThePathsProgram)
{
    constexpr uint16 kCadence = 4;
    constexpr uint32 kCooldown = 2;
    AIECS::AiRegistry registry;
    const uint16 idle = registry.RegisterTask({nullptr, &TickIdle, nullptr});
    AIECS::AiProgram oldProgram;
    oldProgram.Options.Options.push_back({.Action = idle, .CooldownTicks = kCooldown});
    oldProgram.CooldownCell = 0;
    AIECS::AiProgram newProgram;
    newProgram.Options.Options.push_back({.Action = idle, .CooldownTicks = kCooldown});
    newProgram.Options.Options.push_back({.Action = idle, .CooldownTicks = kCooldown});
    newProgram.CooldownCell = 4;
    const uint16 oldId = registry.AddProgram(std::move(oldProgram));
    const uint16 newId = registry.AddProgram(std::move(newProgram));
    AIECS::AiTestWorld test(registry, 1);
    Components::AiAgent agent;
    agent.Program = newId;
    agent.PathProgram = oldId;
    agent.Cadence = kCadence;
    agent.EnteredTick = 1000;
    agent.NextTick = 5000;
    agent.Scheduled = true;
    Components::AiCellsSmall cells;
    cells.Values[0] = 5000;
    cells.Values[4] = 5000;
    const ECS::EntityHandle entity = test.GetWorld().Create(agent, cells).GetHandle();

    test.Step(1);

    const uint32 tick = test.GetWorld().GetSingleton<Components::AiClock>()->Tick;
    const Components::AiCellsSmall& after = *test.GetWorld().GetComponent<Components::AiCellsSmall>(entity);
    EXPECT_LE(after.Values[0], tick + kCooldown) << "the old program's cooldown cell";
    EXPECT_EQ(after.Values[4], 5000u) << "the new program's cooldown cell";
}

// An input registered for job threads fills each job's agents on that job; an input registered
// for the calling thread fills all agents in one call on the thread that runs the decisions.
TEST(AiDecideTests, InputsRunOnTheThreadTheirRegistrationNames)
{
    constexpr uint32 kAgentCount = 1000;
    AIECS::AiRegistry registry;
    AIECS::AiProgram program;
    program.ColumnInputs = {registry.RegisterInput(&ReadOnJobs, AIECS::AiInputThreading::JobThreads),
                            registry.RegisterInput(&ReadOnCallingThread, AIECS::AiInputThreading::CallingThread)};
    program.Options.Options.push_back({.Action = registry.RegisterTask({nullptr, &TickIdle, nullptr})});
    const uint16 programId = registry.AddProgram(std::move(program));
    JobSystem::WorkStealingThreadPool jobs(2);
    AIECS::AiTestWorld test(registry, 1, &jobs);
    for (uint32 index = 0; index < kAgentCount; ++index)
        CreateTracedAgent(test.GetWorld(), programId, 1);
    g_JobInputCalls = 0;
    g_JobInputRows = 0;
    g_CallingInputRows = 0;

    test.Step(1);

    EXPECT_GT(g_JobInputCalls.load(), 1u);
    EXPECT_EQ(g_JobInputRows.load(), kAgentCount);
    EXPECT_EQ(g_CallingInputThread, std::this_thread::get_id());
    EXPECT_EQ(g_CallingInputRows, kAgentCount);
}

// Each agent's own value of a CallingThread input reaches its decision in every job's chunk,
// also in the second column of the program.
TEST(AiDecideTests, CallingThreadInputsReachEachAgentAcrossChunks)
{
    constexpr uint32 kAgentCount = 200;
    AIECS::AiRegistry registry;
    g_EatCommand = registry.RegisterCommand(&ApplyEat);
    AIECS::AiProgram program;
    program.ColumnInputs = {registry.RegisterInput(&ReadOne, AIECS::AiInputThreading::CallingThread),
                            registry.RegisterInput(&ReadHunger, AIECS::AiInputThreading::CallingThread)};
    program.Options.Curves.push_back(AI::ResponseCurve::FromPreset(AI::CurvePreset::Linear, 1.0f, 0.5f));
    program.Options.Considerations.push_back({0.0f, 1.0f, 1, 0});
    program.Options.Options.push_back({1.0f, 0, 1, registry.RegisterTask({nullptr, &TickEat, nullptr}), 0});
    program.Options.Options.push_back({0.5f, 0, 0, registry.RegisterTask({nullptr, &TickIdle, nullptr}), 0});
    const uint16 programId = registry.AddProgram(std::move(program));
    JobSystem::WorkStealingThreadPool jobs(2);
    AIECS::AiTestWorld test(registry, 1, &jobs);
    std::vector<ECS::EntityHandle> agents;
    for (uint32 index = 0; index < kAgentCount; ++index)
    {
        // Every third agent is hungry; each hunger is distinct and far from the idle score.
        const float32 spread = 0.2f * static_cast<float32>(index) / static_cast<float32>(kAgentCount);
        const float32 hunger = (index % 3 == 0 ? 0.75f : 0.05f) + spread;
        Components::AiAgent agent;
        agent.Program = programId;
        agents.push_back(test.GetWorld().Create(agent, TestHunger{hunger}, TestMeals{}).GetHandle());
    }

    test.Step(1);

    for (uint32 index = 0; index < kAgentCount; ++index)
        EXPECT_EQ(test.GetWorld().GetComponent<TestMeals>(agents[index])->Count, index % 3 == 0 ? 1 : 0) << index;
}

// An agent whose cells cannot hold its program's cooldown cells is skipped, with one error for
// each program and cell class; an agent with enough cells decides.
TEST(AiDecideTests, AgentWhoseCellsCannotHoldTheCooldownsIsSkippedWithOneError)
{
    AIECS::AiRegistry registry;
    const uint16 idle = registry.RegisterTask({nullptr, &TickIdle, nullptr});
    AIECS::AiProgram program;
    for (uint32 option = 0; option < 6; ++option)
        program.Options.Options.push_back({.Action = idle, .CooldownTicks = 2});
    program.CooldownCell = 4;
    const uint16 programId = registry.AddProgram(std::move(program));
    ASSERT_NE(programId, Components::AiAgent::kNoProgram);
    AIECS::AiTestWorld test(registry, 1);
    Components::AiAgent agent;
    agent.Program = programId;
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle small = world.Create(agent, Components::AiCellsSmall{}, Components::AiTrace{}).GetHandle();
    const ECS::EntityHandle otherSmall = world.Create(agent, Components::AiCellsSmall{}, Components::AiTrace{}).GetHandle();
    const ECS::EntityHandle none = world.Create(agent, Components::AiTrace{}).GetHandle();
    const ECS::EntityHandle medium = world.Create(agent, Components::AiCellsMedium{}, Components::AiTrace{}).GetHandle();

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        test.Step(3);
        Logger::Log::Flush();
    }
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "cells cannot hold"), 2u);
    for (const ECS::EntityHandle skipped : {small, otherSmall, none})
        EXPECT_EQ(CountDecisions(world, skipped), 0u) << skipped.index;
    EXPECT_EQ(CountDecisions(world, medium), 3u);
}

// AiApplySystem skips the orders of an agent that an earlier order destroyed, and still applies
// the orders of the other agents.
TEST(AiDecideTests, OrdersOfAnAgentDestroyedByAnEarlierOrderAreSkipped)
{
    AIECS::AiRegistry registry;
    g_RecordCommand = registry.RegisterCommand(&ApplyRecord);
    g_DestroyCommand = registry.RegisterCommand(&ApplyDestroy);
    Components::AiAgent destroyed;
    destroyed.Program = MakeSingleTaskProgram(registry, registry.RegisterTask({nullptr, &TickDestroyThenRecord, nullptr}));
    Components::AiAgent recorded;
    recorded.Program = MakeSingleTaskProgram(registry, registry.RegisterTask({nullptr, &TickRecord, nullptr}));
    AIECS::AiTestWorld test(registry, 1);
    test.GetWorld().Create(destroyed);
    const ECS::EntityHandle survivor = test.GetWorld().Create(recorded).GetHandle();
    g_Applied.clear();

    test.Step(1);

    EXPECT_EQ(g_Applied, std::vector<uint32>{survivor.id});
}
