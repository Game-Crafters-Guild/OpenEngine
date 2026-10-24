#pragma once

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

#include "Types/Types.h"

namespace GameEngine::Components
{
struct NavigationAgent;
struct NavigationAgentState;
} // namespace GameEngine::Components

namespace GameEngine::PathfindingECS
{

// Drops every GridMap cell the agent holds on its map. No-op if it holds
// none, the nav service is gone, or its map is not a grid.
void ClearNavigationAgentReservations(Components::NavigationAgentState& state);

// Drop the corridor and restamp occupancy on the cell the agent stands in.
void StampNavigationAgentOccupancy(const Components::NavigationAgent& agent,
                                   Components::NavigationAgentState& state,
                                   float32 posX, float32 posZ, float32 currentTime);

// Registers the component-teardown hooks that free a NavigationSurface's
// NavigationWorld map and drop an agent's cell reservations and crowd agent when
// its NavigationAgentState is removed. Covers World::Clear (scene close),
// World::DestroyEntity and a plain component remove alike — the paths a scene
// open actually takes.
void RegisterNavigationWorldHooks(ECS::World& world);

} // namespace GameEngine::PathfindingECS
