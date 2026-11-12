// The world's AI events: a post reaches its agent or its group at the agent's next step, in a fixed order, once.
#include "AIECS/AiEvent.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/AiTestWorld.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeData.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "AIECS/Components/AiClock.h"
#include "AIECS/Components/AiStateTree.h"
#include "ECS/ECSTemplates.h"
#include "EngineLogCapture.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

// Idle waits for the Alarm event, then Alert runs. Both run Listen.
constexpr const char* kGuard = R"({ "root": { "id": 1, "name": "Guard", "children": [
    { "id": 2, "name": "Idle", "tasks": ["Listen"], "transitions": [ { "to": 3, "on": "event", "event": "Alarm" } ] },
    { "id": 3, "name": "Alert", "tasks": ["Listen"] } ] } })";
// One state that runs Shout.
constexpr const char* kShouter = R"({ "root": { "id": 1, "name": "Shout", "tasks": ["Shout"] } })";

// The agent id and the first payload word of each event a Listen task got at its enter or its tick, in call order.
std::vector<std::pair<uint32, uint32>> g_Heard;
// The target of each Shout.
ECS::EntityHandle g_ShoutTarget;
uint16 g_AlarmType = 0;

void EnterListen(AIECS::AiTaskContext& context)
{
    for (const AIECS::AiEvent& event : context.GetEvents())
        g_Heard.emplace_back(context.GetAgent().id, event.Payload[0]);
}

AIECS::AiTaskStatus TickListen(AIECS::AiTaskContext& context)
{
    EnterListen(context);
    return AIECS::AiTaskStatus::Running;
}

AIECS::AiTaskStatus TickShout(AIECS::AiTaskContext& context)
{
    context.PostEvent({.Type = g_AlarmType, .Target = g_ShoutTarget, .Payload = {context.GetAgent().id, 0}});
    return AIECS::AiTaskStatus::Running;
}

uint16 Compile(AIECS::AiRegistry& registry, const char* text)
{
    AIECS::AiStateTreeData data;
    std::string error;
    EXPECT_TRUE(AIECS::ParseAiStateTree(text, data, error)) << error;
    return registry.CompileStateTree(data);
}

struct Fixture
{
    AIECS::AiRegistry Registry;
    uint16 Guard = 0;
    uint16 Shouter = 0;
    Fixture()
    {
        g_Heard.clear();
        g_AlarmType = Registry.RegisterEventType("Alarm");
        Registry.RegisterTask({&EnterListen, &TickListen, nullptr, "Listen"});
        Registry.RegisterTask({nullptr, &TickShout, nullptr, "Shout"});
        Guard = Compile(Registry, kGuard);
        Shouter = Compile(Registry, kShouter);
    }
};

ECS::EntityHandle AddAgent(ECS::World& world, uint16 program, ECS::EntityHandle group = {})
{
    Components::AiAgent agent;
    agent.Program = program;
    Components::AiStateTree tree;
    tree.Group = group;
    return world.Create(agent, tree, Components::AiCellsSmall{}).GetHandle();
}

uint16 LeafOf(const ECS::World& world, ECS::EntityHandle entity)
{
    const Components::AiAgent& agent = *world.GetComponent<Components::AiAgent>(entity);
    return agent.ActivePath[agent.PathDepth - 1];
}

const Components::AiStateTree& TreeOf(const ECS::World& world, ECS::EntityHandle entity)
{
    return *world.GetComponent<Components::AiStateTree>(entity);
}

uint32 TickOf(const ECS::World& world)
{
    return world.GetSingleton<Components::AiClock>()->Tick;
}

constexpr uint16 kIdle = 1;
constexpr uint16 kAlert = 2;
constexpr uint32 kSlowCadence = 5;

} // namespace

