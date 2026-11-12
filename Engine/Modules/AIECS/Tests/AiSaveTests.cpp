// A scene save keeps an agent's program, schedule, cooldowns and entity cells, and a group's
// records. A load into a fresh world points the entity references at the loaded entities, so
// the loaded agent makes the same decisions as the saved one.

#include "AIECS/AiGroupKnowledge.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/AiTestWorld.h"
#include "AIECS/Assets/AiOptionSetData.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "AIECS/Components/AiClock.h"
#include "AIECS/Components/AiGroup.h"
#include "AIECS/Components/AiTrace.h"
#include "AI/RecordStore.h"
#include "AssetCore/GUID.h"
#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EngineLogCapture.h"
#include "Logger/Logger.h"
#include "Scene/SceneIO.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr uint64 kSeed = 7;
constexpr uint16 kTargetCell = 2;
constexpr uint16 kCooldownCell = 4;
constexpr float32 kHuntScore = 0.45f;
constexpr uint32 kComparedTicks = 12;

// The sensor sees these subjects, owned by g_Owner, while g_Seeing is true.
bool g_Seeing = false;
std::array<ECS::EntityHandle, 2> g_Subjects{};
ECS::EntityHandle g_Owner;

void SeeSubjects(ECS::World&, ECS::EntityHandle, AI::RecordStore& records, uint32 tick)
{
    if (!g_Seeing)
        return;
    for (const ECS::EntityHandle subject : g_Subjects)
    {
        AI::RecordStore::Sighting sighting;
        sighting.Subject = subject.id;
        sighting.Owner = g_Owner.id;
        sighting.Strength = 0.5f;
        records.Observe(sighting, tick);
    }
}

// kHuntScore when the entity in the agent's target cell is the entity named "Target", else 0.
void ReadTarget(const ECS::World& world, std::span<const ECS::EntityHandle> agents, std::span<float32> out)
{
    for (size_t index = 0; index < agents.size(); ++index)
    {
        const Components::AiCellsSmall* cells = world.GetComponent<Components::AiCellsSmall>(agents[index]);
        const ECS::EntityHandle target(cells->Values[kTargetCell]);
        const Components::Name* name = target.IsValid() ? world.GetComponent<Components::Name>(target) : nullptr;
        out[index] = name != nullptr && name->View() == "Target" ? kHuntScore : 0.0f;
    }
}

AIECS::AiTaskStatus TickHunt(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Running;
}

AIECS::AiTaskStatus TickWait(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Succeeded;
}

// The scene schemas find programs in the global registry, so the test registers there once.
void RegisterOnce()
{
    static const bool registered = []
    {
        AIECS::AiRegistry& registry = AIECS::AiRegistry::Get();
        registry.RegisterInput(&ReadTarget, AIECS::AiInputThreading::CallingThread, "SaveTest.Target");
        registry.RegisterTask({.Tick = &TickHunt, .Name = "SaveTest.Hunt"});
        registry.RegisterTask({.Tick = &TickWait, .Name = "SaveTest.Wait"});
        registry.RegisterSensor(&SeeSubjects);
        return true;
    }();
    (void)registered;
}

// Option 0 hunts with the target score; option 1 waits at a flat 0.5 and then cools down for 10
// ticks. With a noise margin, both options wait and each decision draws between them.
uint16 CompileProgram(float32 noiseMargin)
{
    RegisterOnce();
    AIECS::AiOptionSetData data;
    data.InputNames = {"SaveTest.Target"};
    data.Options.NoiseMargin = noiseMargin;
    data.Options.Curves.push_back(AI::ResponseCurve::FromPreset(AI::CurvePreset::Linear, 1.0f, 0.0f));
    data.Options.Considerations.push_back({0.0f, 1.0f, 0, 0});
    if (noiseMargin > 0.0f)
    {
        data.TaskNames = {"SaveTest.Wait", "SaveTest.Wait"};
        data.Options.Options.push_back({.Weight = 1.0f, .ConsiderationCount = 1, .Action = 0});
        data.Options.Options.push_back({.Weight = 0.5f, .Action = 1});
    }
    else
    {
        data.TaskNames = {"SaveTest.Hunt", "SaveTest.Wait"};
        data.Options.Options.push_back({.Weight = 1.0f, .ConsiderationCount = 1, .Action = 0});
        data.Options.Options.push_back({.Weight = 0.5f, .Action = 1, .CooldownTicks = 10});
        data.CooldownCell = kCooldownCell;
    }
    data.EntityCells = {kTargetCell};
    data.StableId = GUID::Generate();
    return AIECS::AiRegistry::Get().CompileProgram(data);
}

