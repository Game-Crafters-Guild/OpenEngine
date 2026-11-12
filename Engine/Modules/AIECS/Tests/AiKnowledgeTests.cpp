#include "AIECS/AiCommand.h"
#include "AIECS/AiGroupKnowledge.h"
#include "AIECS/AiProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/AiTestWorld.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiGroup.h"
#include "AI/RecordStore.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr uint64 kSeed = 1;

struct TestSubject
{
    uint32 Id = 0;
};

// Sees one subject per group, with the group's own entity id as the subject.
void SeeOwnId(ECS::World&, ECS::EntityHandle group, AI::RecordStore& records, uint32 tick)
{
    AI::RecordStore::Sighting sighting;
    sighting.Subject = group.id;
    records.Observe(sighting, tick);
}

// Sees the subject in the group's TestSubject.
void SeeTestSubject(ECS::World& world, ECS::EntityHandle group, AI::RecordStore& records, uint32 tick)
{
    AI::RecordStore::Sighting sighting;
    sighting.Subject = world.GetComponent<TestSubject>(group)->Id;
    records.Observe(sighting, tick);
}

// Sees subject 7 on the first AI tick only.
void SeeOnceOnFirstTick(ECS::World&, ECS::EntityHandle, AI::RecordStore& records, uint32 tick)
{
    if (tick != 1)
        return;
    AI::RecordStore::Sighting sighting;
    sighting.Subject = 7;
    records.Observe(sighting, tick);
}

struct TestMember
{
    ECS::EntityHandle Group;
};

struct TestSaw
{
    uint32 Count = 0;
};

uint16 g_SawCommand = 0;
std::thread::id g_CallingThread;
std::atomic<uint32> g_JobThreadReads{0};

// The calling thread can claim every chunk when the workers start late. It waits here, so a worker
// takes the other chunks. The time limit stops a hang when no worker runs.
void WaitForAJobThreadRead()
{
    constexpr std::chrono::seconds kTimeLimit{5};
    const auto start = std::chrono::steady_clock::now();
    while (g_JobThreadReads.load() == 0 && std::chrono::steady_clock::now() - start < kTimeLimit)
        std::this_thread::yield();
}

// Orders a TestSaw count when the agent's group has seen its own id.
AIECS::AiTaskStatus TickReadGroup(AIECS::AiTaskContext& context)
{
    const ECS::EntityHandle group = context.GetWorld().GetComponent<TestMember>(context.GetAgent())->Group;
    const AI::RecordStore* records = context.FindGroupRecords(group);
    if (std::this_thread::get_id() != g_CallingThread)
        g_JobThreadReads.fetch_add(1);
    else
        WaitForAJobThreadRead();
    if (records != nullptr && records->GetCount() == 1 && records->GetSubjects()[0] == group.id)
        context.IssueCommand({context.GetAgent(), 1, g_SawCommand});
    return AIECS::AiTaskStatus::Running;
}

void ApplySaw(ECS::World& world, ECS::EntityHandle, const AIECS::AiCommand& command)
{
    world.GetComponentForWrite<TestSaw>(command.Target)->Count += static_cast<uint32>(command.Argument);
}

} // namespace

// The sensor gets each group's own records, so two groups do not share what they see.
TEST(AiKnowledgeTests, SensorsWriteIntoTheRecordsOfEachGroup)
{
    AIECS::AiRegistry registry;
    registry.RegisterSensor(&SeeOwnId);
    AIECS::AiTestWorld test(registry, kSeed);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle first = world.Create(Components::AiGroup{}).GetHandle();
    const ECS::EntityHandle second = world.Create(Components::AiGroup{}).GetHandle();

    test.Step();

    const AI::RecordStore* firstRecords = AIECS::FindGroupRecords(world, first);
    const AI::RecordStore* secondRecords = AIECS::FindGroupRecords(world, second);
    ASSERT_NE(firstRecords, nullptr);
    ASSERT_NE(secondRecords, nullptr);
    ASSERT_EQ(firstRecords->GetCount(), 1u);
    ASSERT_EQ(secondRecords->GetCount(), 1u);
    EXPECT_EQ(firstRecords->GetSubjects()[0], first.id);
    EXPECT_EQ(secondRecords->GetSubjects()[0], second.id);
    EXPECT_EQ(firstRecords->GetFirstSeen()[0], 1u);
}

