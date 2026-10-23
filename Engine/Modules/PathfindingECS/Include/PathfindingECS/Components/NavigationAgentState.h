#pragma once
#include "Pathfinding/PathfindingTypes.h"
#include "PathfindingECS/NavigationScriptApi.h"
#include "Types/Types.h"
#include <type_traits>

namespace GameEngine::Components
{

// Runtime state for navigation agents (written every frame by movement/pathfinding systems).
// Separated from NavigationAgent config for cache efficiency - systems that only
// read config (e.g., debug display) don't pollute cache lines with hot runtime data.
struct NavigationAgentState
{
    // Destination
    float32 DestinationX = 0.0f;
    float32 DestinationY = 0.0f;
    float32 DestinationZ = 0.0f;
    bool HasDestination = false;
    bool PathDirty = true;
    // Set by NavigationPathfindingSystem's replan timer: a refresh of a path
    // that is still valid, so the agent keeps walking it until the new solve
    // lands. PathDirty (orders, the corridor checks) drops the path instead.
    bool ReplanRequested = false;
    // The AgentId under which this agent holds stamped cells (body or lookahead) on its map; 0 while
    // it holds none.
    uint32 CellReservationOwner = 0;

    // Path reference (PathHandle packed)
    uint32 PathIndex = 0;
    uint32 PathGeneration = 0;
    uint32 CurrentPathPointIndex = 0;

    // Cached waypoints (written by pathfinding, read by movement)
    float32 CurrentWaypointX = 0.0f;
    float32 CurrentWaypointY = 0.0f;
    float32 CurrentWaypointZ = 0.0f;
    float32 NextWaypointX = 0.0f;
    float32 NextWaypointY = 0.0f;
    float32 NextWaypointZ = 0.0f;

    // Velocity from steering
    float32 VelocityX = 0.0f;
    float32 VelocityY = 0.0f;
    float32 VelocityZ = 0.0f;

    // The order as Navigation::GetStatus reports it. Status is the path's: Invalid with no order,
    // Pending while the order waits, Complete or Partial while the agent walks and after it arrives
    // (HasDestination false), Failed. Reason says why a Failed or Pending order does not move the
    // agent; PendingTiles counts the surface tiles an order waiting for its surface's first bake
    // waits for.
    Pathfinding::PathStatus Status = Pathfinding::PathStatus::Invalid;
    Navigation::StatusReason Reason = Navigation::StatusReason::None;
    uint32 PendingTiles = 0;
    // The last Reason logged for this order (runtime, not saved): each reason is logged once when
    // the order reaches it, not on every retry. None after a path is found or a new order.
    Navigation::StatusReason LoggedReason = Navigation::StatusReason::None;
    // Set by Navigation::SetDestination: the destination is moved onto the surface's walkable ground,
    // or refused, once the surface has baked tiles (runtime, not saved).
    bool DestinationUnchecked = false;

    // The map of the surface the agent's Surface resolves to, written by NavigationPathfindingSystem
    // each update (runtime, not saved); invalid while the agent has no surface or has a dynamic body.
    uint32 NavMapIndex = 0;
    uint32 NavMapGeneration = 0;

    // Detour crowd handle (-1 = not in crowd) and the crowd generation it was added under (runtime,
    // not saved). A handle from another generation counts as no handle: a new-layout bake starts
    // the crowd again, empty.
    int32 CrowdAgentHandle = -1;
    uint32 CrowdGeneration = 0;
    // Counts the orders Navigation::SetDestination gave (runtime, not saved). The crowd agent steers
    // to the destination of order CrowdOrder, and takes the current destination when the two differ.
    uint32 Order = 0;
    uint32 CrowdOrder = 0;
    // Seconds the crowd agent has stood almost still with a straight line to its goal and a
    // touching neighbor holding it off (runtime, not saved): a member of a group ordered to one
    // point arrives this way at the pile around it.
    float32 CrowdStallSeconds = 0.0f;

    // Replan timer (runtime, not saved)
    float32 ReplanTimer = 0.0f;

    // The agent's place in NavigationPathfindingSystem's solve queue (runtime,
    // not saved): set the first Update a request (HasDestination with PathDirty
    // or ReplanRequested) is seen, kept while it waits, 0 when the agent is
    // not queued.
    uint32 PathRequestSequence = 0;
};

static_assert(std::is_trivially_copyable_v<NavigationAgentState>);
static_assert(std::is_standard_layout_v<NavigationAgentState>);

} // namespace GameEngine::Components
