#pragma once

#include "AI/OptionSet.h"
#include "AI/StateTree/StateTreeTypes.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::AI
{

/// A state tree as a designer writes it. Each state has a stable id that does not change when the
/// state is renamed or moved; a reload keeps an agent in the states whose ids remain.
/// StateTreeProgram::Compile turns a definition into a program.
struct StateTreeDefinition
{
    /// The ParentId of the root state.
    static constexpr uint32 kNoParent = 0xFFFFFFFF;

    /// A transition to the state with the stable id `TargetId`.
    struct Transition
    {
        StateTreeTrigger Trigger = StateTreeTrigger::Condition;
        /// The result a Completion transition accepts.
        StateTreeCompletion On = StateTreeCompletion::Any;
        /// The condition of a Condition transition.
        uint16 Condition = kStateTreeNone;
        /// The ticks of a Timeout transition.
        uint32 TimeoutTicks = 0;
        uint32 TargetId = 0;
        /// The event of an Event transition: an index in the events that StateTreeInterpreter::Step gets.
        uint16 Event = kStateTreeNone;
    };

    struct State
    {
        uint32 Id = 0;
        uint32 ParentId = kNoParent;
        StateTreePolicy Policy = StateTreePolicy::First;
        StateTreeJoin Join = StateTreeJoin::Sequence;
        /// A condition that must be true for a parent's policy to choose this state. kStateTreeNone: always.
        uint16 EnterCondition = kStateTreeNone;
        /// The task kinds, in order. The host reads the kind of a task from StateTreeProgram::GetTasks.
        std::vector<uint16> Tasks;
        /// Checked in order; the first that fires wins.
        std::vector<Transition> Transitions;
        std::vector<StateTreeModifier> Modifiers;
        /// The weight of this state when a Random parent draws among its children.
        float32 Weight = 1.0f;
        /// For a Utility state: option i scores child i, in the children's order. Option::Action is not read.
        OptionSet Utility;
    };

    /// The children of a state run in the order they have in this list.
    std::vector<State> States;
};

} // namespace GameEngine::AI
