#include "StateTree/AiStateTreeDecider.h"

#include "AI/StateTree/StateTreeHost.h"
#include "AI/StateTree/StateTreeInterpreter.h"
#include "AIECS/AiEvent.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiStateTreeProgram.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiClock.h"
#include "AIECS/Components/AiPathTrace.h"
#include "AIECS/Components/AiStateTree.h"
#include "ECS/Entity.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "Logger/Logger.h"

#include <algorithm>

namespace GameEngine::AIECS
{

namespace
{

// The fewest agents one job steps; one step is short, so smaller batches cost more than they save.
constexpr size_t kAgentsPerJob = 64;

static_assert(Components::AiAgent::kMaxPathDepth == AI::StateTreeProgram::kMaxDepth);
static_assert(AiPathTraceRecord::kMaxDepth == AI::StateTreeProgram::kMaxDepth);
static_assert(Components::AiAgent::kNoState == AI::kStateTreeNone);

// What the host functions of one agent's step read: StateTreeHost::User points at it.
struct AgentBinding
{
    const AiRegistry& Registry;
    const AiStateTreeProgram& Program;
    AiTaskContext& Context;
    // Condition c of this agent is Conditions[c * Stride]; input column c is Columns[c * Stride].
    const uint8* Conditions = nullptr;
    const float32* Columns = nullptr;
    size_t Stride = 0;
    // During a reload: the old program, whose tasks run with OldContext.
    const AiStateTreeProgram* OldProgram = nullptr;
    AiTaskContext* OldContext = nullptr;
};

// A task of the bound programs and the context it runs with.
struct BoundTask
{
    const AiTaskDefinition& Definition;
    AiTaskContext& Context;
};

BoundTask TaskOf(const AgentBinding& binding, const AI::StateTreeProgram& program, uint32 task)
{
    const bool old = binding.OldProgram != nullptr && &program == &binding.OldProgram->Program;
    const AiStateTreeProgram& owner = old ? *binding.OldProgram : binding.Program;
    return {binding.Registry.GetTask(owner.Tasks[program.GetTasks()[task].Kind]),
            old ? *binding.OldContext : binding.Context};
}

void EnterTask(void* user, const AI::StateTreeProgram& program, uint32 task)
{
    const BoundTask bound = TaskOf(*static_cast<AgentBinding*>(user), program, task);
    if (bound.Definition.Enter != nullptr)
        bound.Definition.Enter(bound.Context);
}

AI::StateTreeTaskStatus TickTask(void* user, const AI::StateTreeProgram& program, uint32 task)
{
    const BoundTask bound = TaskOf(*static_cast<AgentBinding*>(user), program, task);
    switch (bound.Definition.Tick(bound.Context))
    {
    case AiTaskStatus::Running:
        return AI::StateTreeTaskStatus::Running;
    case AiTaskStatus::Succeeded:
        return AI::StateTreeTaskStatus::Succeeded;
    case AiTaskStatus::Failed:
        return AI::StateTreeTaskStatus::Failed;
    }
    return AI::StateTreeTaskStatus::Failed;
}

AiTaskExitReason ToExitReason(AI::StateTreeExitReason reason)
{
    switch (reason)
    {
    case AI::StateTreeExitReason::Completed:
        return AiTaskExitReason::Completed;
    case AI::StateTreeExitReason::Failed:
        return AiTaskExitReason::Failed;
    case AI::StateTreeExitReason::Preempted:
        return AiTaskExitReason::Preempted;
    case AI::StateTreeExitReason::Reloaded:
        return AiTaskExitReason::Reloaded;
    }
    return AiTaskExitReason::Preempted;
}

void ExitTask(void* user, const AI::StateTreeProgram& program, uint32 task, AI::StateTreeExitReason reason)
{
    const BoundTask bound = TaskOf(*static_cast<AgentBinding*>(user), program, task);
    if (bound.Definition.Exit != nullptr)
        bound.Definition.Exit(bound.Context, ToExitReason(reason));
}

bool TestCondition(void* user, uint16 condition)
{
    const AgentBinding& binding = *static_cast<const AgentBinding*>(user);
    return binding.Conditions[condition * binding.Stride] != 0;
}

void FillColumns(void* user, uint16, std::span<float32> columns)
{
    const AgentBinding& binding = *static_cast<const AgentBinding*>(user);
    for (size_t column = 0; column < columns.size(); ++column)
        columns[column] = binding.Columns[column * binding.Stride];
}

// Copies the tree's path into the agent and the trace.
void MirrorPath(const AI::StateTreeState& state, uint32 tick, Components::AiAgent& agent)
{
    agent.ActivePath.fill(Components::AiAgent::kNoState);
    for (uint8 depth = 0; depth < state.Depth; ++depth)
        agent.ActivePath[depth] = state.Path[depth].State;
    agent.PathDepth = state.Depth;
    agent.EnteredTick = state.Depth > 0 ? state.Path[state.Depth - 1].EnteredTick : tick;
    agent.ActiveScore = 0.0f;
}

void WritePathTrace(Components::AiPathTrace& trace, const Components::AiAgent& agent, uint16 program, uint32 tick,
                    AI::StateTreeTaskStatus result)
{
    AiPathTraceRecord record;
    record.Tick = tick;
    record.Program = program;
    record.Depth = agent.PathDepth;
    record.Result = result;
    record.States = agent.ActivePath;
    trace.Records[trace.Count % Components::AiPathTrace::kCapacity] = record;
    ++trace.Count;
}

// The index of `type` in the program's events, or AI::kStateTreeNone when the program names no such event.
uint16 FindProgramEvent(const AiStateTreeProgram& program, uint16 type)
{
    const auto found = std::find(program.Events.begin(), program.Events.end(), type);
    return found != program.Events.end() ? static_cast<uint16>(found - program.Events.begin()) : AI::kStateTreeNone;
}

bool TargetsFirst(const std::pair<ECS::EntityHandle, uint32>& left, const std::pair<ECS::EntityHandle, uint32>& right)
{
    return left.first != right.first ? left.first < right.first : left.second < right.second;
}

// The delivery indices of the events addressed to `target`, from the list sorted by target.
std::span<const std::pair<ECS::EntityHandle, uint32>> EventsOf(
    std::span<const std::pair<ECS::EntityHandle, uint32>> byTarget, ECS::EntityHandle target)
{
    const auto first = std::lower_bound(byTarget.begin(), byTarget.end(), std::pair(target, 0u), &TargetsFirst);
    auto last = first;
    while (last != byTarget.end() && last->first == target)
        ++last;
    return {first, last};
}

// Logs that the first `depth` frames of `path` hold a state that the program does not have, so the interpreter
// exited no task of the path.
void ReportForeignPath(ECS::EntityHandle agent, uint16 programId, const AI::StateTreeState& path, uint8 depth,
                       size_t stateCount)
{
    uint16 foreign = AI::kStateTreeNone;
    for (uint8 frame = 0; frame < depth && foreign == AI::kStateTreeNone; ++frame)
    {
        if (path.Path[frame].State >= stateCount)
            foreign = path.Path[frame].State;
    }
    Logger::Log::Error("AiDecideSystem: the path of agent {} holds state {}, which state-tree program {:#x} does not "
                       "have ({} states), so no task of the path exits. Only the engine writes AiStateTree::State.",
                       agent.id, foreign, programId, stateCount);
}

} // namespace

AiStateTreeDecider::AiStateTreeDecider(const AiRegistry& registry) : m_Registry(registry)
{
}

bool AiStateTreeDecider::CanDecide(const AiStateTreeProgram& program, uint16 programId, size_t cellCount, bool hasTree)
{
    if (!hasTree)
    {
        if (std::find(m_ReportedNoTree.begin(), m_ReportedNoTree.end(), programId) == m_ReportedNoTree.end())
        {
            m_ReportedNoTree.push_back(programId);
            Logger::Log::Error("AiDecideSystem: an agent of state-tree program {} has no AiStateTree component, so it "
                               "does not decide. Add AiStateTree to each agent whose AiAgent::Program is a state-tree "
                               "program.",
                               programId);
        }
        return false;
    }
    const size_t slots = program.Program.GetCooldownSlotCount();
    if (slots <= cellCount)
        return true;
    const std::pair<uint16, size_t> key{programId, cellCount};
    if (std::find(m_ReportedCells.begin(), m_ReportedCells.end(), key) == m_ReportedCells.end())
    {
        m_ReportedCells.push_back(key);
        Logger::Log::Error("AiDecideSystem: state-tree program {} keeps {} cooldowns in the last cells of its agents, "
                           "but an agent has {} cells, so its agents with {} cells do not decide. Give them a larger "
                           "cell class (AiCellsSmall, AiCellsMedium or AiCellsLarge).",
                           programId, slots, cellCount, cellCount);
    }
    return false;
}

void AiStateTreeDecider::Decide(const ECS::World& world, const Batch& batch)
{
    const AiStateTreeProgram& program = *batch.Program;
    const size_t count = batch.Agents.size();
    m_Entities.resize(count);
    for (size_t index = 0; index < count; ++index)
        m_Entities[index] = batch.Agents[index].Entity;
    m_Conditions.resize(program.Conditions.size() * count);
    m_Columns.resize(program.Inputs.size() * count);
    for (size_t column = 0; column < program.Inputs.size(); ++column)
    {
        const uint16 input = program.Inputs[column];
        if (m_Registry.GetInputThreading(input) == AiInputThreading::CallingThread)
            m_Registry.GetInput(input)(world, m_Entities, std::span(m_Columns).subspan(column * count, count));
    }
    CollectEvents(batch);
    JobSystem::ParallelFor(
        world.GetJobSystem(), count, [this, &world, &batch](size_t chunkBegin, size_t chunkEnd)
        { DecideChunk(world, batch, chunkBegin, chunkEnd); }, kAgentsPerJob);
}

void AiStateTreeDecider::DecideChunk(const ECS::World& world, const Batch& batch, size_t chunkBegin, size_t chunkEnd)
{
    const AiStateTreeProgram& program = *batch.Program;
    const size_t count = batch.Agents.size();
    const size_t rowCount = chunkEnd - chunkBegin;
    const std::span<const ECS::EntityHandle> entities(m_Entities.data() + chunkBegin, rowCount);
    for (size_t condition = 0; condition < program.Conditions.size(); ++condition)
    {
        const std::span<uint8> out(m_Conditions.data() + condition * count + chunkBegin, rowCount);
        m_Registry.GetCondition(program.Conditions[condition])(world, entities, out);
    }
    for (size_t column = 0; column < program.Inputs.size(); ++column)
    {
        const uint16 input = program.Inputs[column];
        if (m_Registry.GetInputThreading(input) == AiInputThreading::JobThreads)
            m_Registry.GetInput(input)(world, entities, std::span(m_Columns).subspan(column * count + chunkBegin, rowCount));
    }

    const AI::StateTreeHost hostFunctions{nullptr, &EnterTask, &TickTask, &ExitTask, &TestCondition, &FillColumns};
    const size_t slots = program.Program.GetCooldownSlotCount();
    AI::StateTreeInterpreter interpreter;
    for (size_t index = chunkBegin; index < chunkEnd; ++index)
    {
        const AiDueAgent& due = batch.Agents[index];
        Components::AiAgent& agent = *due.Agent;
        Components::AiStateTree& tree = *due.Tree;
        AiDecisionRow& row = batch.Rows[index];
        row.Agent = due.Entity;
        // Tasks own the front cells; the program's cooldowns take the last ones.
        const std::span<uint32> taskCells = due.Cells.first(due.Cells.size() - slots);
        AiTaskContext context(world, due.Entity, batch.Tick, taskCells, row, m_Registry.GetCommandCount(), batch.Groups);
        context.m_Events = std::span(m_AgentEvents)
                               .subspan(m_EventOffsets[index], m_EventOffsets[index + 1] - m_EventOffsets[index]);
        const std::span<const uint16> events = std::span(m_AgentEventIndices)
                                                   .subspan(m_IndexOffsets[index],
                                                            m_IndexOffsets[index + 1] - m_IndexOffsets[index]);
        AgentBinding binding{m_Registry, program, context, m_Conditions.data() + index, m_Columns.data() + index, count};
        AI::StateTreeHost host = hostFunctions;
        host.User = &binding;

        interpreter.Bind(tree.State, due.Cells.last(slots), agent.DrawSeed, batch.Seed);
        if (agent.PathProgram != batch.ProgramId)
        {
            interpreter.Start(program.Program, host, batch.Tick);
            agent.PathProgram = batch.ProgramId;
        }
        interpreter.Step(program.Program, host, events, batch.Tick);

        MirrorPath(tree.State, batch.Tick, agent);
        agent.NextTick = batch.Tick + std::max<uint32>(1, agent.Cadence);
        tree.LastStepTick = batch.Tick;
        if (due.PathTrace != nullptr)
            WritePathTrace(*due.PathTrace, agent, batch.ProgramId, batch.Tick, tree.State.Result);
    }
}

void AiStateTreeDecider::StopPath(const ECS::World& world, const AiDueAgent& due, uint16 programId, uint32 tick,
                                  AiDecisionRow& row, const AiGroupStore* groups) const
{
    if (due.Tree == nullptr)
        return;
    if (const AiStateTreeProgram* program = m_Registry.FindStateTree(programId))
        StopWith(world, due, *program, programId, tick, row, groups);
    due.Tree->State.Depth = 0;
}

void AiStateTreeDecider::StopWith(const ECS::World& world, const AiDueAgent& due, const AiStateTreeProgram& program,
                                  uint16 programId, uint32 tick, AiDecisionRow& row, const AiGroupStore* groups) const
{
    const size_t slots = program.Program.GetCooldownSlotCount();
    if (slots <= due.Cells.size())
    {
        AiTaskContext context(world, due.Entity, tick, due.Cells.first(due.Cells.size() - slots), row,
                              m_Registry.GetCommandCount(), groups);
        AgentBinding binding{m_Registry, program, context};
        const AI::StateTreeHost host{&binding, &EnterTask, &TickTask, &ExitTask, &TestCondition, &FillColumns};
        // Stop draws no random value, so the draw keys are 0.
        AI::StateTreeInterpreter interpreter;
        interpreter.Bind(due.Tree->State, due.Cells.last(slots), 0, 0);
        const uint8 depth = due.Tree->State.Depth;
        if (!interpreter.Stop(program.Program, host))
            ReportForeignPath(due.Entity, programId, due.Tree->State, depth, program.Program.GetStates().size());
    }
    due.Tree->State.Depth = 0;
}

void AiStateTreeDecider::RemapPath(const ECS::World& world, const AiDueAgent& due, const AiStateTreeProgram& oldProgram,
                                   const AiStateTreeProgram& newProgram, uint32 tick, AiDecisionRow& row,
                                   const AiGroupStore* groups) const
{
    Components::AiAgent& agent = *due.Agent;
    const size_t oldSlots = oldProgram.Program.GetCooldownSlotCount();
    const size_t newSlots = newProgram.Program.GetCooldownSlotCount();
    if (std::max(oldSlots, newSlots) > due.Cells.size())
    {
        StopWith(world, due, oldProgram, due.Program, tick, row, groups);
        MirrorPath(due.Tree->State, tick, agent);
        agent.PathProgram = Components::AiAgent::kNoProgram;
        return;
    }
    // The reload is rare and runs on the main thread, so the conditions and inputs are read for one agent.
    const std::span<const ECS::EntityHandle> entity(&due.Entity, 1);
    std::vector<uint8> conditions(newProgram.Conditions.size());
    for (size_t condition = 0; condition < conditions.size(); ++condition)
    {
        if (const AiConditionFunction read = m_Registry.GetCondition(newProgram.Conditions[condition]))
            read(world, entity, std::span(conditions).subspan(condition, 1));
    }
    std::vector<float32> columns(newProgram.Inputs.size());
    for (size_t column = 0; column < columns.size(); ++column)
    {
        if (const AiInputFunction read = m_Registry.GetInput(newProgram.Inputs[column]))
            read(world, entity, std::span(columns).subspan(column, 1));
    }

    AiTaskContext oldContext(world, due.Entity, tick, due.Cells.first(due.Cells.size() - oldSlots), row,
                             m_Registry.GetCommandCount(), groups);
    AiTaskContext newContext(world, due.Entity, tick, due.Cells.first(due.Cells.size() - newSlots), row,
                             m_Registry.GetCommandCount(), groups);
    AgentBinding binding{m_Registry, newProgram, newContext, conditions.data(), columns.data(), 1};
    binding.OldProgram = &oldProgram;
    binding.OldContext = &oldContext;
    const AI::StateTreeHost host{&binding, &EnterTask, &TickTask, &ExitTask, &TestCondition, &FillColumns};
    // The exits write the old program's cooldown cells and Remap resets them, so both programs' cells are bound.
    const Components::AiClock* clock = world.GetSingleton<Components::AiClock>();
    AI::StateTreeInterpreter interpreter;
    interpreter.Bind(due.Tree->State, due.Cells.last(std::max(oldSlots, newSlots)), agent.DrawSeed,
                     clock != nullptr ? clock->Seed : 0);
    const uint8 depth = due.Tree->State.Depth;
    if (!interpreter.Remap(oldProgram.Program, newProgram.Program, host, tick))
        ReportForeignPath(due.Entity, due.Program, due.Tree->State, depth, oldProgram.Program.GetStates().size());
    MirrorPath(due.Tree->State, tick, agent);
}

void AiStateTreeDecider::PrepareEvents(std::span<const AiEventQueue::Delivery> deliveries)
{
    m_Deliveries = deliveries;
}

void AiStateTreeDecider::CollectEvents(const Batch& batch)
{
    const AiStateTreeProgram& program = *batch.Program;
    m_AgentEvents.clear();
    m_AgentEventIndices.clear();
    m_EventOffsets.assign(1, 0);
    m_IndexOffsets.assign(1, 0);
    for (const AiDueAgent& due : batch.Agents)
    {
        // The events posted at or after the agent's last step; an agent that has not stepped gets those of the
        // last cadence ticks.
        const uint32 cadence = std::max<uint32>(1, due.Agent->Cadence);
        const uint32 fromTick = due.Tree->LastStepTick != Components::AiStateTree::kNotStepped
                                    ? due.Tree->LastStepTick
                                    : batch.Tick - std::min(batch.Tick, cadence);
        for (const AiEventQueue::Delivery& delivery : m_Deliveries)
        {
            if (delivery.Events.back().Tick < fromTick)
                continue;
            const auto own = EventsOf(delivery.ByTarget, due.Entity);
            const auto group = due.Tree->Group.IsValid() ? EventsOf(delivery.ByTarget, due.Tree->Group)
                                                         : std::span<const std::pair<ECS::EntityHandle, uint32>>();
            // The agent's events and its group's events, merged in delivery order.
            auto left = own.begin();
            auto right = group.begin();
            while (left != own.end() || right != group.end())
            {
                const bool takeLeft = right == group.end() || (left != own.end() && left->second < right->second);
                const AiEvent& event = delivery.Events[(takeLeft ? left++ : right++)->second];
                if (event.Tick < fromTick)
                    continue;
                m_AgentEvents.push_back(event);
                const uint16 index = FindProgramEvent(program, event.Type);
                if (index != AI::kStateTreeNone)
                    m_AgentEventIndices.push_back(index);
            }
        }
        m_EventOffsets.push_back(static_cast<uint32>(m_AgentEvents.size()));
        m_IndexOffsets.push_back(static_cast<uint32>(m_AgentEventIndices.size()));
    }
}

} // namespace GameEngine::AIECS
