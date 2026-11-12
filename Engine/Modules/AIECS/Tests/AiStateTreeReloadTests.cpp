// The reload of a state-tree program file: the next AI tick moves the running agents to the new program by
// stable id, and the cells of the agents stay in range.
#include "AIECS/AiRegistry.h"
#include "AIECS/AiStateTreeProgram.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/AiTestWorld.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeAsset.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "AIECS/Components/AiStateTree.h"
#include "ECS/ECSTemplates.h"
#include "EngineLogCapture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{

// Guard > Watch > Hold, each with a cooldown, so the program has two cooldown slots.
constexpr const char* kOld = R"({ "root": { "id": 1, "name": "Guard", "children": [
    { "id": 2, "name": "Watch", "modifiers": [ { "kind": "cooldown", "ticks": 40 } ], "children": [
        { "id": 3, "name": "Hold", "tasks": ["Hold"], "modifiers": [ { "kind": "cooldown", "ticks": 40 } ] } ] } ] } })";
// Hold is gone; Rest comes before Watch, so Watch has another index; one cooldown slot.
constexpr const char* kNew = R"({ "root": { "id": 1, "name": "Guard", "children": [
    { "id": 4, "name": "Rest", "tasks": ["Hold"] },
    { "id": 2, "name": "Watch", "tasks": ["Hold"], "modifiers": [ { "kind": "cooldown", "ticks": 40 } ] } ] } })";

constexpr uint32 kWatchId = 2;
constexpr uint32 kFrontValue = 0xA5A5A5A5u;

std::vector<AIECS::AiTaskExitReason> g_Exits;

AIECS::AiTaskStatus TickHold(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Running;
}

void ExitHold(AIECS::AiTaskContext&, AIECS::AiTaskExitReason reason)
{
    g_Exits.push_back(reason);
}

} // namespace

// In AIECS, so that AiRegistry can name it a friend.
namespace GameEngine::AIECS
{

class AiStateTreeReload : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_Exits.clear();
        m_Root = std::filesystem::temp_directory_path() / ("ai-state-tree-reload-" + GUID::Generate().ToString());
        std::filesystem::create_directories(m_Root);
        m_Path = m_Root / "guard.scribe";
        Write(kOld);
        m_Asset = std::make_shared<AIECS::AiStateTreeAsset>(GUID::Generate(), m_Path);
        ASSERT_TRUE(m_Asset->Load());
        m_Registry.RegisterTask({nullptr, &TickHold, &ExitHold, "Hold"});
        m_Program = m_Registry.LoadStateTree(m_Asset);
        ASSERT_NE(m_Program, Components::AiAgent::kNoProgram);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(m_Root, error);
    }

    void Write(const char* text)
    {
        std::ofstream file(m_Path, std::ios::binary | std::ios::trunc);
        file << text;
        file.close();
        ASSERT_TRUE(file);
    }

    ECS::EntityHandle AddAgent(ECS::World& world, uint16 program)
    {
        Components::AiAgent agent;
        agent.Program = program;
        Components::AiCellsSmall cells;
        cells.Values.fill(kFrontValue);
        return world.Create(agent, Components::AiStateTree{}, cells).GetHandle();
    }

    // The number of worlds that have a remap mark in m_Registry.
    size_t CountRemappedWorlds() const { return m_Registry.m_RemappedGenerations.Size(); }

    std::filesystem::path m_Root;
    std::filesystem::path m_Path;
    SharedPtr<AIECS::AiStateTreeAsset> m_Asset;
    AIECS::AiRegistry m_Registry;
    uint16 m_Program = 0;
};

} // namespace GameEngine::AIECS

using AIECS::AiStateTreeReload;

