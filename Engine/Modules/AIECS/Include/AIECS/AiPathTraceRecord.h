#pragma once

#include "AI/StateTree/StateTreeTypes.h"
#include "Types/Types.h"

#include <array>

namespace GameEngine::AIECS
{

/// One decision of one state-tree agent: the active path after the step. The record has no padding, so
/// two traces compare byte for byte.
struct AiPathTraceRecord
{
    static constexpr uint32 kMaxDepth = 8;
    static constexpr uint16 kNoState = 0xFFFF;

    /// The AI tick of the decision.
    uint32 Tick = 0;
    uint16 Program = 0;
    /// The number of active states; 0 after the root completes.
    uint8 Depth = 0;
    /// Running while the root is active; else the result the root completed with.
    AI::StateTreeTaskStatus Result = AI::StateTreeTaskStatus::Running;
    /// The active states from the root down, as indices in the program's states; kNoState below Depth.
    std::array<uint16, kMaxDepth> States{kNoState, kNoState, kNoState, kNoState,
                                         kNoState, kNoState, kNoState, kNoState};
};

static_assert(sizeof(AiPathTraceRecord) == 24, "AiPathTraceRecord must have no padding");

} // namespace GameEngine::AIECS
