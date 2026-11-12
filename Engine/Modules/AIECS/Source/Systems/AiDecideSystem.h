#pragma once

#include "AI/OptionSet.h"
#include "AIECS/AiTraceRecord.h"
#include "AiDecisionRow.h"
#include "AiDueAgent.h"
#include "ECS/Systems.h"
#include "StateTree/AiStateTreeDecider.h"

#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace GameEngine::Components
{
struct AiAgent;
struct AiPathTrace;
struct AiStateTree;
struct AiTrace;
}

namespace GameEngine::AIECS
{

class AiGroupStore;
struct AiProgram;
class AiRegistry;
class AiTaskContext;

/// Decides for the agents due this AI tick, program by program, in jobs: in each job the JobThreads
/// inputs fill the job's columns, the option set chooses and the tasks run. CallingThread inputs fill
/// their columns before the jobs start. A state-tree program's agents step their trees instead
/// (AiStateTreeDecider). Each agent writes only its own AiAgent, AiStateTree, cells, traces and row.
class AiDecideSystem : public ECS::ISystem
{
public:
    static constexpr ECS::SystemCadence Cadence = ECS::SystemCadence::Simulation;

    AiDecideSystem(const AiRegistry& registry, std::shared_ptr<std::vector<AiDecisionRow>> rows);

    void Update(ECS::World& world, float32 deltaTime) override;
    const char* GetName() const override { return "AiDecideSystem"; }
    // Job-thread inputs and tasks read the world, so no other system runs beside this one. The
    // update still splits its agents over every worker.
    bool RequiresExclusiveUpdate() const override { return true; }

private:
    void CollectDueAgents(ECS::World& world, uint32 tick);
    // Schedules the agent's first decision and adds it to m_Due when it is due. Skips kNoProgram, an
    // agent that CanDecide refuses, and, for a state-tree program, an agent that AiStateTreeDecider refuses.
    void CollectAgent(uint32 tick, const AiDueAgent& candidate);
    // Exits the path of the due agent m_Due[index] with the tasks of AiAgent::PathProgram, with Preempted, empties
    // the path and clears the restart request. Writes no cooldown. The deciders then start the agent's program.
    void ExitPath(const ECS::World& world, uint32 tick, size_t index);
    // False, with an error, for a program id that is not registered, a program that uses a removed
    // registration, and a utility program whose cooldown cells do not fit in `cellCount` cells.
    bool CanDecide(uint16 programId, size_t cellCount);
    // Sets EnteredTick to at most `tick` and NextTick to at most `tick + cadence`. Returns true when
    // EnteredTick was past `tick`: the agent's ticks come from a later clock, for example a save.
    static bool KeepTicksAtOrBefore(uint32 tick, uint32 cadence, Components::AiAgent& agent);
    // Logs one warning for the first agent whose saved ticks are past the AI tick.
    void ReportTicksPastNow(uint32 tick);
    // Logs one error for each program id that is not registered.
    void ReportUnregisteredProgram(uint16 program);
    // Logs one error for each program that uses a task, a condition or an input that its module removed.
    void ReportRemovedRegistration(uint16 program);
    // Logs one error for each program and cell count that cannot hold the program's cooldown cells.
    void ReportCellsTooSmall(const AiProgram& program, uint16 programId, size_t cellCount);
    // One program's due agents: m_Due[Begin, Begin + Count).
    struct ProgramBatch
    {
        const AiProgram* Program = nullptr;
        uint32 Tick = 0;
        uint64 Seed = 0;
        size_t Begin = 0;
        size_t Count = 0;
    };

    void DecideProgram(const ECS::World& world, uint32 tick, uint64 seed, size_t begin, size_t end);
    // Fills m_CallingColumns for the program's CallingThread inputs, for all agents in m_Entities.
    void FillCallingThreadColumns(const ECS::World& world, const AiProgram& program);
    // Runs on a job: fills the chunk's columns, chooses, and runs the chunk's agents.
    void DecideChunk(const ECS::World& world, const ProgramBatch& batch, size_t chunkBegin, size_t chunkEnd);
    // `index` is the agent's place in m_Due and in the rows; `local` its place in the program's states,
    // choices and scores.
    void RunAgent(const ECS::World& world, const AiProgram& program, uint32 tick, size_t index, size_t local);
    // Ticks the chosen option's task; a task that stops leaves the agent with no active option.
    AiTraceOutcome TickChosen(AiTaskContext& context, const AiProgram& program, size_t index, uint16 chosen,
                              uint32 tick);

    const AiRegistry& m_Registry;
    AiStateTreeDecider m_StateTrees;
    std::shared_ptr<std::vector<AiDecisionRow>> m_Rows;
    // The rows of this tick's due agents, in the order of m_Due.
    std::span<AiDecisionRow> m_DueRows;
    // The world's group knowledge for this update, found once; the tasks read it with no lock.
    const AiGroupStore* m_Groups = nullptr;
    std::vector<AiDueAgent> m_Due;
    // The largest cadence of the agents of this AI tick: the AI ticks that a delivered event stays.
    uint32 m_MaxCadence = 1;
    std::vector<ECS::EntityHandle> m_Entities;
    // Chunk by chunk: the columns of chunk [b, e) fill m_Columns[columns * b, columns * e).
    std::vector<float32> m_Columns;
    // Column by column, for all agents of the program: m_CallingColumns[c * count + r].
    std::vector<float32> m_CallingColumns;
    std::vector<AI::OptionSet::Row> m_States;
    std::vector<uint16> m_Chosen;
    std::vector<float32> m_Scores;
    std::vector<uint16> m_ReportedPrograms;
    std::vector<uint16> m_ReportedRemovedPrograms;
    // Program id and cell count of each ReportCellsTooSmall error.
    std::vector<std::pair<uint16, size_t>> m_ReportedCells;
    bool m_ReportedTicksPastNow = false;
};

} // namespace GameEngine::AIECS
