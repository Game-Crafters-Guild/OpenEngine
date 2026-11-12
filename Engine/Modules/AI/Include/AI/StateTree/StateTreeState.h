#pragma once

#include "AI/StateTree/StateTreeProgram.h"
#include "AI/StateTree/StateTreeTypes.h"
#include "Types/Types.h"

#include <array>
#include <type_traits>

namespace GameEngine::AI
{

/// One agent's place in a state tree: the active path from the root down and the result. Plain data, so a
/// component can hold it; StateTreeInterpreter reads and writes it.
struct StateTreeState
{
    /// One active state of the path.
    struct Frame
    {
        uint32 EnteredTick = 0;
        /// Bit i: task i of the state stopped (the All, Any and Primary joins).
        uint32 DoneTasks = 0;
        uint16 State = kStateTreeNone;
        /// The running task of the Sequence join; TaskCount when all stopped.
        uint16 TaskCursor = 0;
        /// The runs left of a Repeat modifier.
        uint16 RepeatsLeft = 0;
    };

    std::array<Frame, StateTreeProgram::kMaxDepth> Path{};
    uint8 Depth = 0;
    /// Running while the root is active; else the result the root completed with.
    StateTreeTaskStatus Result = StateTreeTaskStatus::Running;
};

static_assert(std::is_trivially_copyable_v<StateTreeState>);

} // namespace GameEngine::AI