// With a forget age of 2, a record seen on tick 1 stays through tick 3 and is gone on tick 4.
TEST(AiKnowledgeTests, KnowledgeForgetsRecordsOlderThanTheForgetAge)
{
    AIECS::AiRegistry registry;
    registry.RegisterSensor(&SeeOnceOnFirstTick);
    AIECS::AiTestWorld test(registry, kSeed);
    Components::AiGroup group;
    group.ForgetTicks = 2;
    group.PlanCells = 3;
    const ECS::EntityHandle entity = test.GetWorld().Create(group).GetHandle();

    test.Step(3);
    ASSERT_NE(AIECS::FindGroupRecords(test.GetWorld(), entity), nullptr);
    EXPECT_EQ(AIECS::FindGroupRecords(test.GetWorld(), entity)->GetCount(), 1u);
    ASSERT_EQ(AIECS::FindGroupPlan(test.GetWorld(), entity).size(), 3u);
    EXPECT_EQ(AIECS::FindGroupPlan(test.GetWorld(), entity)[2], 0u);

    test.Step();
    EXPECT_EQ(AIECS::FindGroupRecords(test.GetWorld(), entity)->GetCount(), 0u);
}

// The world frees a group's knowledge when the group entity dies or loses its AiGroup.
TEST(AiKnowledgeTests, RemovedGroupFreesItsKnowledge)
{
    AIECS::AiRegistry registry;
    AIECS::AiTestWorld test(registry, kSeed);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle dies = world.Create(Components::AiGroup{}).GetHandle();
    const ECS::EntityHandle leaves = world.Create(Components::AiGroup{}).GetHandle();
    test.Step();
    ASSERT_EQ(test.CountGroupKnowledge(), 2u);

    world.DestroyEntityImmediate(dies);
    EXPECT_EQ(test.CountGroupKnowledge(), 1u);
    world.RemoveComponentImmediate<Components::AiGroup>(leaves);
    EXPECT_EQ(test.CountGroupKnowledge(), 0u);
    EXPECT_EQ(AIECS::FindGroupRecords(world, leaves), nullptr);
}

// A world reset frees the knowledge of every group. World::Clear is the only path that moves the
// lifecycle reset generation, and it runs the removal hooks first.
TEST(AiKnowledgeTests, ClearedWorldFreesAllKnowledge)
{
    AIECS::AiRegistry registry;
    AIECS::AiTestWorld test(registry, kSeed);
    ECS::World& world = test.GetWorld();
    world.Create(Components::AiGroup{});
    world.Create(Components::AiGroup{});
    test.Step();
    ASSERT_EQ(test.CountGroupKnowledge(), 2u);

    world.Clear();
    EXPECT_EQ(test.CountGroupKnowledge(), 0u);
}

// A copied AiGroup gets knowledge of its own, and its removal leaves the original's knowledge.
TEST(AiKnowledgeTests, CopiedGroupGetsItsOwnKnowledge)
{
    AIECS::AiRegistry registry;
    registry.RegisterSensor(&SeeOwnId);
    AIECS::AiTestWorld test(registry, kSeed);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle original = world.Create(Components::AiGroup{}).GetHandle();
    test.Step();
    const ECS::EntityHandle copy = world.Create(*world.GetComponent<Components::AiGroup>(original)).GetHandle();

    test.Step();
    ASSERT_EQ(test.CountGroupKnowledge(), 2u);
    ASSERT_EQ(AIECS::FindGroupRecords(world, original)->GetCount(), 1u);
    ASSERT_EQ(AIECS::FindGroupRecords(world, copy)->GetCount(), 1u);
    EXPECT_EQ(AIECS::FindGroupRecords(world, original)->GetSubjects()[0], original.id);
    EXPECT_EQ(AIECS::FindGroupRecords(world, copy)->GetSubjects()[0], copy.id);

    world.DestroyEntityImmediate(copy);
    EXPECT_EQ(test.CountGroupKnowledge(), 1u);
    EXPECT_NE(AIECS::FindGroupRecords(world, original), nullptr);
}

