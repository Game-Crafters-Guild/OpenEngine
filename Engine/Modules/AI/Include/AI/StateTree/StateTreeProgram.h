#pragma once

#include "AI/OptionSet.h"
#include "AI/StateTree/StateTreeTypes.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::AI
{

struct StateTreeCompileError;
struct StateTreeDefinition;

/// A compiled state tree: flat arrays that the interpreter reads by index. State 0 is the root, and
/// the children of a state are contiguous. Immutable after Compile; any number of agents and threads
/// may read one program at once.
class StateTreeProgram
{
public:
    /// The deepest active path: the root and seven levels below it.
    static constexpr uint8 kMaxDepth = 8;
    /// The most tasks one state can have.
    static constexpr uint16 kMaxTasksPerState = 32;

    struct State
    {
        uint32 StableId = 0;
        uint16 Parent = kStateTreeNone;
        uint16 FirstChild = 0;
        uint16 ChildCount = 0;
        uint16 EnterCondition = kStateTreeNone;
        uint16 FirstTask = 0;
        uint16 TaskCount = 0;
        uint16 FirstTransition = 0;
        uint16 TransitionCount = 0;
        uint16 FirstModifier = 0;
        uint16 ModifierCount = 0;
        /// The index in GetUtilitySets of a Utility state.
        uint16 UtilitySet = kStateTreeNone;
        /// The interpreter's cooldown slot of a state with a Cooldown modifier.
        uint16 CooldownSlot = kStateTreeNone;
        uint8 Depth = 0;
        StateTreePolicy Policy = StateTreePolicy::First;
        StateTreeJoin Join = StateTreeJoin::Sequence;
        float32 Weight = 1.0f;
    };

    struct Task
    {
        uint16 Kind = 0;
        uint16 State = 0;
    };

    struct Transition
    {
        StateTreeTrigger Trigger = StateTreeTrigger::Condition;
        StateTreeCompletion On = StateTreeCompletion::Any;
        uint16 Condition = kStateTreeNone;
        uint16 Target = 0;
        uint16 Event = kStateTreeNone;
        uint32 TimeoutTicks = 0;
    };

    /// The options that score the children of a Utility state, and the column count they read.
    struct UtilitySet
    {
        OptionSet Options;
        uint16 ColumnCount = 0;
    };

    /// Compiles `definition` into `out`. Refused, with the state, the item and the fix in `outError`: no states; more
    /// than 65534 states, tasks, transitions or modifiers in total; a repeated id; no root or two roots; a
    /// parent or transition target not in the list; a state not under the root; a path deeper than kMaxDepth;
    /// more than kMaxTasksPerState tasks; a Condition transition or AbortIf with no condition; an Event
    /// transition with no event; a negative weight; a Utility state whose option count is not its child count.
    static bool Compile(const StateTreeDefinition& definition, StateTreeProgram& out, StateTreeCompileError& outError);

    std::span<const State> GetStates() const { return m_States; }
    std::span<const Task> GetTasks() const { return m_Tasks; }
    std::span<const Transition> GetTransitions() const { return m_Transitions; }
    std::span<const StateTreeModifier> GetModifiers() const { return m_Modifiers; }
    std::span<const UtilitySet> GetUtilitySets() const { return m_UtilitySets; }
    uint16 GetCooldownSlotCount() const { return m_CooldownSlotCount; }

    /// The index of the state with the stable id `stableId`, or kStateTreeNone.
    uint16 FindState(uint32 stableId) const;

private:
    std::vector<State> m_States;
    std::vector<Task> m_Tasks;
    std::vector<Transition> m_Transitions;
    std::vector<StateTreeModifier> m_Modifiers;
    std::vector<UtilitySet> m_UtilitySets;
    // (stable id, state index), sorted by id.
    std::vector<std::pair<uint32, uint16>> m_StatesById;
    uint16 m_CooldownSlotCount = 0;
};

} // namespace GameEngine::AI