TEST_F(AiStateTreeReload, AnEditedFileKeepsThePathByStableIdAndTheLostStateExitsWithReloaded)
{
    AIECS::AiTestWorld test(m_Registry, 1);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle entity = AddAgent(world, m_Program);
    test.Step();
    ASSERT_EQ(world.GetComponent<Components::AiAgent>(entity)->PathDepth, 3u);

    Write(kNew);
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    test.Step();

    const Components::AiAgent& agent = *world.GetComponent<Components::AiAgent>(entity);
    ASSERT_EQ(agent.PathDepth, 2u);
    const auto states = m_Registry.FindStateTree(m_Program)->Program.GetStates();
    EXPECT_EQ(states[agent.ActivePath[1]].StableId, kWatchId);
    ASSERT_FALSE(g_Exits.empty());
    EXPECT_EQ(g_Exits.front(), AIECS::AiTaskExitReason::Reloaded);
}

TEST_F(AiStateTreeReload, AnOldProgramWithMoreCooldownSlotsWritesOnlyTheAgentsCooldownCells)
{
    AIECS::AiTestWorld test(m_Registry, 1);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle entity = AddAgent(world, m_Program);
    // Same components, no program: the cells after the agent's cells in memory.
    const ECS::EntityHandle neighbor = AddAgent(world, Components::AiAgent::kNoProgram);
    test.Step();

    Write(kNew);
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    test.Step();

    const auto& cells = world.GetComponent<Components::AiCellsSmall>(entity)->Values;
    const size_t taskCells = cells.size() - 2;
    EXPECT_TRUE(
        std::all_of(cells.begin(), cells.begin() + taskCells, [](uint32 value) { return value == kFrontValue; }));
    const auto& neighborCells = world.GetComponent<Components::AiCellsSmall>(neighbor)->Values;
    EXPECT_TRUE(
        std::all_of(neighborCells.begin(), neighborCells.end(), [](uint32 value) { return value == kFrontValue; }));
}

TEST_F(AiStateTreeReload, APathWithAStateOutsideTheOldProgramStartsTheNewProgramAndLogsTheState)
{
    AIECS::AiTestWorld test(m_Registry, 1);
    ECS::World& world = test.GetWorld();
    const ECS::EntityHandle entity = AddAgent(world, m_Program);
    test.Step();
    world.GetComponentForWrite<Components::AiStateTree>(entity)->State.Path[2].State = 77;

    Write(kNew);
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        test.Step();
        Logger::Log::Flush();
    }
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "holds state 77"), 1u);
    EXPECT_TRUE(g_Exits.empty()) << "no task of the path exits";
    const Components::AiAgent& agent = *world.GetComponent<Components::AiAgent>(entity);
    ASSERT_GE(agent.PathDepth, 2u);
    EXPECT_EQ(m_Registry.FindStateTree(m_Program)->Program.GetStates()[agent.ActivePath[1]].StableId, 4u);
}

TEST_F(AiStateTreeReload, ATickWithNoRetiredProgramForgetsTheWorldsThatAreGone)
{
    {
        AIECS::AiTestWorld gone(m_Registry, 1);
        gone.Step();
    }
    AIECS::AiTestWorld live(m_Registry, 1);
    live.Step();
    EXPECT_EQ(CountRemappedWorlds(), 1u);
}

TEST_F(AiStateTreeReload, AWorldThatTicksAfterTheReloadMovesItsAgentsFromTheOldProgram)
{
    AIECS::AiTestWorld first(m_Registry, 1);
    AIECS::AiTestWorld second(m_Registry, 1);
    const ECS::EntityHandle firstAgent = AddAgent(first.GetWorld(), m_Program);
    const ECS::EntityHandle secondAgent = AddAgent(second.GetWorld(), m_Program);
    first.Step();
    second.Step();

    Write(kNew);
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    first.Step();
    second.Step();

    const auto states = m_Registry.FindStateTree(m_Program)->Program.GetStates();
    for (const auto& [world, entity] :
         {std::pair(&first.GetWorld(), firstAgent), std::pair(&second.GetWorld(), secondAgent)})
    {
        const Components::AiAgent& agent = *world->GetComponent<Components::AiAgent>(entity);
        ASSERT_EQ(agent.PathDepth, 2u);
        EXPECT_EQ(states[agent.ActivePath[1]].StableId, kWatchId);
    }
    EXPECT_EQ(std::count(g_Exits.begin(), g_Exits.end(), AIECS::AiTaskExitReason::Reloaded), 2);
}
