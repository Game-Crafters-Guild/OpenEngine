#pragma once

#include "AiDecisionRow.h"
#include "AiDueAgent.h"
#include "ECS/ECS.h"
#include "Events/AiEventQueue.h"
#include "Types/Types.h"

#include <span>
#include <utility>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::AIECS
{

class AiGroupStore;
class AiRegistry;
struct AiStateTreeProgram;

/// Steps the state trees of the due agents of one state-tree program, in jobs. In each job the program's
/// conditions and JobThreads inputs fill the job's columns, then each agent's tree steps once. Each agent
/// writes only its own AiAgent, AiStateTree, cells, AiPathTrace and row.
class AiStateTreeDecider
{
public:
    /// One program's due agents and their rows, in the same order.
    struct Batch
    {
        const AiStateTreeProgram* Program = nullptr;
        uint16 ProgramId = 0;
        uint32 Tick = 0;
        uint64 Seed = 0;
        std::span<const AiDueAgent> Agents;
        std::span<AiDecisionRow> Rows;
        /// The world's group knowledge, or null when the world has none.
        const AiGroupStore* Groups = nullptr;
    };

    explicit AiStateTreeDecider(const AiRegistry& registry);

    /// True when an agent of the program can decide: it has an AiStateTree, and its `cellCount` cells hold
    /// the program's cooldown cells. Else logs one error for each program and cause, with the fix.
    bool CanDecide(const AiStateTreeProgram& program, uint16 programId, size_t cellCount, bool hasTree);
    /// Sets the event deliveries for the batches of Decide. Call once per AI tick, before the first Decide;
    /// `deliveries` must stay unchanged until the last Decide of the tick.
    void PrepareEvents(std::span<const AiEventQueue::Delivery> deliveries);
    /// Steps each agent of the batch once, with the events addressed to it or to its AiStateTree::Group. First
    /// starts the tree of an agent whose AiAgent::PathProgram is not the batch's program; AiDecideSystem exits the
    /// path of that other program before.
    void Decide(const ECS::World& world, const Batch& batch);
    /// Exits the agent's path with the tasks of `programId`, the state-tree program the path belongs to, and
    /// empties the path. The tasks get the cells in front of that program's cooldown cells. Writes no cooldown.
    /// Exits no task when a state of the path is not a state of that program.
    void StopPath(const ECS::World& world, const AiDueAgent& due, uint16 programId, uint32 tick, AiDecisionRow& row,
                  const AiGroupStore* groups) const;
    /// Moves the agent's path from `oldProgram` to `newProgram`, the reloaded program under the same id
    /// (StateTreeInterpreter::Remap); `due.Program` is the id. The tasks of each program get the cells in front of
    /// that program's cooldown cells. Runs on the main thread: the new program's conditions and inputs are read for
    /// this agent.
    /// When the cells cannot hold the cooldown cells of both programs, exits the path and starts no tree.
    void RemapPath(const ECS::World& world, const AiDueAgent& due, const AiStateTreeProgram& oldProgram,
                   const AiStateTreeProgram& newProgram, uint32 tick, AiDecisionRow& row,
                   const AiGroupStore* groups) const;

private:
    // Exits the path with the tasks of `program`, whose id is `programId`, and empties it. Writes no cooldown.
    void StopWith(const ECS::World& world, const AiDueAgent& due, const AiStateTreeProgram& program, uint16 programId,
                  uint32 tick, AiDecisionRow& row, const AiGroupStore* groups) const;
    // Fills the batch's event lists: for each agent, its events and their indices in the program's events.
    void CollectEvents(const Batch& batch);
    // Runs on a job: fills the chunk's condition and input columns, and steps the chunk's agents.
    void DecideChunk(const ECS::World& world, const Batch& batch, size_t chunkBegin, size_t chunkEnd);

    const AiRegistry& m_Registry;
    std::vector<ECS::EntityHandle> m_Entities;
    // Condition c of agent r is m_Conditions[c * count + r]; input column c is m_Columns[c * count + r].
    std::vector<uint8> m_Conditions;
    std::vector<float32> m_Columns;
    std::vector<uint16> m_ReportedNoTree;
    // Program id and cell count of each error for cells that cannot hold the cooldown cells.
    std::vector<std::pair<uint16, size_t>> m_ReportedCells;
    std::span<const AiEventQueue::Delivery> m_Deliveries;
    // Agent r of the batch gets m_AgentEvents[m_EventOffsets[r] .. m_EventOffsets[r + 1]), and the program event
    // indices m_AgentEventIndices[m_IndexOffsets[r] .. m_IndexOffsets[r + 1]).
    std::vector<AiEvent> m_AgentEvents;
    std::vector<uint32> m_EventOffsets;
    std::vector<uint16> m_AgentEventIndices;
    std::vector<uint32> m_IndexOffsets;
};

} // namespace GameEngine::AIECS