// A copy removed before its first AI tick still names the original's entry, and leaves it.
TEST(AiKnowledgeTests, CopyRemovedBeforeItsFirstTickLeavesTheOriginalsKnowledge)
{
    AIECS::AiRegistry registry;
    AIECS::AiTestWorld test(registry, kSeed);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle original = world.Create(Components::AiGroup{}).GetHandle();
    test.Step();
    const ECS::EntityHandle copy = world.Create(*world.GetComponent<Components::AiGroup>(original)).GetHandle();

    world.DestroyEntityImmediate(copy);
    EXPECT_EQ(test.CountGroupKnowledge(), 1u);
    EXPECT_NE(AIECS::FindGroupRecords(world, original), nullptr);
}

// An AiGroup copied into another world gets that world's knowledge, not the first world's.
TEST(AiKnowledgeTests, TwoWorldsDoNotShareKnowledge)
{
    AIECS::AiRegistry registry;
    registry.RegisterSensor(&SeeTestSubject);
    AIECS::AiTestWorld first(registry, kSeed);
    AIECS::AiTestWorld second(registry, kSeed);
    const ECS::EntityHandle firstGroup = first.GetWorld().Create(Components::AiGroup{}, TestSubject{11}).GetHandle();
    first.Step();
    const Components::AiGroup copied = *first.GetWorld().GetComponent<Components::AiGroup>(firstGroup);
    const ECS::EntityHandle secondGroup = second.GetWorld().Create(copied, TestSubject{22}).GetHandle();

    first.Step();
    second.Step();

    EXPECT_EQ(first.CountGroupKnowledge(), 1u);
    EXPECT_EQ(second.CountGroupKnowledge(), 1u);
    const AI::RecordStore* firstRecords = AIECS::FindGroupRecords(first.GetWorld(), firstGroup);
    const AI::RecordStore* secondRecords = AIECS::FindGroupRecords(second.GetWorld(), secondGroup);
    ASSERT_NE(firstRecords, nullptr);
    ASSERT_NE(secondRecords, nullptr);
    ASSERT_EQ(firstRecords->GetCount(), 1u);
    ASSERT_EQ(secondRecords->GetCount(), 1u);
    EXPECT_EQ(firstRecords->GetSubjects()[0], 11u);
    EXPECT_EQ(secondRecords->GetSubjects()[0], 22u);
}

// Tasks on job threads read the records of their group through the task context: each agent
// sees its own group's subject on each AI tick.
TEST(AiKnowledgeTests, TasksReadTheirGroupsRecordsOnJobThreads)
{
    constexpr uint32 kAgentCount = 300;
    constexpr uint32 kTicks = 2;
    AIECS::AiRegistry registry;
    registry.RegisterSensor(&SeeOwnId);
    g_SawCommand = registry.RegisterCommand(&ApplySaw);
    AIECS::AiProgram program;
    program.Options.Options.push_back({.Action = registry.RegisterTask({nullptr, &TickReadGroup, nullptr})});
    Components::AiAgent agent;
    agent.Program = registry.AddProgram(std::move(program));
    JobSystem::WorkStealingThreadPool jobs(2);
    AIECS::AiTestWorld test(registry, kSeed, &jobs);
    ECS::World& world = test.GetWorld();
    const std::array<ECS::EntityHandle, 2> groups{world.Create(Components::AiGroup{}).GetHandle(),
                                                  world.Create(Components::AiGroup{}).GetHandle()};
    std::vector<ECS::EntityHandle> agents;
    for (uint32 index = 0; index < kAgentCount; ++index)
        agents.push_back(world.Create(agent, TestMember{groups[index % 2]}, TestSaw{}).GetHandle());
    g_CallingThread = std::this_thread::get_id();
    g_JobThreadReads = 0;

    test.Step(kTicks);

    EXPECT_GT(g_JobThreadReads.load(), 0u);
    for (const ECS::EntityHandle member : agents)
        EXPECT_EQ(world.GetComponent<TestSaw>(member)->Count, kTicks) << member.index;
}
