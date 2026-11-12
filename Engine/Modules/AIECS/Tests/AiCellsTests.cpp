#include "AIECS/AiProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsLarge.h"
#include "AIECS/Components/AiCells/AiCellsMedium.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
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

// Counts its ticks in cell 0 and writes the cell count to the last cell.
AIECS::AiTaskStatus TickCount(AIECS::AiTaskContext& context)
{
    const std::span<uint32> cells = context.GetCells();
    if (cells.empty())
        return AIECS::AiTaskStatus::Running;
    ++cells[0];
    cells.back() = static_cast<uint32>(cells.size());
    return AIECS::AiTaskStatus::Running;
}

// One option at a flat weight, so every agent runs TickCount on each decision.
uint16 MakeCountProgram(AIECS::AiRegistry& registry)
{
    AIECS::AiProgram program;
    program.Options.Options.push_back({1.0f, 0, 0, registry.RegisterTask({nullptr, &TickCount, nullptr}), 0});
    return registry.AddProgram(std::move(program));
}

AIECS::AiTaskStatus TickDone(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Succeeded;
}

AIECS::AiTaskStatus TickIdle(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Running;
}

// Option 0 (weight 1) finishes at once and cools down for 3 ticks in cell 0; option 1 idles at 0.5.
uint16 MakeCooldownProgram(AIECS::AiRegistry& registry)
{
    AIECS::AiProgram program;
    program.Options.Options.push_back({1.0f, 0, 0, registry.RegisterTask({nullptr, &TickDone, nullptr}), 0, 3});
    program.Options.Options.push_back({0.5f, 0, 0, registry.RegisterTask({nullptr, &TickIdle, nullptr}), 0});
    program.CooldownCell = 0;
    return registry.AddProgram(std::move(program));
}

} // namespace

// Option 0 runs on tick 1 and stops; its cooldown keeps it out on ticks 2 and 3 and lets it back on tick 4.
TEST(AiCellsTests, StoppedOptionCoolsDownInTheAgentCells)
{
    AIECS::AiRegistry registry;
    Components::AiAgent agent;
    agent.Program = MakeCooldownProgram(registry);
    ECS::World world;
    const ECS::EntityHandle entity = world.Create(agent, Components::AiCellsSmall{}).GetHandle();

    ECS::SystemScheduleBuilder builder;
    registry.AddSystemsToSchedule(builder);
    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Complete);

    manager.Update(world, kOneAiTickSeconds);
    EXPECT_EQ(world.GetComponent<Components::AiCellsSmall>(entity)->Values[0], 4u);
    manager.Update(world, kOneAiTickSeconds);
    EXPECT_EQ(world.GetComponent<Components::AiAgent>(entity)->ActivePath[0], 1);
    manager.Update(world, kOneAiTickSeconds);
    EXPECT_EQ(world.GetComponent<Components::AiAgent>(entity)->ActivePath[0], 1);
    manager.Update(world, kOneAiTickSeconds);
    EXPECT_EQ(world.GetComponent<Components::AiCellsSmall>(entity)->Values[0], 7u);
}

// A task reads and writes the cells of its own agent, whatever the size class; an agent with no
// cells gets an empty span.
TEST(AiCellsTests, TasksKeepMemoryInTheCellsOfTheirAgent)
{
    AIECS::AiRegistry registry;
    Components::AiAgent agent;
    agent.Program = MakeCountProgram(registry);
    ECS::World world;
    const ECS::EntityHandle small = world.Create(agent, Components::AiCellsSmall{}).GetHandle();
    const ECS::EntityHandle medium = world.Create(agent, Components::AiCellsMedium{}).GetHandle();
    const ECS::EntityHandle large = world.Create(agent, Components::AiCellsLarge{}).GetHandle();
    const ECS::EntityHandle none = world.Create(agent).GetHandle();

    ECS::SystemScheduleBuilder builder;
    registry.AddSystemsToSchedule(builder);
    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Complete);
    manager.Update(world, kOneAiTickSeconds);
    manager.Update(world, kOneAiTickSeconds);

    const auto& smallCells = world.GetComponent<Components::AiCellsSmall>(small)->Values;
    const auto& mediumCells = world.GetComponent<Components::AiCellsMedium>(medium)->Values;
    const auto& largeCells = world.GetComponent<Components::AiCellsLarge>(large)->Values;
    EXPECT_EQ(smallCells[0], 2u);
    EXPECT_EQ(smallCells.back(), 8u);
    EXPECT_EQ(mediumCells[0], 2u);
    EXPECT_EQ(mediumCells.back(), 20u);
    EXPECT_EQ(largeCells[0], 2u);
    EXPECT_EQ(largeCells.back(), 64u);
    EXPECT_EQ(world.GetComponent<Components::AiAgent>(none)->PathDepth, 1u);
}