ECS::EntityHandle CreateNamed(ECS::World& world, std::string_view name)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::Name component{};
    name.copy(component.value, sizeof(component.value) - 1);
    world.AddComponentImmediate(entity, component);
    return entity;
}

ECS::EntityHandle FindNamed(ECS::World& world, std::string_view name)
{
    ECS::EntityHandle found;
    world.Query<ECS::Read<Components::Name>>().Each(
        [&found, name](ECS::EntityHandle entity, const Components::Name& entityName)
        {
            if (entityName.View() == name)
                found = entity;
        });
    return found;
}

void AddTrace(ECS::World& world, ECS::EntityHandle agent)
{
    world.AddComponentImmediate(agent, Components::AiTrace{});
}

class AiSaveTests : public testing::TestWithParam<float32>
{
protected:
    TestUtils::ScopedTempDir m_Directory{TestUtils::MakeUniqueTempDirectory("AiSaveTests")};
};

} // namespace

// The loaded world keeps the agent, its cells and the group's records, with every entity
// reference on the loaded entity, and the agent then decides as the saved agent does.
TEST_P(AiSaveTests, LoadedAgentDecidesAsTheSavedAgent)
{
    const uint16 program = CompileProgram(GetParam());
    ASSERT_NE(program, Components::AiAgent::kNoProgram);

    AIECS::AiTestWorld saved(AIECS::AiRegistry::Get(), kSeed);
    ECS::World& world = saved.GetWorld();
    // These entities go away, so the saved entities have ids that a fresh world does not give them.
    for (uint32 filler = 0; filler < 5; ++filler)
        world.DestroyEntityImmediate(world.CreateEntity());

    const ECS::EntityHandle target = CreateNamed(world, "Target");
    const ECS::EntityHandle other = CreateNamed(world, "Other");
    const ECS::EntityHandle agent = CreateNamed(world, "Agent");
    world.AddComponentImmediate(agent, Components::AiAgent{.Program = program});
    Components::AiCellsSmall cells;
    cells.Values[kTargetCell] = target.id;
    world.AddComponentImmediate(agent, cells);
    const ECS::EntityHandle group = CreateNamed(world, "Group");
    Components::AiGroup groupSettings;
    groupSettings.IdsAreEntities = true;
    world.AddComponentImmediate(group, groupSettings);

    g_Subjects = {other, target};
    g_Owner = target;
    g_Seeing = true;
    saved.Step(3);
    g_Seeing = false;
    saved.Step(2);

    const std::filesystem::path path = m_Directory.Path() / "saved.scene";
    Scene::SceneWriteOptions options;
    options.writeSimulationTick = true;
    ASSERT_TRUE(Scene::WriteSceneFile(world, path, options));

    AIECS::AiTestWorld loaded(AIECS::AiRegistry::Get(), kSeed);
    ECS::World& loadedWorld = loaded.GetWorld();
    const Scene::SceneInstantiateResult result = Scene::ReadSceneInto(loadedWorld, path);
    ASSERT_TRUE(result) << result.Error->message;
    Components::AiClock clock;
    clock.Seed = kSeed;
    loadedWorld.SetSingleton(clock);

    const ECS::EntityHandle loadedTarget = FindNamed(loadedWorld, "Target");
    const ECS::EntityHandle loadedOther = FindNamed(loadedWorld, "Other");
    const ECS::EntityHandle loadedAgent = FindNamed(loadedWorld, "Agent");
    const ECS::EntityHandle loadedGroup = FindNamed(loadedWorld, "Group");
    ASSERT_TRUE(loadedTarget.IsValid() && loadedOther.IsValid() && loadedAgent.IsValid() && loadedGroup.IsValid());
    ASSERT_NE(loadedTarget.id, target.id) << "the fixture needs ids that change in the load";

    const Components::AiAgent* savedAgent = world.GetComponent<Components::AiAgent>(agent);
    const Components::AiAgent* loadedAgentState = loadedWorld.GetComponent<Components::AiAgent>(loadedAgent);
    ASSERT_NE(loadedAgentState, nullptr);
    EXPECT_EQ(loadedAgentState->Program, program);
    EXPECT_EQ(loadedAgentState->PathProgram, program);
    EXPECT_EQ(loadedAgentState->NextTick, savedAgent->NextTick);
    EXPECT_EQ(loadedAgentState->DrawSeed, savedAgent->DrawSeed);
    EXPECT_EQ(loadedAgentState->PathDepth, savedAgent->PathDepth);
    EXPECT_EQ(loadedAgentState->ActivePath[0], savedAgent->ActivePath[0]);

    const Components::AiCellsSmall* loadedCells = loadedWorld.GetComponent<Components::AiCellsSmall>(loadedAgent);
    ASSERT_NE(loadedCells, nullptr);
    EXPECT_EQ(loadedCells->Values[kTargetCell], loadedTarget.id);
    const Components::AiCellsSmall* savedCells = world.GetComponent<Components::AiCellsSmall>(agent);
    EXPECT_EQ(loadedCells->Values[kCooldownCell + 1], savedCells->Values[kCooldownCell + 1]);

    const AI::RecordStore* savedRecords = AIECS::FindGroupRecords(world, group);
    const AI::RecordStore* loadedRecords = AIECS::FindGroupRecords(loadedWorld, loadedGroup);
    ASSERT_NE(savedRecords, nullptr);
    ASSERT_NE(loadedRecords, nullptr);
    ASSERT_EQ(loadedRecords->GetCount(), 2u);
    EXPECT_EQ(loadedRecords->GetSubjects()[0], loadedOther.id);
    EXPECT_EQ(loadedRecords->GetSubjects()[1], loadedTarget.id);
    for (uint32 index = 0; index < 2; ++index)
    {
        EXPECT_EQ(loadedRecords->GetFirstSeen()[index], savedRecords->GetFirstSeen()[index]);
        EXPECT_EQ(loadedRecords->GetLastSeen()[index], savedRecords->GetLastSeen()[index]);
        EXPECT_EQ(loadedRecords->GetOwners()[index], loadedTarget.id);
        EXPECT_FLOAT_EQ(loadedRecords->GetStrengths()[index], 0.5f);
    }

    AddTrace(world, agent);
    AddTrace(loadedWorld, loadedAgent);
    saved.Step(kComparedTicks);
    loaded.Step(kComparedTicks);
    const Components::AiTrace& savedTrace = *world.GetComponent<Components::AiTrace>(agent);
    const Components::AiTrace& loadedTrace = *loadedWorld.GetComponent<Components::AiTrace>(loadedAgent);
    EXPECT_EQ(savedTrace.Count, kComparedTicks);
    EXPECT_TRUE(AIECS::AiTestWorld::SameTrace(savedTrace, loadedTrace));
}

