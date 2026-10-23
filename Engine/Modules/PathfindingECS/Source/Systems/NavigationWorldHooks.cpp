#include "PathfindingECS/Systems/NavigationWorldHooks.h"

#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"

#include "ECS/Entity.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"

namespace GameEngine::PathfindingECS
{

void ClearNavigationAgentReservations(Components::NavigationAgentState& state)
{
    if (state.CellReservationOwner == 0)
        return;

    auto* navWorld = NavigationService::TryGet();
    if (!navWorld)
        return;

    Pathfinding::NavMapHandle mapHandle{state.NavMapIndex, state.NavMapGeneration};
    if (auto* gridMap = navWorld->GetGridMap(mapHandle))
        gridMap->ClearReservationsForAgent(state.CellReservationOwner);
    state.CellReservationOwner = 0;
}

void StampNavigationAgentOccupancy(const Components::NavigationAgent& agent,
                                   Components::NavigationAgentState& state,
                                   float32 posX, float32 posZ, float32 currentTime)
{
    if (agent.AgentId == 0)
        return;
    auto* navWorld = NavigationService::TryGet();
    if (!navWorld)
        return;
    Pathfinding::NavMapHandle mapHandle{state.NavMapIndex, state.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    if (!gridMap)
        return;

    gridMap->ClearReservationsForAgent(agent.AgentId);
    state.CellReservationOwner = 0;

    uint32 hereX = 0;
    uint32 hereZ = 0;
    if (!gridMap->WorldToCell(posX, posZ, hereX, hereZ))
        return;

    constexpr float32 kReservationLookaheadSeconds = 1.5f;
    const int32 r = static_cast<int32>(gridMap->FootprintRadiusInCells(agent.Radius));
    const float32 expirationTime = currentTime + kReservationLookaheadSeconds;
    for (int32 rdz = -r; rdz <= r; ++rdz)
    {
        for (int32 rdx = -r; rdx <= r; ++rdx)
        {
            const int32 rx = static_cast<int32>(hereX) + rdx;
            const int32 rz = static_cast<int32>(hereZ) + rdz;
            if (rx >= 0 && rz >= 0)
                gridMap->ReserveCell(static_cast<uint32>(rx), static_cast<uint32>(rz),
                                     agent.AgentId, expirationTime, true);
        }
    }
    state.CellReservationOwner = agent.AgentId;
}

// The world a surface leaves releases its map only when that world owns it: a copy of the
// component's bytes in another world names the original's map.
void ReleaseNavigationSurfaceInWorld(const ECS::World& world, ECS::EntityHandle /*entity*/,
                                     Components::NavigationSurface& surface)
{
    ResetNavigationSurfaceMap(world, surface);
}

// The state names the agent's map, the id its cells are stamped under and its crowd agent, which
// an arrived agent keeps. A hook runs under the world's lock, so it reads only the component it is
// given.
void ReleaseNavigationAgentState(Components::NavigationAgentState& state)
{
    ClearNavigationAgentReservations(state);
    // A handle from an earlier crowd names a slot another agent may now hold.
    auto* navigation = NavigationService::TryGet();
    if (navigation && state.CrowdAgentHandle >= 0 &&
        state.CrowdGeneration == navigation->GetCrowdManager().GetGeneration())
        navigation->GetCrowdManager().RemoveAgent(state.CrowdAgentHandle);
    state.CrowdAgentHandle = -1;
}

void RegisterNavigationWorldHooks(ECS::World& world)
{
    world.RegisterOnRemove<Components::NavigationSurface>(&ReleaseNavigationSurfaceInWorld);
    world.RegisterOnRemove<Components::NavigationAgentState>(&ReleaseNavigationAgentState);
}

} // namespace GameEngine::PathfindingECS
