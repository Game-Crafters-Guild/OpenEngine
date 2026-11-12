#pragma once

#include "AIECS/AiCommand.h"
#include "AIECS/AiEvent.h"
#include "ECS/ECS.h"
#include "Types/Types.h"

#include <span>

namespace GameEngine::AI
{
class RecordStore;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::AIECS
{

class AiGroupStore;
struct AiDecisionRow;

/// What a task sees when it runs: the world to read, its agent, the AI tick, and the place for
/// its orders. AiDecideSystem runs the tasks of many agents at once on job threads, so a task
/// only reads the world and changes nothing but its own agent; it acts through IssueCommand.
class AiTaskContext
{
public:
    /// The world, read only: a task changes the world through IssueCommand.
    const ECS::World& GetWorld() const { return m_World; }
    /// The agent that runs the task.
    ECS::EntityHandle GetAgent() const { return m_Agent; }
    /// The current AI tick.
    uint32 GetTick() const { return m_Tick; }
    /// The agent's cells (AiCellsSmall, AiCellsMedium or AiCellsLarge); empty when it has none.
    /// A task keeps here what it must remember between ticks, for example the order it issued.
    std::span<uint32> GetCells() const { return m_Cells; }
    /// The memory records of the group entity, or null when the entity has no AiGroup or the group
    /// had no AI tick yet. Takes no lock: the knowledge does not change while the tasks run.
    const AI::RecordStore* FindGroupRecords(ECS::EntityHandle group) const;

    /// Queues an order for AiApplySystem.
    /// @return false when this decision already holds 4 orders, or when AiCommand::Type is not a
    ///         registered command (an error names the type); the order is dropped.
    bool IssueCommand(const AiCommand& command);
    /// Posts an event from the agent: AiEvent::Source is the agent and AiEvent::Tick the current AI tick.
    /// AiApplySystem posts it in entity order; each addressed agent gets it at its next step.
    /// @return false when this decision already posted 4 events; the event is dropped.
    bool PostEvent(AiEvent event);
    /// The events posted since the agent's last step, in delivery order. Empty for a utility program.
    std::span<const AiEvent> GetEvents() const { return m_Events; }

private:
    friend class AiDecideSystem;
    friend class AiStateTreeDecider;

    // `commandCount` is the number of commands in the registry; `groups` is the world's group
    // knowledge, or null when the world has none.
    AiTaskContext(const ECS::World& world, ECS::EntityHandle agent, uint32 tick, std::span<uint32> cells,
                  AiDecisionRow& row, size_t commandCount, const AiGroupStore* groups);

    const ECS::World& m_World;
    const AiGroupStore* m_Groups = nullptr;
    AiDecisionRow& m_Row;
    std::span<uint32> m_Cells;
    std::span<const AiEvent> m_Events;
    ECS::EntityHandle m_Agent;
    size_t m_CommandCount = 0;
    uint32 m_Tick = 0;
};

} // namespace GameEngine::AIECS
