#pragma once

#include "AI/ResponseCurve.h"
#include "AI/StateTree/StateTreeDefinition.h"
#include "Types/Types.h"

#include <string>
#include <vector>

namespace GameEngine::AIECS
{

/// A state-tree program file (.scribe) as the author wrote it. Name list X[k] is the name of index k in the
/// definition (task kind, condition, event, input column); each list holds each name once, in first-use order.
struct AiStateTreeSource
{
    /// The curve of one consideration as the file writes it.
    struct Curve
    {
        AI::CurvePreset Preset = AI::CurvePreset::Linear;
        float32 Shape = 1.0f;
        float32 Midpoint = 0.5f;
    };

    /// The text of one state that the definition does not keep.
    struct StateText
    {
        /// Empty when the file gives the state no name.
        std::string Name;
        /// For a Utility state: curve i is the curve of Definition.States[s].Utility.Considerations[i].
        std::vector<Curve> Curves;
    };

    /// The states in the file's order: depth first, each state before its children.
    AI::StateTreeDefinition Definition;
    /// States[s] is the text of Definition.States[s].
    std::vector<StateText> States;
    std::vector<std::string> TaskNames;
    std::vector<std::string> ConditionNames;
    std::vector<std::string> EventNames;
    std::vector<std::string> InputNames;
};

} // namespace GameEngine::AIECS
