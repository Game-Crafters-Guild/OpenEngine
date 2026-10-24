#pragma once
#include "Types/Types.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "ECS/ECS.h"
#include "ECS/RequiredComponents.h"
#include <type_traits>

namespace GameEngine::Components
{

// An entity that walks a NavigationSurface. Navigation::SetDestination orders it; NavigationMovementSystem
// moves its Transform, so a physics body on it must be kinematic.
struct NavigationAgent
{
    // The entity whose NavigationSurface the agent walks. Empty: the only surface in the scene.
    // @ge-tooltip The entity whose Navigation Surface the agent walks. Empty: the only surface in the scene.
    ECS::EntityHandle Surface{};

    // Configuration (set once, read by many systems)
    // @ge-tooltip Meters per second at full speed.
    // @ge-range 0
    float32 Speed = 3.5f;
    // @ge-tooltip Meters per second squared: how fast the agent reaches its speed and turns.
    // @ge-range 0
    float32 Acceleration = 8.0f;
    // @ge-tooltip Meters. The agent counts a corner of its path as reached this close to it.
    // @ge-range 0
    float32 StoppingDistance = 0.1f;
    // @ge-tooltip Meters. The agent's size for avoidance and for the cells it holds on a grid.
    // @ge-range 0.01
    float32 Radius = 0.5f;
    // @ge-tooltip Meters. How far above and below the agent a path looks for the surface.
    // @ge-range 0.01
    float32 Height = 2.0f;
    // @ge-tooltip Cut the corners of a grid path where the way between them is clear.
    bool SmoothPaths = true;

    // Obstacle avoidance config
    // @ge-tooltip 0 to 3. Higher looks at more ways around other agents and costs more.
    // @ge-range 0 3
    uint8 AvoidanceQuality = 3;
    // @ge-tooltip How strongly the agent keeps apart from agents near it.
    // @ge-range 0
    float32 SeparationWeight = 2.0f;

    // Replan config
    // @ge-tooltip Seconds between solves of a path the agent walks, so it follows changes on the way. 0: solve once.
    // @ge-range 0
    float32 ReplanInterval = 2.0f;

    // Entity ID used for reservation ownership
    // @ge-hidden
    // [DoNotSerialize]
    uint32 AgentId = 0;
};

static_assert(std::is_trivially_copyable_v<NavigationAgent>);
static_assert(std::is_standard_layout_v<NavigationAgent>);

} // namespace GameEngine::Components

namespace GameEngine::ECS::Detail
{

template<>
struct RequiredComponents<GameEngine::Components::NavigationAgent>
{
    using type = TypeList<GameEngine::Components::NavigationAgentState>;
};

} // namespace GameEngine::ECS::Detail