INSTANTIATE_TEST_SUITE_P(NoiseMargin, AiSaveTests, testing::Values(0.0f, 0.3f));

// A save cannot name a program that has no stable id: it warns once for the program, not once per agent.
TEST(AiSaveWarningTests, ProgramWithNoStableIdWarnsOnce)
{
    RegisterOnce();
    AIECS::AiOptionSetData data;
    data.TaskNames = {"SaveTest.Wait"};
    data.Options.Options.push_back({.Action = 0});
    const uint16 program = AIECS::AiRegistry::Get().CompileProgram(data);
    ASSERT_NE(program, Components::AiAgent::kNoProgram);
    AIECS::AiTestWorld test(AIECS::AiRegistry::Get(), kSeed);
    for (uint32 index = 0; index < 2; ++index)
        test.GetWorld().AddComponentImmediate(CreateNamed(test.GetWorld(), "Agent"),
                                              Components::AiAgent{.Program = program});

    const TestUtils::ScopedTempDir directory{TestUtils::MakeUniqueTempDirectory("AiSaveWarningTests")};
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        const Scene::SceneWriteOptions options;
        ASSERT_TRUE(Scene::WriteSceneFile(test.GetWorld(), directory.Path() / "saved.scene", options));
        Logger::Log::Flush();
    }
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "program " + std::to_string(program) + " has no stable id"), 1u);
}
