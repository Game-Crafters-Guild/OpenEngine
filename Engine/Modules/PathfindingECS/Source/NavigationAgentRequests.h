#pragma once

// An agent's request between Navigation::SetDestination and the solve: which surface the agent walks,
// what holds its request back, and the log line each reason gets once.

#include "ECS/ECS.h"
#include "Pathfinding/PathfindingTypes.h"
#include "PathfindingECS/NavigationScriptApi.h"

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Pathfinding
{
class INavigationMap;
} // namespace GameEngine::Pathfinding

namespace GameEngine::Components
{
struct NavigationAgent;
struct NavigationAgentState;
struct NavigationSurface;
} // namespace GameEngine::Components

namespace GameEngine::PathfindingECS
{

// Binds every agent to the map of its surface for this update (NavigationAgentState's map handle),
// resolving each surface entity once. Holds an order back,
// Pending with its reason, while its agent has no surface, has a dynamic physics body, or its
// surface's map holds no completed bake; an agent on a surface that bakes again keeps its path. Once
// the map holds a bake, a destination SetDestination left unchecked is moved onto the walkable
// ground, or refused with DestinationOffSurface. Main thread, while no path solve runs.
void BindNavigationAgents(ECS::World& world);

// True for the reasons BindNavigationAgents holds a request back for: the request is not solved
// while its Reason is one of them.
bool HoldsRequestBack(Navigation::StatusReason reason);

// The reason a solve that came back Failed failed: the agent or the destination is too far from the
// map's walkable ground, else nothing connects them.
Navigation::StatusReason ClassifyFailedSolve(const Pathfinding::INavigationMap& map,
                                              const Pathfinding::PathRequest& request);

// Records `reason` on the request, and logs it when the request has not logged it yet.
void ReportRequestReason(ECS::EntityHandle agent, Components::NavigationAgentState& state,
                         Navigation::StatusReason reason);

} // namespace GameEngine::PathfindingECS