TEST(AiEvents, AnEventFiresOnTheAgentsNextStepWhateverItsCadence)
{
    Fixture fixture;
    AIECS::AiTestWorld test(fixture.Registry, 1);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle due = AddAgent(world, fixture.Guard);
    const ECS::EntityHandle slow = AddAgent(world, fixture.Guard);
    world.GetComponentForWrite<Components::AiAgent>(slow)->Cadence = kSlowCadence;
    // Post right after a step of the slow agent, so that its next step is a full cadence later.
    test.Step();
    while (TreeOf(world, slow).LastStepTick != TickOf(world))
        test.Step();
    ASSERT_TRUE(AIECS::PostAiEvent(world, {.Type = g_AlarmType, .Target = due}));
    ASSERT_TRUE(AIECS::PostAiEvent(world, {.Type = g_AlarmType, .Target = slow, .Payload = {9, 0}}));
    EXPECT_EQ(LeafOf(world, due), kIdle);
    g_Heard.clear();
    test.Step();
    EXPECT_EQ(LeafOf(world, due), kAlert);
    for (uint32 tick = 1; tick < kSlowCadence; ++tick)
    {
        EXPECT_EQ(LeafOf(world, slow), kIdle) << tick;
        test.Step();
    }
    EXPECT_EQ(LeafOf(world, slow), kAlert);
    test.Step(2 * kSlowCadence);
    EXPECT_EQ(std::count(g_Heard.begin(), g_Heard.end(), std::pair<uint32, uint32>(slow.id, 9)), 1);
}

TEST(AiEvents, AnEventToAGroupReachesEveryMemberInEntityOrder)
{
    Fixture fixture;
    AIECS::AiTestWorld test(fixture.Registry, 1);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle group = world.CreateEntity();
    const ECS::EntityHandle first = AddAgent(world, fixture.Guard, group);
    const ECS::EntityHandle outsider = AddAgent(world, fixture.Guard);
    const ECS::EntityHandle second = AddAgent(world, fixture.Guard, group);
    test.Step();
    AIECS::PostAiEvent(world, {.Type = g_AlarmType, .Target = group, .Payload = {7, 0}});
    g_Heard.clear();
    test.Step();
    EXPECT_EQ(LeafOf(world, first), kAlert);
    EXPECT_EQ(LeafOf(world, second), kAlert);
    EXPECT_EQ(LeafOf(world, outsider), kIdle);
    EXPECT_EQ(g_Heard, (std::vector<std::pair<uint32, uint32>>{{first.id, 7}, {second.id, 7}}));
}

TEST(AiEvents, EventsOfOneTickArriveInSourceOrderWhateverThePostOrder)
{
    Fixture fixture;
    AIECS::AiTestWorld test(fixture.Registry, 1);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle low = world.CreateEntity();
    const ECS::EntityHandle high = world.CreateEntity();
    const ECS::EntityHandle listener = AddAgent(world, fixture.Guard);
    test.Step();
    AIECS::PostAiEvent(world, {.Type = g_AlarmType, .Source = high, .Target = listener, .Payload = {2, 0}});
    AIECS::PostAiEvent(world, {.Type = g_AlarmType, .Source = low, .Target = listener, .Payload = {1, 0}});
    g_Heard.clear();
    test.Step();
    EXPECT_EQ(g_Heard, (std::vector<std::pair<uint32, uint32>>{{listener.id, 1}, {listener.id, 2}}));
}

TEST(AiEvents, TaskPostsOnJobsArriveInEntityOrderAtTheNextTick)
{
    Fixture fixture;
    JobSystem::WorkStealingThreadPool jobs(4);
    AIECS::AiTestWorld test(fixture.Registry, 1, &jobs);
    ECS::World& world = test.GetWorld();
    g_ShoutTarget = AddAgent(world, fixture.Guard);
    std::vector<ECS::EntityHandle> shouters;
    for (uint32 index = 0; index < 200; ++index)
        shouters.push_back(AddAgent(world, fixture.Shouter));
    g_Heard.clear();
    test.Step();
    EXPECT_TRUE(g_Heard.empty()) << "a post reaches no agent on its own tick";
    g_Heard.clear();
    test.Step();
    ASSERT_EQ(g_Heard.size(), shouters.size());
    for (size_t index = 0; index < shouters.size(); ++index)
        EXPECT_EQ(g_Heard[index].second, shouters[index].id) << index;
}

