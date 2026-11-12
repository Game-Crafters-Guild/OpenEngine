#include "AI/StateTree/StateTreeInterpreter.h"

#include "AI/StateTree/StateTreeHost.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace GameEngine::AI
{

namespace
{

using State = StateTreeProgram::State;

bool IsDone(uint32 doneTasks, uint32 task)
{
    return ((doneTasks >> task) & 1u) != 0;
}

void EnterTask(const StateTreeHost& host, const StateTreeProgram& program, uint32 task)
{
    if (host.EnterTask)
        host.EnterTask(host.User, program, task);
}

void ExitTask(const StateTreeHost& host, const StateTreeProgram& program, uint32 task, StateTreeExitReason reason)
{
    if (host.ExitTask)
        host.ExitTask(host.User, program, task, reason);
}

StateTreeExitReason ExitReasonOf(StateTreeTaskStatus status)
{
    return status == StateTreeTaskStatus::Succeeded ? StateTreeExitReason::Completed : StateTreeExitReason::Failed;
}

// The first modifier of `kind` on `state`, or null.
const StateTreeModifier* FindModifier(const StateTreeProgram& program, const State& state, StateTreeModifierKind kind)
{
    for (const StateTreeModifier& modifier : program.GetModifiers().subspan(state.FirstModifier, state.ModifierCount))
    {
        if (modifier.Kind == kind)
            return &modifier;
    }
    return nullptr;
}

bool CompletionMatches(StateTreeCompletion on, StateTreeTaskStatus result)
{
    return on == StateTreeCompletion::Any ||
           (on == StateTreeCompletion::Succeeded) == (result == StateTreeTaskStatus::Succeeded);
}

bool SameTasks(const StateTreeProgram& oldProgram, const State& oldState, const StateTreeProgram& newProgram,
               const State& newState)
{
    if (oldState.TaskCount != newState.TaskCount || oldState.Join != newState.Join)
        return false;
    for (uint16 task = 0; task < oldState.TaskCount; ++task)
    {
        if (oldProgram.GetTasks()[oldState.FirstTask + task].Kind != newProgram.GetTasks()[newState.FirstTask + task].Kind)
            return false;
    }
    return true;
}

// True when a Condition, Timeout or Event transition fires; a Completion transition fires in Complete.
bool Fires(const StateTreeProgram::Transition& transition, const StateTreeHost& host, std::span<const uint16> events,
           uint32 activeTicks)
{
    switch (transition.Trigger)
    {
    case StateTreeTrigger::Condition:
        return host.TestCondition(host.User, transition.Condition);
    case StateTreeTrigger::Timeout:
        return activeTicks >= transition.TimeoutTicks;
    case StateTreeTrigger::Event:
        return std::find(events.begin(), events.end(), transition.Event) != events.end();
    case StateTreeTrigger::Completion:
        return false;
    }
    return false;
}

// False when a frame of the path names a state that the program does not have: the path belongs to another program.
bool IsPathIn(const StateTreeState& state, const StateTreeProgram& program)
{
    for (uint8 depth = 0; depth < state.Depth; ++depth)
    {
        if (state.Path[depth].State >= program.GetStates().size())
            return false;
    }
    return true;
}

} // namespace

void StateTreeInterpreter::Bind(StateTreeState& state, std::span<uint32> readyTicks, uint64 agent, uint64 seed)
{
    m_State = &state;
    m_ReadyTicks = readyTicks;
    m_Agent = agent;
    m_Seed = seed;
}

void StateTreeInterpreter::Start(const StateTreeProgram& program, const StateTreeHost& host, uint32 tick)
{
    assert(m_ReadyTicks.size() >= program.GetCooldownSlotCount() && "StateTreeInterpreter: bind a cell per cooldown slot");
    m_State->Depth = 0;
    m_State->Result = StateTreeTaskStatus::Running;
    std::fill(m_ReadyTicks.begin(), m_ReadyTicks.end(), 0u);
    PushState(program, host, 0, tick);
    Descend(program, host, tick);
}

void StateTreeInterpreter::Step(const StateTreeProgram& program, const StateTreeHost& host,
                                std::span<const uint16> events, uint32 tick)
{
    for (uint8 depth = 0; depth < m_State->Depth; ++depth)
    {
        if (CheckFrame(program, host, events, depth, tick))
            return;
    }
    if (m_State->Depth == 0 || Descend(program, host, tick))
        return;
    for (uint8 depth = 0; depth < m_State->Depth; ++depth)
    {
        const StateTreeTaskStatus result = TickTasks(program, host, m_State->Path[depth]);
        if (result != StateTreeTaskStatus::Running)
        {
            Complete(program, host, depth, result, tick);
            return;
        }
    }
}

bool StateTreeInterpreter::Remap(const StateTreeProgram& oldProgram, const StateTreeProgram& newProgram,
                                 const StateTreeHost& host, uint32 tick)
{
    if (!IsPathIn(*m_State, oldProgram))
    {
        Start(newProgram, host, tick);
        return false;
    }
    if (m_State->Depth == 0)
        return true;
    std::array<uint16, StateTreeProgram::kMaxDepth> newStates{};
    uint8 keep = 0;
    for (; keep < m_State->Depth; ++keep)
    {
        const uint16 state = newProgram.FindState(oldProgram.GetStates()[m_State->Path[keep].State].StableId);
        const uint16 expectedParent = keep == 0 ? kStateTreeNone : newStates[keep - 1];
        if (state == kStateTreeNone || newProgram.GetStates()[state].Parent != expectedParent)
            break;
        newStates[keep] = state;
    }
    assert(m_ReadyTicks.size() >= std::max(oldProgram.GetCooldownSlotCount(), newProgram.GetCooldownSlotCount()) &&
           "StateTreeInterpreter: bind a cell per cooldown slot of both programs");
    PopTo(oldProgram, host, keep, StateTreeExitReason::Reloaded, tick);
    for (uint8 depth = 0; depth < keep; ++depth)
    {
        Frame& frame = m_State->Path[depth];
        const State& oldState = oldProgram.GetStates()[frame.State];
        const State& newState = newProgram.GetStates()[newStates[depth]];
        if (SameTasks(oldProgram, oldState, newProgram, newState))
        {
            frame.State = newStates[depth];
            continue;
        }
        StopTasks(oldProgram, host, frame, StateTreeExitReason::Reloaded);
        frame.State = newStates[depth];
        StartTasks(newProgram, host, frame);
    }
    std::fill(m_ReadyTicks.begin(), m_ReadyTicks.end(), 0u);
    if (keep == 0)
        PushState(newProgram, host, 0, tick);
    Descend(newProgram, host, tick);
    return true;
}

bool StateTreeInterpreter::Stop(const StateTreeProgram& program, const StateTreeHost& host)
{
    if (!IsPathIn(*m_State, program))
    {
        m_State->Depth = 0;
        return false;
    }
    for (; m_State->Depth > 0; --m_State->Depth)
        StopTasks(program, host, m_State->Path[m_State->Depth - 1], StateTreeExitReason::Preempted);
    return true;
}

void StateTreeInterpreter::PushState(const StateTreeProgram& program, const StateTreeHost& host, uint16 state,
                                     uint32 tick)
{
    Frame& frame = m_State->Path[m_State->Depth++];
    frame = {};
    frame.State = state;
    frame.EnteredTick = tick;
    const StateTreeModifier* repeat = FindModifier(program, program.GetStates()[state], StateTreeModifierKind::Repeat);
    frame.RepeatsLeft = repeat ? static_cast<uint16>(repeat->Value) : 0;
    StartTasks(program, host, frame);
}

void StateTreeInterpreter::StartTasks(const StateTreeProgram& program, const StateTreeHost& host, Frame& frame) const
{
    const State& state = program.GetStates()[frame.State];
    frame.DoneTasks = 0;
    frame.TaskCursor = 0;
    if (state.TaskCount == 0)
        return;
    if (state.Join == StateTreeJoin::Sequence)
    {
        EnterTask(host, program, state.FirstTask);
        return;
    }
    for (uint16 task = 0; task < state.TaskCount; ++task)
        EnterTask(host, program, state.FirstTask + task);
}

void StateTreeInterpreter::StopTasks(const StateTreeProgram& program, const StateTreeHost& host, Frame& frame,
                                     StateTreeExitReason reason) const
{
    const State& state = program.GetStates()[frame.State];
    if (state.Join == StateTreeJoin::Sequence)
    {
        if (frame.TaskCursor < state.TaskCount)
            ExitTask(host, program, state.FirstTask + frame.TaskCursor, reason);
        frame.TaskCursor = state.TaskCount;
        return;
    }
    for (uint16 task = 0; task < state.TaskCount; ++task)
    {
        if (!IsDone(frame.DoneTasks, task))
            ExitTask(host, program, state.FirstTask + task, reason);
        frame.DoneTasks |= 1u << task;
    }
}

void StateTreeInterpreter::PopTo(const StateTreeProgram& program, const StateTreeHost& host, uint8 depth,
                                 StateTreeExitReason reason, uint32 tick)
{
    while (m_State->Depth > depth)
    {
        Frame& frame = m_State->Path[m_State->Depth - 1];
        if (frame.State >= program.GetStates().size())
        {
            --m_State->Depth;
            continue;
        }
        StopTasks(program, host, frame, reason);
        const State& state = program.GetStates()[frame.State];
        if (state.CooldownSlot != kStateTreeNone)
        {
            // The ready tick saturates at the last tick, so a long cooldown never wraps to a past tick.
            const uint32 ticks = FindModifier(program, state, StateTreeModifierKind::Cooldown)->Value;
            m_ReadyTicks[state.CooldownSlot] =
                static_cast<uint32>(std::min<uint64>(uint64{tick} + ticks, std::numeric_limits<uint32>::max()));
        }
        --m_State->Depth;
    }
}

bool StateTreeInterpreter::Descend(const StateTreeProgram& program, const StateTreeHost& host, uint32 tick)
{
    bool entered = false;
    while (m_State->Depth > 0 && program.GetStates()[m_State->Path[m_State->Depth - 1].State].ChildCount > 0)
    {
        const uint16 child = ChooseChild(program, host, static_cast<uint8>(m_State->Depth - 1), tick);
        if (child == kStateTreeNone)
            break;
        PushState(program, host, child, tick);
        entered = true;
    }
    return entered;
}

uint16 StateTreeInterpreter::ChooseChild(const StateTreeProgram& program, const StateTreeHost& host, uint8 depth,
                                         uint32 tick)
{
    const State& state = program.GetStates()[m_State->Path[depth].State];
    if (state.Policy == StateTreePolicy::Utility)
        return ChooseUtilityChild(program, host, depth, tick);
    if (state.Policy != StateTreePolicy::Random)
    {
        for (uint16 child = state.FirstChild; child < state.FirstChild + state.ChildCount; ++child)
        {
            if (CanEnter(program, host, child, tick))
                return child;
        }
        return kStateTreeNone;
    }

    // Random: a draw over the weights of the children that can be entered; a weight of 0 is never drawn.
    float32 total = 0.0f;
    m_Columns.assign(state.ChildCount, 0.0f);
    for (uint16 child = 0; child < state.ChildCount; ++child)
    {
        if (CanEnter(program, host, state.FirstChild + child, tick))
            m_Columns[child] = program.GetStates()[state.FirstChild + child].Weight;
        total += m_Columns[child];
    }
    if (total <= 0.0f)
        return kStateTreeNone;
    const float32 draw = Hashing::UnitDraw(m_Seed, m_Agent, tick, depth) * total;
    float32 sum = 0.0f;
    uint16 last = kStateTreeNone;
    for (uint16 child = 0; child < state.ChildCount; ++child)
    {
        if (m_Columns[child] <= 0.0f)
            continue;
        sum += m_Columns[child];
        last = static_cast<uint16>(state.FirstChild + child);
        if (draw < sum)
            return last;
    }
    // The rounding of the sum can leave the draw at the total: the last child with a weight takes it.
    return last;
}

uint16 StateTreeInterpreter::ChooseUtilityChild(const StateTreeProgram& program, const StateTreeHost& host, uint8 depth,
                                                uint32 tick)
{
    const Frame& frame = m_State->Path[depth];
    const State& state = program.GetStates()[frame.State];
    const StateTreeProgram::UtilitySet& set = program.GetUtilitySets()[state.UtilitySet];
    m_Columns.assign(set.ColumnCount, 0.0f);
    if (host.FillColumns && set.ColumnCount > 0)
        host.FillColumns(host.User, frame.State, m_Columns);

    OptionSet::Row row;
    row.Agent = m_Agent;
    if (depth + 1 < m_State->Depth)
    {
        row.Active = static_cast<uint16>(m_State->Path[depth + 1].State - state.FirstChild);
        row.EnteredTick = m_State->Path[depth + 1].EnteredTick;
    }
    // A child that cannot be entered is never ready; the active child is always ready.
    m_ChildReadyTicks.resize(state.ChildCount);
    for (uint16 child = 0; child < state.ChildCount; ++child)
    {
        const bool ready = child == row.Active || CanEnter(program, host, state.FirstChild + child, tick);
        m_ChildReadyTicks[child] = ready ? 0 : std::numeric_limits<uint32>::max();
    }
    row.ReadyTicks = m_ChildReadyTicks;

    uint16 chosen = OptionSet::kNoOption;
    float32 score = 0.0f;
    set.Options.Select(m_Columns, {&row, 1}, tick, m_Seed, {&chosen, 1}, {&score, 1});
    return chosen == OptionSet::kNoOption ? kStateTreeNone : static_cast<uint16>(state.FirstChild + chosen);
}

bool StateTreeInterpreter::CanEnter(const StateTreeProgram& program, const StateTreeHost& host, uint16 state,
                                    uint32 tick) const
{
    const State& entry = program.GetStates()[state];
    if (entry.CooldownSlot != kStateTreeNone && tick < m_ReadyTicks[entry.CooldownSlot])
        return false;
    return entry.EnterCondition == kStateTreeNone || host.TestCondition(host.User, entry.EnterCondition);
}

bool StateTreeInterpreter::CheckFrame(const StateTreeProgram& program, const StateTreeHost& host,
                                      std::span<const uint16> events, uint8 depth, uint32 tick)
{
    const Frame& frame = m_State->Path[depth];
    const State& state = program.GetStates()[frame.State];
    for (const StateTreeModifier& modifier : program.GetModifiers().subspan(state.FirstModifier, state.ModifierCount))
    {
        if (modifier.Kind != StateTreeModifierKind::AbortIf || !host.TestCondition(host.User, modifier.Condition))
            continue;
        PopTo(program, host, static_cast<uint8>(depth + 1), StateTreeExitReason::Preempted, tick);
        StopTasks(program, host, m_State->Path[depth], StateTreeExitReason::Preempted);
        Complete(program, host, depth, StateTreeTaskStatus::Failed, tick);
        return true;
    }
    for (const StateTreeProgram::Transition& transition :
         program.GetTransitions().subspan(state.FirstTransition, state.TransitionCount))
    {
        if (Fires(transition, host, events, tick - frame.EnteredTick))
        {
            TransitionTo(program, host, transition.Target, tick);
            return true;
        }
    }
    if (state.Policy != StateTreePolicy::Utility || depth + 1 >= m_State->Depth)
        return false;
    const uint16 chosen = ChooseUtilityChild(program, host, depth, tick);
    if (chosen == kStateTreeNone || chosen == m_State->Path[depth + 1].State)
        return false;
    PopTo(program, host, static_cast<uint8>(depth + 1), StateTreeExitReason::Preempted, tick);
    PushState(program, host, chosen, tick);
    Descend(program, host, tick);
    return true;
}

void StateTreeInterpreter::TransitionTo(const StateTreeProgram& program, const StateTreeHost& host, uint16 target,
                                        uint32 tick)
{
    std::array<uint16, StateTreeProgram::kMaxDepth> chain{};
    const uint8 targetDepth = program.GetStates()[target].Depth;
    for (uint16 state = target; state != kStateTreeNone; state = program.GetStates()[state].Parent)
        chain[program.GetStates()[state].Depth] = state;

    // The target and the states below the shared ancestors exit; a transition to an active state enters it again.
    uint8 keep = 0;
    while (keep < m_State->Depth && keep < targetDepth && m_State->Path[keep].State == chain[keep])
        ++keep;
    PopTo(program, host, keep, StateTreeExitReason::Preempted, tick);
    for (uint8 depth = keep; depth <= targetDepth; ++depth)
        PushState(program, host, chain[depth], tick);
    Descend(program, host, tick);
}

StateTreeTaskStatus StateTreeInterpreter::TickTasks(const StateTreeProgram& program, const StateTreeHost& host,
                                                    Frame& frame) const
{
    const State& state = program.GetStates()[frame.State];
    if (state.Join == StateTreeJoin::Sequence)
    {
        if (frame.TaskCursor >= state.TaskCount)
            return StateTreeTaskStatus::Running;
        const uint32 task = state.FirstTask + frame.TaskCursor;
        const StateTreeTaskStatus status = host.TickTask(host.User, program, task);
        if (status == StateTreeTaskStatus::Running)
            return status;
        ExitTask(host, program, task, ExitReasonOf(status));
        if (status == StateTreeTaskStatus::Failed || ++frame.TaskCursor == state.TaskCount)
        {
            frame.TaskCursor = state.TaskCount;
            return status;
        }
        EnterTask(host, program, state.FirstTask + frame.TaskCursor);
        return StateTreeTaskStatus::Running;
    }

    StateTreeTaskStatus result = StateTreeTaskStatus::Running;
    for (uint16 task = 0; task < state.TaskCount && result == StateTreeTaskStatus::Running; ++task)
    {
        if (IsDone(frame.DoneTasks, task))
            continue;
        const StateTreeTaskStatus status = host.TickTask(host.User, program, state.FirstTask + task);
        if (status == StateTreeTaskStatus::Running)
            continue;
        ExitTask(host, program, state.FirstTask + task, ExitReasonOf(status));
        frame.DoneTasks |= 1u << task;
        const bool decides = state.Join == StateTreeJoin::Any ||
                             (state.Join == StateTreeJoin::Primary && task == 0) ||
                             (state.Join == StateTreeJoin::All && status == StateTreeTaskStatus::Failed);
        if (decides)
            result = status;
    }
    const uint32 allTasks = state.TaskCount == StateTreeProgram::kMaxTasksPerState ? 0xFFFFFFFFu
                                                                                : (1u << state.TaskCount) - 1u;
    if (state.Join == StateTreeJoin::All && result == StateTreeTaskStatus::Running && state.TaskCount > 0 &&
        frame.DoneTasks == allTasks)
        result = StateTreeTaskStatus::Succeeded;
    if (result != StateTreeTaskStatus::Running)
        StopTasks(program, host, frame, StateTreeExitReason::Preempted);
    return result;
}

void StateTreeInterpreter::Complete(const StateTreeProgram& program, const StateTreeHost& host, uint8 depth,
                                    StateTreeTaskStatus result, uint32 tick)
{
    for (;;)
    {
        PopTo(program, host, static_cast<uint8>(depth + 1), StateTreeExitReason::Preempted, tick);
        Frame& frame = m_State->Path[depth];
        const State& state = program.GetStates()[frame.State];
        if (result == StateTreeTaskStatus::Succeeded && frame.RepeatsLeft > 0)
        {
            --frame.RepeatsLeft;
            frame.EnteredTick = tick;
            StartTasks(program, host, frame);
            Descend(program, host, tick);
            return;
        }
        for (const StateTreeProgram::Transition& transition :
             program.GetTransitions().subspan(state.FirstTransition, state.TransitionCount))
        {
            if (transition.Trigger == StateTreeTrigger::Completion && CompletionMatches(transition.On, result))
            {
                TransitionTo(program, host, transition.Target, tick);
                return;
            }
        }
        const uint16 completed = frame.State;
        PopTo(program, host, depth, StateTreeExitReason::Preempted, tick);
        if (depth == 0)
        {
            m_State->Result = result;
            return;
        }
        --depth;
        const State& parent = program.GetStates()[m_State->Path[depth].State];
        if (parent.Policy == StateTreePolicy::Sequence && result == StateTreeTaskStatus::Succeeded)
        {
            for (uint16 next = completed + 1; next < parent.FirstChild + parent.ChildCount; ++next)
            {
                if (CanEnter(program, host, next, tick))
                {
                    PushState(program, host, next, tick);
                    Descend(program, host, tick);
                    return;
                }
            }
        }
    }
}

} // namespace GameEngine::AI
