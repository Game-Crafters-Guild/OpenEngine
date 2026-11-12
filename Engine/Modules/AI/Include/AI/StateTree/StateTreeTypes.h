#pragma once

#include "Types/Types.h"

namespace GameEngine::AI
{

/// The index a state tree uses for "no state", "no condition" and "no slot".
inline constexpr uint16 kStateTreeNone = 0xFFFF;

/// How a state with children chooses the child it runs.
enum class StateTreePolicy : uint8
{
    First,    ///< The first child, in order, whose entry condition passes.
    Sequence, ///< The children in order: each starts after the one before it succeeds.
    Utility,  ///< The child with the best utility score; the state scores its children again at each step.
    Random,   ///< A seeded draw over the children, in proportion to their weights.
};

/// How the tasks of one state run together, and when the state completes.
enum class StateTreeJoin : uint8
{
    Sequence, ///< One task at a time, in order. All succeed: Succeeded. One fails: Failed.
    All,      ///< Side by side. All succeed: Succeeded. One fails: Failed.
    Any,      ///< Side by side. The first task that stops gives its result.
    Primary,  ///< Side by side. The first task gives the result; the other tasks only run beside it.
};

/// What makes a transition fire.
enum class StateTreeTrigger : uint8
{
    Condition,  ///< Its condition is true at a step.
    Completion, ///< Its state completes with the result in `On`.
    Timeout,    ///< Its state is active for `TimeoutTicks` ticks.
    Event,      ///< Its event is in the events of the step.
};

/// The result a completion transition accepts.
enum class StateTreeCompletion : uint8
{
    Any,
    Succeeded,
    Failed,
};

/// A rule on a state that holds while the state is active, or that guards its entry.
enum class StateTreeModifierKind : uint8
{
    AbortIf,  ///< When `Condition` is true, the state stops and completes with Failed.
    Cooldown, ///< After the state stops, nothing enters it for `Value` ticks.
    Repeat,   ///< After the state succeeds, it runs again, `Value` more times, before it completes.
};

/// One modifier of a state.
struct StateTreeModifier
{
    StateTreeModifierKind Kind = StateTreeModifierKind::AbortIf;
    /// The condition of AbortIf; kStateTreeNone for the other kinds.
    uint16 Condition = kStateTreeNone;
    /// The ticks of Cooldown, the count of Repeat.
    uint32 Value = 0;
};

/// The result of one tick of a task, and of a state tree.
enum class StateTreeTaskStatus : uint8
{
    Running,
    Succeeded,
    Failed,
};

/// Why a task stops.
enum class StateTreeExitReason : uint8
{
    Completed, ///< Its tick returned Succeeded.
    Failed,    ///< Its tick returned Failed.
    Preempted, ///< A transition, a modifier, a utility choice or the join of its state stopped it.
    Reloaded,  ///< Its state is not in the reloaded program, or its state's task list changed.
};

} // namespace GameEngine::AI