TEST(AiEvents, AnEventAgesOutAfterTheLargestCadenceWhenItsAddresseeDoesNotStep)
{
    Fixture fixture;
    AIECS::AiTestWorld test(fixture.Registry, 1);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle agent = AddAgent(world, fixture.Guard);
    test.Step();
    // An agent with no program does not step; its path stays for the next step.
    world.GetComponentForWrite<Components::AiAgent>(agent)->Program = Components::AiAgent::kNoProgram;
    AIECS::PostAiEvent(world, {.Type = g_AlarmType, .Target = agent});
    test.Step(3);
    world.GetComponentForWrite<Components::AiAgent>(agent)->Program = fixture.Guard;
    test.Step();
    EXPECT_EQ(LeafOf(world, agent), kIdle);
}

TEST(AiEvents, AProgramThatNamesAnUnregisteredEventIsRefusedWithTheName)
{
    AIECS::AiRegistry registry;
    registry.RegisterTask({nullptr, &TickListen, nullptr, "Listen"});
    std::vector<std::string> lines;
    uint16 program = 0;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        program = Compile(registry, kGuard);
        Logger::Log::Flush();
    }
    EXPECT_EQ(program, Components::AiAgent::kNoProgram);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "the event 'Alarm'"), 1u);
}

TEST(AiEvents, OneScheduleThatUpdatesTwoWorldsKeepsTheEventsOfEach)
{
    using AIECS::AiTestWorld;
    Fixture fixture;
    ECS::SystemManager systems;
    AiTestWorld::AddAiSchedule(fixture.Registry, systems);
    ECS::World first;
    ECS::World second;
    const ECS::EntityHandle firstAgent = AddAgent(first, fixture.Guard);
    const ECS::EntityHandle secondAgent = AddAgent(second, fixture.Guard);
    uint32 firstTick = AiTestWorld::StepSchedule(systems, first, AiTestWorld::kNoTick);
    uint32 secondTick = AiTestWorld::StepSchedule(systems, second, AiTestWorld::kNoTick);

    ASSERT_TRUE(AIECS::PostAiEvent(first, {.Type = g_AlarmType, .Target = firstAgent}));
    ASSERT_TRUE(AIECS::PostAiEvent(second, {.Type = g_AlarmType, .Target = secondAgent}));
    firstTick = AiTestWorld::StepSchedule(systems, first, firstTick);
    secondTick = AiTestWorld::StepSchedule(systems, second, secondTick);

    EXPECT_EQ(LeafOf(first, firstAgent), kAlert);
    EXPECT_EQ(LeafOf(second, secondAgent), kAlert);
}

TEST(AiEvents, TwoSchedulesOnOneWorldDeliverAnEventOnceAndTheQueueOutlivesEither)
{
    using AIECS::AiTestWorld;
    Fixture fixture;
    ECS::SystemManager systems;
    auto extra = std::make_unique<ECS::SystemManager>();
    AiTestWorld::AddAiSchedule(fixture.Registry, systems);
    AiTestWorld::AddAiSchedule(fixture.Registry, *extra);
    ECS::World world;
    const ECS::EntityHandle agent = AddAgent(world, fixture.Guard);
    uint32 tick = AiTestWorld::StepSchedule(systems, world, AiTestWorld::kNoTick);
    tick = AiTestWorld::StepSchedule(*extra, world, tick);

    ASSERT_TRUE(AIECS::PostAiEvent(world, {.Type = g_AlarmType, .Target = agent, .Payload = {7, 0}}));
    for (uint32 step = 0; step < kSlowCadence; ++step)
    {
        tick = AiTestWorld::StepSchedule(systems, world, tick);
        tick = AiTestWorld::StepSchedule(*extra, world, tick);
    }
    EXPECT_EQ(LeafOf(world, agent), kAlert);
    EXPECT_EQ(std::count(g_Heard.begin(), g_Heard.end(), std::pair<uint32, uint32>(agent.id, 7)), 1);

    extra.reset();
    AiTestWorld::StepSchedule(systems, world, tick);
    EXPECT_TRUE(AIECS::PostAiEvent(world, {.Type = g_AlarmType, .Target = agent}));
}
