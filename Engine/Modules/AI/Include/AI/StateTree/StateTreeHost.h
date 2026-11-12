#pragma once

#include "AI/StateTree/StateTreeTypes.h"
#include "Types/Types.h"

#include <span>

namespace GameEngine::AI
{

class StateTreeProgram;

/// What a state tree calls to run one agent's tasks and to read its conditions. `task` is an index in the
/// tasks of `program` (a reload exits tasks of the old program and enters tasks of the new one), `state` an
/// index in StateTreeProgram::GetStates. Each function gets `User`.
/// EnterTask, ExitTask and FillColumns may be null; FillColumns is needed only by a program with a
/// Utility state.
struct StateTreeHost
{
    void* User = nullptr;
    void (*EnterTask)(void* user, const StateTreeProgram& program, uint32 task) = nullptr;
    StateTreeTaskStatus (*TickTask)(void* user, const StateTreeProgram& program, uint32 task) = nullptr;
    void (*ExitTask)(void* user, const StateTreeProgram& program, uint32 task, StateTreeExitReason reason) = nullptr;
    bool (*TestCondition)(void* user, uint16 condition) = nullptr;
    /// Writes the input columns that the Utility state `state` scores its children with:
    /// columns[c] is column c of the state's option set.
    void (*FillColumns)(void* user, uint16 state, std::span<float32> columns) = nullptr;
};

} // namespace GameEngine::AI
