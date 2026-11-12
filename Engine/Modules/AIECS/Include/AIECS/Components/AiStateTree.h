#pragma once

#include "AI/StateTree/StateTreeState.h"
#include "ECS/ECS.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

/// The state of an agent whose AiAgent::Program is a state-tree program. AiDecideSystem starts the tree and
/// steps it at the agent's first decision, then steps it once at each decision. A game adds it with the AiAgent.
struct AiStateTree
{
    static constexpr uint32 kNotStepped = 0xFFFFFFFF;

    /// The active path and the result. AiAgent::ActivePath holds a copy of the path's states, and
    /// AiAgent::PathProgram the program that the path belongs to.
    AI::StateTreeState State;
    /// The group the agent belongs to, or an invalid handle. The agent gets the AI events addressed to it.
    ECS::EntityHandle Group;
    /// The AI tick of the agent's last step, or kNotStepped. The next step gets the events posted at or after it.
    uint32 LastStepTick = kNotStepped;
};

static_assert(std::is_trivially_copyable_v<AiStateTree>);
static_assert(sizeof(AiStateTree) <= 144, "AiStateTree is one fixed-size row per agent");

} // namespace GameEngine::Components
