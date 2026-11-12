#pragma once

#include "AiDecisionRow.h"
#include "AiDueAgent.h"
#include "ECS/Systems.h"
#include "StateTree/AiStateTreeDecider.h"

#include <memory>
#include <vector>

namespace GameEngine::AIECS
{

class AiRegistry;
struct AiStateTreeProgram;

/// Publishes the AI clock (Components::AiClock) from the world's SimulationClock: the AI tick
/// and whether this step starts one. Keeps no time of its own. At the start of each AI tick it delivers
/// the world's AI events and applies the reloads of the state-tree programs, before the other AI systems.
class AiTickSystem : public ECS::ISystem
{
public:
    static constexpr ECS::SystemCadence Cadence = ECS::SystemCadence::Simulation;

    /// The commands of a reload go to `rows`, ahead of the decisions of the tick.
    AiTickSystem(AiRegistry& registry, std::shared_ptr<std::vector<AiDecisionRow>> rows);
    AiTickSystem(const AiTickSystem&) = delete;
    AiTickSystem& operator=(const AiTickSystem&) = delete;

    void Update(ECS::World& world, float32 deltaTime) override;
    const char* GetName() const override { return "AiTickSystem"; }
    // World::Clear removes the clock and this system adds it again. A singleton write may rehash
    // the map that other systems read, so no other system runs beside this one.
    bool RequiresExclusiveUpdate() const override { return true; }

private:
    // Moves each agent whose path belongs to the reloaded program `id` to its new program.
    void RemapAgents(ECS::World& world, uint32 tick, uint16 id, const AiStateTreeProgram& oldProgram);

    AiRegistry& m_Registry;
    std::shared_ptr<std::vector<AiDecisionRow>> m_Rows;
    AiStateTreeDecider m_StateTrees;
    std::vector<AiDueAgent> m_Remapped;
};

} // namespace GameEngine::AIECS
