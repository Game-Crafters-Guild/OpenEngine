#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

#include <array>
#include <type_traits>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::AIECS
{

/// A message to one agent or to one group. A state-tree transition with "on": "event" fires when the agent
/// gets an event of its type. Each addressed agent gets the event once, at its first step after Tick, whatever
/// its cadence. An event stays for the largest cadence of the world's agents, then goes.
struct AiEvent
{
    /// The id that AiRegistry::RegisterEventType returned for the event name.
    uint16 Type = 0;
    /// The entity that posted the event; an invalid handle for game code with no entity.
    ECS::EntityHandle Source;
    /// An agent, or a group entity: each agent whose AiStateTree::Group is the target gets the event.
    ECS::EntityHandle Target;
    /// The AI tick of the post. PostAiEvent and AiTaskContext::PostEvent set it.
    uint32 Tick = 0;
    /// Two values that the game gives a meaning to, for the tasks that read the event.
    std::array<uint32, 2> Payload{};
};

static_assert(std::is_trivially_copyable_v<AiEvent>);

/// Posts an event from game code on the main thread, while no AI system runs. Sets AiEvent::Tick to the
/// world's AI tick. Events of one tick reach the agents ordered by Source, then in post order.
/// @return false when the world has no AiClock singleton; an error names the fix.
/// An accepted event waits for the next AI tick of the world. A world without AI systems never delivers it.
bool PostAiEvent(ECS::World& world, AiEvent event);

} // namespace GameEngine::AIECS
