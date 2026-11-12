#pragma once

#include "Types/Types.h"

#include <string>

namespace GameEngine::AI
{

/// The part of a state that a compile refusal names.
enum class StateTreeCompileItem : uint8
{
    Program,        ///< The whole program, not one state.
    State,          ///< A field of the state itself: its id, parent, weight or depth.
    EnterCondition, ///< The state's entry condition (enterIf).
    Task,           ///< The task at ItemIndex in the state's task list.
    Transition,     ///< The transition at ItemIndex in the state's transition list.
    Modifier,       ///< The modifier at ItemIndex in the state's modifier list.
    Option,         ///< The option of the state's child at ItemIndex (a Utility state).
};

/// Why a state-tree program does not compile, and where. One refusal at a time: the first one found.
struct StateTreeCompileError
{
    /// The StateId of a refusal of the whole program.
    static constexpr uint32 kNoState = 0xFFFFFFFF;

    /// The refusal and its fix, in plain words, with the state id.
    std::string Message;
    /// The stable id of the state, or kNoState.
    uint32 StateId = kNoState;
    StateTreeCompileItem Item = StateTreeCompileItem::Program;
    /// The index of the item in its list; 0 for Program and State.
    uint16 ItemIndex = 0;
};

} // namespace GameEngine::AI
