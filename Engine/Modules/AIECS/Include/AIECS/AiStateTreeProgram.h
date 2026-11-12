#pragma once

#include "AI/StateTree/StateTreeProgram.h"
#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::AIECS
{

/// A state-tree program with its names resolved to registry ids: task kind k of Program runs the task
/// Tasks[k], condition c tests the condition Conditions[c], event e is the event type Events[e], and column c
/// of each Utility state reads the input Inputs[c].
struct AiStateTreeProgram
{
    AI::StateTreeProgram Program;
    std::vector<uint16> Tasks;
    std::vector<uint16> Conditions;
    std::vector<uint16> Events;
    std::vector<uint16> Inputs;
    /// The GUID of the program asset, or a fixed GUID a game gives a program it builds in code. Null when
    /// the program has neither.
    GUID StableId;
};

} // namespace GameEngine::AIECS
