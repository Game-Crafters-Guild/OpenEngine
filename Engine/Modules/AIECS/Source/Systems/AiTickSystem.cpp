#include "Systems/AiTickSystem.h"

#include "AIECS/AiRegistry.h"
#include "AIECS/AiStateTreeProgram.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsLarge.h"
#include "AIECS/Components/AiCells/AiCellsMedium.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "AIECS/Components/AiClock.h"
#include "AIECS/Components/AiStateTree.h"
#include "AiGroup/AiGroupStore.h"
#include "ECS/ECSTemplates.h"
#include "Events/AiEventQueue.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace GameEngine::AIECS
{

AiTickSystem::AiTickSystem(AiRegistry& registry, std::shared_ptr<std::vector<AiDecisionRow>> rows)
    : m_Registry(registry), m_Rows(std::move(rows)), m_StateTrees(registry)
{
}

void AiTickSystem::Update(ECS::World& world, float32)
{
    Components::AiClock* clock = world.GetSingleton<Components::AiClock>();
    if (clock == nullptr)
    {
        world.SetSingleton(Components::AiClock{});
        clock = world.GetSingleton<Components::AiClock>();
    }
    const ECS::SimulationClock& simulation = world.GetSimulationClock();
    const float64 stepsPerTick = 1.0 / (simulation.GetFixedDeltaSeconds() * static_cast<float64>(clock->RateHz));
    clock->Divider = std::max<uint32>(1, static_cast<uint32>(std::lround(stepsPerTick)));
    const uint64 step = simulation.GetTick();
    clock->Tick = static_cast<uint32>(step / clock->Divider);
    clock->Due = step % clock->Divider == 0;
    if (!clock->Due)
        return;

    // Main thread, before the jobs of AiDecideSystem: the delivered events and the programs do not change while
    // the jobs read them.
    AiEventQueue::FindOrMakeForWorld(world.GetWorldId()).Deliver(clock->Tick);
    const uint32 tick = clock->Tick;
    m_Registry.ReloadStateTrees(world.GetWorldId(),
                                [this, &world, tick](uint16 id, const AiStateTreeProgram& oldProgram)
                                { RemapAgents(world, tick, id, oldProgram); });
}

void AiTickSystem::RemapAgents(ECS::World& world, uint32 tick, uint16 id, const AiStateTreeProgram& oldProgram)
{
    m_Remapped.clear();
    world
        .Query<ECS::Write<Components::AiAgent>, ECS::Write<Components::AiStateTree>,
               ECS::Optional<Components::AiCellsSmall>, ECS::Optional<Components::AiCellsMedium>,
               ECS::Optional<Components::AiCellsLarge>>()
        .Each(
            [this, id](ECS::EntityHandle entity, Components::AiAgent& agent, Components::AiStateTree& tree,
                       Components::AiCellsSmall* small, Components::AiCellsMedium* medium,
                       Components::AiCellsLarge* large)
            {
                if (agent.PathProgram == id)
                {
                    m_Remapped.push_back(
                        {&agent, &tree, SelectCells(small, medium, large), nullptr, nullptr, entity, id});
                }
            });
    std::sort(m_Remapped.begin(), m_Remapped.end(),
              [](const AiDueAgent& left, const AiDueAgent& right) { return left.Entity < right.Entity; });
    const AiStateTreeProgram& newProgram = *m_Registry.FindStateTree(id);
    const AiGroupStore* groups = AiGroupStore::FindForWorld(world.GetWorldId());
    for (const AiDueAgent& due : m_Remapped)
    {
        AiDecisionRow& row = m_Rows->emplace_back();
        row.Agent = due.Entity;
        m_StateTrees.RemapPath(world, due, oldProgram, newProgram, tick, row, groups);
    }
}

} // namespace GameEngine::AIECS
