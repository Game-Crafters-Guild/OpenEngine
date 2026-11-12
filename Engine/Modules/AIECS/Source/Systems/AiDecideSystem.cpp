#include "Systems/AiDecideSystem.h"

#include "AIECS/AiProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsLarge.h"
#include "AIECS/Components/AiCells/AiCellsMedium.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "AIECS/Components/AiClock.h"
#include "AIECS/Components/AiPathTrace.h"
#include "AIECS/Components/AiStateTree.h"
#include "AIECS/Components/AiTrace.h"
#include "AiGroup/AiGroupStore.h"
#include "Events/AiEventQueue.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "Logger/Logger.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cassert>
#include <span>
#include <utility>

namespace GameEngine::AIECS
{

namespace
{

// The fewest agents one job decides for; one decision is short, so smaller batches cost more than they save.
constexpr size_t kAgentsPerJob = 64;

// An empty path level and "no option chosen" are one value, so the two compare directly.
static_assert(Components::AiAgent::kNoState == AI::OptionSet::kNoOption);

// True when the program has no cooldown or `cellCount` cells hold one cooldown cell per option.
bool CellsHoldCooldowns(const AiProgram& program, size_t cellCount)
{
    return program.CooldownCell == AiProgram::kNoCell ||
           program.CooldownCell + program.Options.Options.size() <= cellCount;
}

// The cells that hold each option's ready tick; empty when the program has no cooldown.
std::span<uint32> ReadyTicks(const AiProgram& program, std::span<uint32> cells)
{
    if (program.CooldownCell == AiProgram::kNoCell)
        return {};
    const size_t optionCount = program.Options.Options.size();
    assert(CellsHoldCooldowns(program, cells.size()) && "AiDecideSystem: CollectAgent admits only cells that hold the cooldowns");
    return cells.subspan(program.CooldownCell, optionCount);
}

// Starts the option's cooldown when it stops.
void StartCooldown(const AiProgram& program, std::span<uint32> cells, uint16 option, uint32 tick)
{
    const std::span<uint32> readyTicks = ReadyTicks(program, cells);
    if (!readyTicks.empty())
        readyTicks[option] = tick + program.Options.Options[option].CooldownTicks;
}

// Sets each option's ready tick to at most `tick` plus the option's cooldown.
void KeepReadyTicksAtOrBefore(const AiProgram& program, std::span<uint32> cells, uint32 tick)
{
    const std::span<uint32> readyTicks = ReadyTicks(program, cells);
    for (size_t option = 0; option < readyTicks.size(); ++option)
        readyTicks[option] = std::min(readyTicks[option], tick + program.Options.Options[option].CooldownTicks);
}

AiTraceOutcome ToTraceOutcome(AiTaskStatus status)
{
    switch (status)
    {
    case AiTaskStatus::Running:
        return AiTraceOutcome::Running;
    case AiTaskStatus::Succeeded:
        return AiTraceOutcome::Succeeded;
    case AiTaskStatus::Failed:
        return AiTraceOutcome::Failed;
    }
    return AiTraceOutcome::None;
}

// Appends the decision to the ring; `record` holds everything but the orders, which come from `row`.
void WriteTrace(Components::AiTrace& trace, AiTraceRecord record, const AiDecisionRow& row)
{
    record.CommandCount = static_cast<uint8>(row.CommandCount);
    const uint32 kept = std::min(row.CommandCount, AiTraceRecord::kMaxCommands);
    for (uint32 index = 0; index < kept; ++index)
    {
        record.CommandTargets[index] = row.Commands[index].Target;
        record.CommandArguments[index] = row.Commands[index].Argument;
        record.CommandTypes[index] = row.Commands[index].Type;
    }
    trace.Records[trace.Count % Components::AiTrace::kCapacity] = record;
    ++trace.Count;
}

} // namespace

std::span<uint32> SelectCells(Components::AiCellsSmall* small, Components::AiCellsMedium* medium,
                              Components::AiCellsLarge* large)
{
    if (small != nullptr)
        return small->Values;
    if (medium != nullptr)
        return medium->Values;
    if (large != nullptr)
        return large->Values;
    return {};
}

AiDecideSystem::AiDecideSystem(const AiRegistry& registry, std::shared_ptr<std::vector<AiDecisionRow>> rows)
    : m_Registry(registry), m_StateTrees(registry), m_Rows(std::move(rows))
{
}

void AiDecideSystem::Update(ECS::World& world, float32)
{
    const Components::AiClock* clock = world.GetSingleton<Components::AiClock>();
    if (clock == nullptr || !clock->Due)
        return;

    // This system runs alone, so the store neither changes nor goes away while the tasks read it.
    m_Groups = AiGroupStore::FindForWorld(world.GetWorldId());
    CollectDueAgents(world, clock->Tick);
    // The rows of a reload that AiTickSystem wrote come first.
    const size_t rowBase = m_Rows->size();
    m_Rows->resize(rowBase + m_Due.size());
    m_DueRows = std::span(*m_Rows).subspan(rowBase);
    AiEventQueue* events = AiEventQueue::FindForWorld(world.GetWorldId());
    if (events != nullptr)
        events->SetKeepTicks(m_MaxCadence);
    m_StateTrees.PrepareEvents(events != nullptr ? events->GetDeliveries() : std::span<const AiEventQueue::Delivery>());
    for (size_t index = 0; index < m_Due.size(); ++index)
    {
        const Components::AiAgent& agent = *m_Due[index].Agent;
        if (agent.PathProgram != m_Due[index].Program || agent.RestartRequested)
            ExitPath(world, clock->Tick, index);
    }
    for (size_t begin = 0; begin < m_Due.size();)
    {
        size_t end = begin + 1;
        while (end < m_Due.size() && m_Due[end].Program == m_Due[begin].Program)
            ++end;
        if (const AiStateTreeProgram* stateTree = m_Registry.FindStateTree(m_Due[begin].Program))
        {
            const std::span<const AiDueAgent> agents = std::span(m_Due).subspan(begin, end - begin);
            const std::span<AiDecisionRow> rows = m_DueRows.subspan(begin, end - begin);
            m_StateTrees.Decide(world, {stateTree, m_Due[begin].Program, clock->Tick, clock->Seed, agents, rows, m_Groups});
        }
        else
        {
            DecideProgram(world, clock->Tick, clock->Seed, begin, end);
        }
        begin = end;
    }
}

void AiDecideSystem::CollectDueAgents(ECS::World& world, uint32 tick)
{
    m_Due.clear();
    m_MaxCadence = 1;
    world
        .Query<ECS::Write<Components::AiAgent>, ECS::Optional<Components::AiCellsSmall>,
               ECS::Optional<Components::AiCellsMedium>, ECS::Optional<Components::AiCellsLarge>,
               ECS::Optional<Components::AiTrace>, ECS::Optional<Components::AiStateTree>,
               ECS::Optional<Components::AiPathTrace>>()
        .Each(
            [this, tick](ECS::EntityHandle entity, Components::AiAgent& agent, Components::AiCellsSmall* small,
                         Components::AiCellsMedium* medium, Components::AiCellsLarge* large,
                         Components::AiTrace* trace, Components::AiStateTree* tree, Components::AiPathTrace* pathTrace)
            {
                CollectAgent(tick, {&agent, tree, SelectCells(small, medium, large), trace, pathTrace, entity,
                                    agent.Program});
            });
    // Program first, so a program's agents are one batch of input calls; entity next, so the order
    // does not depend on the archetype layout.
    std::sort(m_Due.begin(), m_Due.end(),
              [](const AiDueAgent& left, const AiDueAgent& right)
              { return left.Program != right.Program ? left.Program < right.Program : left.Entity < right.Entity; });
}

void AiDecideSystem::CollectAgent(uint32 tick, const AiDueAgent& candidate)
{
    Components::AiAgent& agent = *candidate.Agent;
    const ECS::EntityHandle entity = candidate.Entity;
    if (agent.Program == Components::AiAgent::kNoProgram || !CanDecide(agent.Program, candidate.Cells.size()))
        return;
    const AiStateTreeProgram* stateTree = m_Registry.FindStateTree(agent.Program);
    if (stateTree != nullptr &&
        !m_StateTrees.CanDecide(*stateTree, agent.Program, candidate.Cells.size(), candidate.Tree != nullptr))
        return;
    const uint32 cadence = std::max<uint32>(1, agent.Cadence);
    m_MaxCadence = std::max(m_MaxCadence, cadence);
    if (!agent.Scheduled)
    {
        agent.NextTick = tick + entity.index % cadence;
        agent.Scheduled = true;
    }
    // Odd, so that 0 stays the value of an agent with no seed. Wake does not touch the seed.
    if (agent.DrawSeed == 0)
        agent.DrawSeed = static_cast<uint32>(Hashing::SplitMix64(Hashing::HashCombine(entity.id, tick))) | 1u;
    if (KeepTicksAtOrBefore(tick, cadence, agent))
    {
        // The cells hold the cooldowns of the program that the path belongs to until ExitPath runs.
        const uint16 cellsProgram = agent.PathProgram != Components::AiAgent::kNoProgram ? agent.PathProgram : agent.Program;
        const AiProgram* program = m_Registry.FindProgram(cellsProgram);
        if (program != nullptr && CellsHoldCooldowns(*program, candidate.Cells.size()))
            KeepReadyTicksAtOrBefore(*program, candidate.Cells, tick);
        ReportTicksPastNow(tick);
    }
    if (tick >= agent.NextTick)
        m_Due.push_back(candidate);
}

void AiDecideSystem::ExitPath(const ECS::World& world, uint32 tick, size_t index)
{
    const AiDueAgent& due = m_Due[index];
    Components::AiAgent& agent = *due.Agent;
    AiDecisionRow& row = m_DueRows[index];
    row.Agent = due.Entity;
    // The path belongs to PathProgram, never to Program: a restart and a program change in one decision exit
    // the tasks of the old program.
    const uint16 pathProgram = agent.PathProgram;
    agent.RestartRequested = false;
    if (const AiProgram* program = m_Registry.FindProgram(pathProgram))
    {
        const uint16 active = agent.PathDepth > 0 ? agent.ActivePath[0] : Components::AiAgent::kNoState;
        if (active < program->Options.Options.size())
        {
            const AiTaskDefinition& task = m_Registry.GetTask(program->Options.Options[active].Action);
            AiTaskContext context(world, due.Entity, tick, due.Cells, row, m_Registry.GetCommandCount(), m_Groups);
            if (task.Exit != nullptr)
                task.Exit(context, AiTaskExitReason::Preempted);
        }
    }
    else
    {
        m_StateTrees.StopPath(world, due, pathProgram, tick, row, m_Groups);
    }
    agent.ActivePath.fill(Components::AiAgent::kNoState);
    agent.PathDepth = 0;
    agent.PathProgram = Components::AiAgent::kNoProgram;
}

bool AiDecideSystem::CanDecide(uint16 programId, size_t cellCount)
{
    const AiProgram* program = m_Registry.FindProgram(programId);
    if (program == nullptr && m_Registry.FindStateTree(programId) == nullptr)
    {
        ReportUnregisteredProgram(programId);
        return false;
    }
    if (!m_Registry.IsProgramRunnable(programId))
    {
        ReportRemovedRegistration(programId);
        return false;
    }
    if (program != nullptr && !CellsHoldCooldowns(*program, cellCount))
    {
        ReportCellsTooSmall(*program, programId, cellCount);
        return false;
    }
    return true;
}

bool AiDecideSystem::KeepTicksAtOrBefore(uint32 tick, uint32 cadence, Components::AiAgent& agent)
{
    const bool past = agent.EnteredTick > tick;
    if (past)
        agent.EnteredTick = tick;
    if (agent.NextTick > tick + cadence)
        agent.NextTick = tick + cadence;
    return past;
}

void AiDecideSystem::ReportTicksPastNow(uint32 tick)
{
    if (m_ReportedTicksPastNow)
        return;
    m_ReportedTicksPastNow = true;
    Logger::Log::Warning("AiDecideSystem: an agent entered its state after the AI tick {}, so its ticks are set to "
                         "the AI tick. A scene load into a world that keeps its simulation tick causes this: save "
                         "with SceneWriteOptions::writeSimulationTick and load into a cleared world.",
                         tick);
}

void AiDecideSystem::ReportUnregisteredProgram(uint16 program)
{
    if (std::find(m_ReportedPrograms.begin(), m_ReportedPrograms.end(), program) != m_ReportedPrograms.end())
        return;
    m_ReportedPrograms.push_back(program);
    Logger::Log::Error("AiDecideSystem: program {} is not registered, so its agents do not decide. Set "
                       "AiAgent::Program to an id that AiRegistry::AddProgram or CompileStateTree returned, or to "
                       "kNoProgram.",
                       program);
}

void AiDecideSystem::ReportRemovedRegistration(uint16 program)
{
    if (std::find(m_ReportedRemovedPrograms.begin(), m_ReportedRemovedPrograms.end(), program) !=
        m_ReportedRemovedPrograms.end())
        return;
    m_ReportedRemovedPrograms.push_back(program);
    Logger::Log::Error("AiDecideSystem: program {} uses a task, a condition or an input that its module removed, so "
                       "its agents do not decide. Load the module that registers it, or set AiAgent::Program to a "
                       "program whose registrations are all loaded.",
                       program);
}

void AiDecideSystem::ReportCellsTooSmall(const AiProgram& program, uint16 programId, size_t cellCount)
{
    const std::pair<uint16, size_t> key{programId, cellCount};
    if (std::find(m_ReportedCells.begin(), m_ReportedCells.end(), key) != m_ReportedCells.end())
        return;
    m_ReportedCells.push_back(key);
    Logger::Log::Error("AiDecideSystem: the agent's {} cells cannot hold cooldown cells {} to {} of program {}, so its "
                       "agents with {} cells do not decide. Give them a larger cell class (AiCellsSmall, AiCellsMedium "
                       "or AiCellsLarge) or lower AiProgram::CooldownCell.",
                       cellCount, program.CooldownCell, program.CooldownCell + program.Options.Options.size() - 1,
                       programId, cellCount);
}

void AiDecideSystem::DecideProgram(const ECS::World& world, uint32 tick, uint64 seed, size_t begin, size_t end)
{
    const AiProgram* program = m_Registry.FindProgram(m_Due[begin].Program);
    assert(program != nullptr && "AiDecideSystem: CollectAgent admits only registered programs");
    const size_t count = end - begin;

    m_Entities.resize(count);
    m_States.resize(count);
    for (size_t index = begin; index < end; ++index)
    {
        const Components::AiAgent& agent = *m_Due[index].Agent;
        m_Entities[index - begin] = m_Due[index].Entity;
        m_States[index - begin] = {agent.DrawSeed, agent.EnteredTick,
                                   agent.PathDepth > 0 ? agent.ActivePath[0] : Components::AiAgent::kNoState,
                                   ReadyTicks(*program, m_Due[index].Cells)};
    }

    m_Columns.resize(program->ColumnInputs.size() * count);
    m_Chosen.resize(count);
    m_Scores.resize(count);
    FillCallingThreadColumns(world, *program);

    const ProgramBatch batch{program, tick, seed, begin, count};
    JobSystem::ParallelFor(
        world.GetJobSystem(), count, [this, &world, &batch](size_t chunkBegin, size_t chunkEnd)
        { DecideChunk(world, batch, chunkBegin, chunkEnd); }, kAgentsPerJob);
}

void AiDecideSystem::FillCallingThreadColumns(const ECS::World& world, const AiProgram& program)
{
    const size_t count = m_Entities.size();
    m_CallingColumns.resize(program.ColumnInputs.size() * count);
    for (size_t column = 0; column < program.ColumnInputs.size(); ++column)
    {
        const uint16 input = program.ColumnInputs[column];
        if (m_Registry.GetInputThreading(input) != AiInputThreading::CallingThread)
            continue;
        const std::span<float32> out(m_CallingColumns.data() + column * count, count);
        m_Registry.GetInput(input)(world, m_Entities, out);
    }
}

void AiDecideSystem::DecideChunk(const ECS::World& world, const ProgramBatch& batch, size_t chunkBegin, size_t chunkEnd)
{
    const AiProgram& program = *batch.Program;
    const size_t rowCount = chunkEnd - chunkBegin;
    const size_t columnCount = program.ColumnInputs.size();
    // The chunk's columns: column c of local row r is at columns[c * rowCount + r].
    const std::span<float32> columns(m_Columns.data() + columnCount * chunkBegin, columnCount * rowCount);
    const std::span<const ECS::EntityHandle> entities(m_Entities.data() + chunkBegin, rowCount);
    for (size_t column = 0; column < columnCount; ++column)
    {
        const uint16 input = program.ColumnInputs[column];
        const std::span<float32> out = columns.subspan(column * rowCount, rowCount);
        if (m_Registry.GetInputThreading(input) == AiInputThreading::JobThreads)
            m_Registry.GetInput(input)(world, entities, out);
        else
            std::copy_n(m_CallingColumns.data() + column * batch.Count + chunkBegin, rowCount, out.begin());
    }

    program.Options.Select(columns, std::span(m_States).subspan(chunkBegin, rowCount), batch.Tick, batch.Seed,
                           std::span(m_Chosen).subspan(chunkBegin, rowCount),
                           std::span(m_Scores).subspan(chunkBegin, rowCount));
    for (size_t local = chunkBegin; local < chunkEnd; ++local)
        RunAgent(world, program, batch.Tick, batch.Begin + local, local);
}

void AiDecideSystem::RunAgent(const ECS::World& world, const AiProgram& program, uint32 tick, size_t index, size_t local)
{
    const AiDueAgent& due = m_Due[index];
    Components::AiAgent& agent = *due.Agent;
    AiDecisionRow& row = m_DueRows[index];
    row.Agent = due.Entity;
    const AiRegistry& registry = m_Registry;
    AiTaskContext context(world, due.Entity, tick, due.Cells, row, registry.GetCommandCount(), m_Groups);

    const uint16 chosen = m_Chosen[local];
    const uint16 active = agent.PathDepth > 0 ? agent.ActivePath[0] : Components::AiAgent::kNoState;
    if (chosen != active)
    {
        if (active != Components::AiAgent::kNoState)
        {
            const AiTaskDefinition& task = registry.GetTask(program.Options.Options[active].Action);
            if (task.Exit != nullptr)
                task.Exit(context, AiTaskExitReason::Preempted);
            StartCooldown(program, due.Cells, active, tick);
        }
        agent.ActivePath[0] = chosen;
        agent.PathDepth = chosen != AI::OptionSet::kNoOption ? 1 : 0;
        agent.EnteredTick = tick;
        if (chosen != AI::OptionSet::kNoOption)
        {
            const AiTaskDefinition& task = registry.GetTask(program.Options.Options[chosen].Action);
            if (task.Enter != nullptr)
                task.Enter(context);
        }
    }
    agent.PathProgram = due.Program;
    agent.ActiveScore = m_Scores[local];
    agent.NextTick = tick + std::max<uint32>(1, agent.Cadence);
    const AiTraceOutcome outcome =
        chosen == AI::OptionSet::kNoOption ? AiTraceOutcome::None : TickChosen(context, program, index, chosen, tick);
    if (due.Trace != nullptr)
        WriteTrace(*due.Trace, {tick, agent.EnteredTick, m_Scores[local], due.Program, active, chosen, outcome}, row);
}

AiTraceOutcome AiDecideSystem::TickChosen(AiTaskContext& context, const AiProgram& program, size_t index,
                                          uint16 chosen, uint32 tick)
{
    const AiDueAgent& due = m_Due[index];
    const AiTaskDefinition& task = m_Registry.GetTask(program.Options.Options[chosen].Action);
    const AiTaskStatus status = task.Tick(context);
    if (status == AiTaskStatus::Running)
        return AiTraceOutcome::Running;
    if (task.Exit != nullptr)
        task.Exit(context, status == AiTaskStatus::Succeeded ? AiTaskExitReason::Completed : AiTaskExitReason::Failed);
    StartCooldown(program, due.Cells, chosen, tick);
    due.Agent->ActivePath[0] = Components::AiAgent::kNoState;
    due.Agent->PathDepth = 0;
    return ToTraceOutcome(status);
}

} // namespace GameEngine::AIECS
