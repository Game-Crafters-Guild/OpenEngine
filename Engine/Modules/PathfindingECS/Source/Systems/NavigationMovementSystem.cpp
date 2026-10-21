#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "Components/Transform.h"
#include "Components/TransformDirtyFeed.h"
#include "ECS/Query.h"
#include "Mathematics/Interpolation.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/PathBuffer.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/ObstacleAvoidance.h"
#include "Pathfinding/PathfindingTypes.h"

#include <cmath>
#include <algorithm>

namespace GameEngine::PathfindingECS
{

namespace
{

constexpr float32 kReservationLookaheadSeconds = 1.5f;

// Hierarchy rebuilds WorldTransform from local Transform on topology
// changes. A world-only write is discarded and the agent snaps home.
void WriteAgentTranslation(ECS::World& world, ECS::EntityHandle e,
                           Components::Transform& xf, Components::WorldTransform& wt,
                           float32 x, float32 y, float32 z)
{
    xf.matrix[12] = x;
    xf.matrix[13] = y;
    xf.matrix[14] = z;
    wt.matrix[12] = x;
    wt.matrix[13] = y;
    wt.matrix[14] = z;
    Components::BumpWorldTransform(world, e, wt);
}

void AdvanceWaypoints(Components::NavigationAgentState& state, const Pathfinding::PathBuffer& pathBuffer)
{
    Pathfinding::PathHandle pathHandle{state.PathIndex, state.PathGeneration};
    uint32 pointCount = pathBuffer.GetPointCount(pathHandle);

    state.CurrentPathPointIndex++;

    if (state.CurrentPathPointIndex >= pointCount)
    {
        // Reached end of path — mark Complete (not Invalid) so callers can
        // distinguish successful arrival from a failed/cancelled path.
        state.HasDestination = false;
        state.Status = static_cast<uint8>(Pathfinding::PathStatus::Complete);
        state.VelocityX = 0.0f;
        state.VelocityY = 0.0f;
        state.VelocityZ = 0.0f;
        return;
    }

    Pathfinding::PathPoint current = pathBuffer.GetPoint(pathHandle, state.CurrentPathPointIndex);
    state.CurrentWaypointX = current.X;
    state.CurrentWaypointY = current.Y;
    state.CurrentWaypointZ = current.Z;

    uint32 nextIdx = state.CurrentPathPointIndex + 1;
    if (nextIdx < pointCount)
    {
        Pathfinding::PathPoint next = pathBuffer.GetPoint(pathHandle, nextIdx);
        state.NextWaypointX = next.X;
        state.NextWaypointY = next.Y;
        state.NextWaypointZ = next.Z;
    }
    else
    {
        state.NextWaypointX = current.X;
        state.NextWaypointY = current.Y;
        state.NextWaypointZ = current.Z;
    }
}

void ReserveFootprint(Pathfinding::GridMap& gridMap, uint32 cellX, uint32 cellZ,
                      uint32 agentId, float32 expirationTime, float32 agentRadius,
                      bool occupancy)
{
    const int32 r = static_cast<int32>(gridMap.FootprintRadiusInCells(agentRadius));
    for (int32 rdz = -r; rdz <= r; ++rdz)
    {
        for (int32 rdx = -r; rdx <= r; ++rdx)
        {
            const int32 rx = static_cast<int32>(cellX) + rdx;
            const int32 rz = static_cast<int32>(cellZ) + rdz;
            if (rx >= 0 && rz >= 0)
                gridMap.ReserveCell(static_cast<uint32>(rx), static_cast<uint32>(rz),
                                    agentId, expirationTime, occupancy);
        }
    }
}

void ReserveCellsAlongPath(const Components::NavigationAgent& agent,
                           Components::NavigationAgentState& state,
                           Pathfinding::NavigationWorld& navWorld,
                           float32 posX, float32 posZ,
                           float32 currentTime)
{
    Pathfinding::NavMapHandle mapHandle{agent.NavMapIndex, agent.NavMapGeneration};
    auto* gridMap = navWorld.GetGridMap(mapHandle);
    if (!gridMap)
        return;

    // Drop the previous stamp first so cells the agent has left are not
    // taxed for the remaining reservation TTL.
    if (agent.AgentId != 0)
        gridMap->ClearReservationsForAgent(agent.AgentId);
    state.HasCellReservations = false;

    Pathfinding::PathHandle pathHandle{state.PathIndex, state.PathGeneration};
    auto& pathBuffer = navWorld.GetPathBuffer();
    uint32 pointCount = pathBuffer.GetPointCount(pathHandle);

    float32 lookaheadDist = agent.Speed * kReservationLookaheadSeconds;
    float32 accumulatedDist = 0.0f;
    float32 prevX = posX;
    float32 prevZ = posZ;

    float32 expirationTime = currentTime + kReservationLookaheadSeconds;

    uint32 hereX = 0;
    uint32 hereZ = 0;
    if (gridMap->WorldToCell(posX, posZ, hereX, hereZ))
        ReserveFootprint(*gridMap, hereX, hereZ, agent.AgentId, expirationTime, agent.Radius, true);

    for (uint32 i = state.CurrentPathPointIndex; i < pointCount && accumulatedDist < lookaheadDist; ++i)
    {
        Pathfinding::PathPoint pt = pathBuffer.GetPoint(pathHandle, i);

        float32 dx = pt.X - prevX;
        float32 dz = pt.Z - prevZ;
        accumulatedDist += std::sqrt(dx * dx + dz * dz);
        prevX = pt.X;
        prevZ = pt.Z;

        uint32 cellX = 0;
        uint32 cellZ = 0;
        if (gridMap->WorldToCell(pt.X, pt.Z, cellX, cellZ))
            ReserveFootprint(*gridMap, cellX, cellZ, agent.AgentId, expirationTime, agent.Radius, false);
    }

    if (agent.AgentId != 0)
        state.HasCellReservations = true;
}

void ReserveStandingCell(const Components::NavigationAgent& agent,
                         Components::NavigationAgentState& state,
                         Pathfinding::NavigationWorld& /*navWorld*/,
                         float32 posX, float32 posZ,
                         float32 currentTime)
{
    StampNavigationAgentOccupancy(agent, state, posX, posZ, currentTime);
}

// A peer may already overlap the agent when a spawn, teleport or another
// movement owner publishes its position. Only occupancy newly entered by this
// update is an obstruction: rejecting the existing footprint would repeatedly
// replan to the same start-cell centre without ever letting the agent leave.
// This value is local to one update; it grants no permission to reenter later.
struct StartingFootprint
{
    uint32 X = 0;
    uint32 Z = 0;
    uint32 Radius = 0;
    bool Valid = false;

    bool Contains(uint32 x, uint32 z) const
    {
        return Valid && std::abs(static_cast<int64>(x) - X) <= Radius &&
               std::abs(static_cast<int64>(z) - Z) <= Radius;
    }
};

bool NewCellOccupiedByOther(const Pathfinding::GridMap& grid, uint32 x, uint32 z,
                            const StartingFootprint& start, uint32 agentId, float32 currentTime)
{
    return !start.Contains(x, z) && grid.IsOccupiedByOther(x, z, agentId, currentTime);
}

bool NewPositionOccupiedByOther(const Pathfinding::GridMap& grid, float32 x, float32 z,
                                const StartingFootprint& start, uint32 agentId, float32 currentTime)
{
    uint32 cx = 0;
    uint32 cz = 0;
    if (!grid.WorldToCell(x, z, cx, cz))
        return false;
    return NewCellOccupiedByOther(grid, cx, cz, start, agentId, currentTime);
}

// The segment clears every blocked cell outside `openCells` by `radius`. A
// zero radius has no swept test to exempt cells from, so it takes Raycast's
// line and forgives a first hit inside `openCells`.
bool SegmentClearOfWalls(const Pathfinding::GridMap& grid, float32 startX, float32 startZ,
                         float32 endX, float32 endZ, float32 radius,
                         const Pathfinding::GridMap::CellRange* openCells)
{
    if (radius > 0.0f)
        return grid.IsSegmentClear(startX, startZ, endX, endZ, radius, openCells);

    float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    if (!grid.Raycast(startX, 0.0f, startZ, endX, 0.0f, endZ, hitX, hitY, hitZ))
        return true;
    uint32 hitCx = 0, hitCz = 0;
    return openCells && grid.WorldToCell(hitX, hitZ, hitCx, hitCz) && openCells->Contains(hitCx, hitCz);
}

} // anonymous namespace

// Runtime-spawned agents (no authored NavigationAgent in repo scenes) still
// hit this query. Write local Transform and WorldTransform together —
// TransformHierarchySystem rebuilds world from local on topology changes.
//
// The agent query declares Write<WorldTransform>, and columns are stamped
// at visit rather than on assignment (Query.h), so every visited agent chunk has
// its WorldTransform column stamped every frame even when no agent moves. That
// defeats Changed<WorldTransform> gates on those chunks, and HLODSelectSystem
// probes that column with an unscoped one-component query — an authored agent in
// a scene with baked HLOD clusters would re-run its per-member staleness scan
// every frame. Narrow the visit once a skip path exists that still writes both.
void NavigationMovementSystem::Update(ECS::World& world, float32 deltaTime)
{
    auto* navWorld = NavigationService::TryGet();
    if (!navWorld)
        return;

    m_AccumulatedTime += deltaTime;
    auto& pathBuffer = navWorld->GetPathBuffer();
    auto& crowd = navWorld->GetCrowdManager();

    // Step the crowd simulation first so position/velocity reads below
    // reflect the latest state. Running this after reads would introduce
    // a 1-frame lag where agents appear stationary on their first frame.
    if (crowd.IsInitialized())
    {
        crowd.Update(deltaTime);
    }

    // Dynamic-obstacle application lives in NavigationSurfaceSystem: dynamic-blocked
    // is an unsynchronized GridMap field that path solves read, so its writes
    // sit behind that system's HasActivePathJobs gate. This system only reads
    // map content (Raycast / IsCellBlocked) and writes reservations, after the
    // pathfinding system's solves have finished.

    // Manage crowd agent lifecycle for navmesh-based agents
    if (crowd.IsInitialized())
    {
        world.Query<ECS::Write<Components::NavigationAgent>,
                    ECS::Write<Components::NavigationAgentState>,
                    ECS::Read<Components::WorldTransform>>().Each(
            [&](ECS::EntityHandle /*e*/, Components::NavigationAgent& agent,
                Components::NavigationAgentState& state,
                const Components::WorldTransform& wt)
            {
                Pathfinding::NavMapHandle mapHandle{agent.NavMapIndex, agent.NavMapGeneration};
                bool isNavMeshAgent = (navWorld->GetDetourNavMap(mapHandle) != nullptr);
                if (!isNavMeshAgent)
                    return;

                // The crowd steers toward the destination. PathDirty stays set:
                // it is the request NavigationPathfindingSystem serves, and one
                // the solve budget deferred is still queued.
                // Create crowd agent for navmesh agents that need one
                if (state.CrowdAgentHandle < 0 && state.HasDestination)
                {
                    Pathfinding::CrowdAgentParams params;
                    params.Radius = agent.Radius;
                    params.Height = agent.Height;
                    params.MaxSpeed = agent.Speed;
                    params.MaxAcceleration = agent.Acceleration;
                    params.AvoidanceQuality = static_cast<Pathfinding::ObstacleAvoidanceQuality>(agent.AvoidanceQuality);
                    params.SeparationWeight = agent.SeparationWeight;

                    float32 posX = wt.matrix[12];
                    float32 posY = wt.matrix[13];
                    float32 posZ = wt.matrix[14];

                    state.CrowdAgentHandle = crowd.AddAgent(posX, posY, posZ, params);
                    if (state.CrowdAgentHandle >= 0)
                    {
                        crowd.SetAgentTarget(state.CrowdAgentHandle,
                                             state.DestinationX, state.DestinationY, state.DestinationZ);
                    }
                    return;
                }

                // Update target when destination changes
                if (state.CrowdAgentHandle >= 0 && state.HasDestination && state.PathDirty)
                {
                    crowd.SetAgentTarget(state.CrowdAgentHandle,
                                         state.DestinationX, state.DestinationY, state.DestinationZ);
                }

                // Remove crowd agent when destination is cleared
                if (state.CrowdAgentHandle >= 0 && !state.HasDestination)
                {
                    crowd.RemoveAgent(state.CrowdAgentHandle);
                    state.CrowdAgentHandle = -1;
                }
            });
    }

    // Move agents along their paths
    world.Query<ECS::Write<Components::NavigationAgent>,
                ECS::Write<Components::NavigationAgentState>,
                ECS::Write<Components::Transform>,
                ECS::Write<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle e, Components::NavigationAgent& agent,
            Components::NavigationAgentState& state,
            Components::Transform& xf,
            Components::WorldTransform& wt)
        {
            // Halt / arrival: drop the lookahead corridor but keep a body
            // stamp on the cell the agent is standing in so others cannot
            // walk through idle units.
            if (!state.HasDestination)
            {
                ReserveStandingCell(agent, state, *navWorld, wt.matrix[12], wt.matrix[14],
                                    m_AccumulatedTime);
                return;
            }

            // Detour crowd-based movement: when the agent has a valid crowd handle,
            // read position and velocity from the crowd simulation instead of steering manually.
            if (state.CrowdAgentHandle >= 0)
            {
                auto& crowd = navWorld->GetCrowdManager();
                if (crowd.IsInitialized() && crowd.IsAgentActive(state.CrowdAgentHandle))
                {
                    float32 cx, cy, cz;
                    crowd.GetAgentPosition(state.CrowdAgentHandle, cx, cy, cz);
                    WriteAgentTranslation(world, e, xf, wt, cx, cy, cz);

                    crowd.GetAgentVelocity(state.CrowdAgentHandle,
                                           state.VelocityX, state.VelocityY, state.VelocityZ);
                    return; // crowd handles movement
                }
            }

            auto status = static_cast<Pathfinding::PathStatus>(state.Status);
            if (status != Pathfinding::PathStatus::Complete &&
                status != Pathfinding::PathStatus::Partial)
            {
                // No path to follow (queued for a solve, Failed): the agent
                // stands, so it reports no velocity.
                state.VelocityX = 0.0f;
                state.VelocityY = 0.0f;
                state.VelocityZ = 0.0f;
                if (status == Pathfinding::PathStatus::Failed)
                    ReserveStandingCell(agent, state, *navWorld, wt.matrix[12], wt.matrix[14],
                                        m_AccumulatedTime);
                return;
            }

            // Proximity check: verify the upcoming path is still walkable.
            // If any cell on the way to the next few waypoints is now blocked
            // (obstacle placed, etc.), trigger immediate re-pathing instead of
            // waiting for the replan timer.
            //
            // Sweeps the agent's body from its position along consecutive
            // waypoints (GridMap::IsSegmentClear, the test the path smoother
            // keeps shortcuts to), so a body pushed off its route that would
            // clip a blocked cell re-paths, and the cells between endpoints of
            // a smoothed straight segment are covered. A hex grid sweeps the
            // line, as Raycast does: IsSegmentClear models a hex cell as a
            // rectangle, and its rows are only sqrt(3)/4 of a cell from a cell
            // centre, so a body wider than that re-paths a valid route every
            // frame and never moves. For large agents, also checks the full
            // footprint at each waypoint, the cell clearance A* solves with.
            Pathfinding::NavMapHandle proxyMapHandle{agent.NavMapIndex, agent.NavMapGeneration};
            auto* proxyGridMap = navWorld->GetGridMap(proxyMapHandle);
            StartingFootprint startFootprint;
            if (proxyGridMap)
            {
                startFootprint.Valid = proxyGridMap->WorldToCell(
                    wt.matrix[12], wt.matrix[14], startFootprint.X, startFootprint.Z);
                startFootprint.Radius = proxyGridMap->FootprintRadiusInCells(agent.Radius);
            }
            if (proxyGridMap && !state.PathDirty)
            {
                Pathfinding::PathHandle proxyPath{state.PathIndex, state.PathGeneration};
                uint32 proxyPointCount = pathBuffer.GetPointCount(proxyPath);
                constexpr uint32 kLookaheadPoints = 5;
                uint32 checkEnd = std::min(state.CurrentPathPointIndex + kLookaheadPoints, proxyPointCount);

                uint32 destCx = 0;
                uint32 destCz = 0;
                const bool destOk = proxyGridMap->WorldToCell(
                    state.DestinationX, state.DestinationZ, destCx, destCz);

                // Dest sits on an obstacle stamp ring. A smoothed shortcut can
                // clip that 8-neighbour stamp every frame, so the ring counts
                // as open unless the dest cell itself is blocked. A blocked
                // cell beyond the ring is a real wall.
                Pathfinding::GridMap::CellRange destRing;
                const bool destRingOpen = destOk && !proxyGridMap->IsCellBlocked(destCx, destCz);
                if (destRingOpen)
                {
                    destRing.MinX = destCx > 0 ? destCx - 1 : 0;
                    destRing.MinZ = destCz > 0 ? destCz - 1 : 0;
                    destRing.MaxX = destCx + 1;
                    destRing.MaxZ = destCz + 1;
                }
                const float32 sweepRadius =
                    proxyGridMap->GetSettings().Type == Pathfinding::GridType::Square ? agent.Radius : 0.0f;

                float32 rayStartX = wt.matrix[12];
                float32 rayStartZ = wt.matrix[14];
                for (uint32 pi = state.CurrentPathPointIndex; pi < checkEnd && !state.PathDirty; ++pi)
                {
                    Pathfinding::PathPoint pt = pathBuffer.GetPoint(proxyPath, pi);

                    if (!SegmentClearOfWalls(*proxyGridMap, rayStartX, rayStartZ, pt.X, pt.Z, sweepRadius,
                                             destRingOpen ? &destRing : nullptr))
                    {
                        state.PathDirty = true;
                        break;
                    }
                    rayStartX = pt.X;
                    rayStartZ = pt.Z;

                    uint32 cellX = 0, cellZ = 0;
                    if (!proxyGridMap->WorldToCell(pt.X, pt.Z, cellX, cellZ))
                        continue;
                    // Same dest-occupancy rule as A*: a body on the dest cell
                    // is a stand-off, not a wall. PathDirty here livelocks a
                    // dest-allowed close (smoothed paths put dest in the
                    // first few lookahead points). The last-step refuse still
                    // stands off instead of entering that cell.
                    const bool destCell = destOk && cellX == destCx && cellZ == destCz;
                    if (!destCell &&
                        NewCellOccupiedByOther(*proxyGridMap, cellX, cellZ, startFootprint,
                                               agent.AgentId, m_AccumulatedTime))
                    {
                        state.PathDirty = true;
                        break;
                    }
                    if (startFootprint.Radius == 0)
                        continue;

                    const int32 r = static_cast<int32>(startFootprint.Radius);
                    for (int32 dz = -r; dz <= r && !state.PathDirty; ++dz)
                    {
                        for (int32 dx = -r; dx <= r && !state.PathDirty; ++dx)
                        {
                            const int32 fx = static_cast<int32>(cellX) + dx;
                            const int32 fz = static_cast<int32>(cellZ) + dz;
                            if (fx < 0 || fz < 0)
                                continue;
                            const bool destFoot =
                                destOk &&
                                static_cast<uint32>(fx) == destCx &&
                                static_cast<uint32>(fz) == destCz;
                            if (proxyGridMap->IsCellBlocked(static_cast<uint32>(fx), static_cast<uint32>(fz)) ||
                                (!destFoot &&
                                 NewCellOccupiedByOther(*proxyGridMap, static_cast<uint32>(fx),
                                                        static_cast<uint32>(fz), startFootprint,
                                                        agent.AgentId, m_AccumulatedTime)))
                                state.PathDirty = true;
                        }
                    }
                }
            }

            float32 posX = wt.matrix[12];
            float32 posY = wt.matrix[13];
            float32 posZ = wt.matrix[14];

            float32 dirX = state.CurrentWaypointX - posX;
            float32 dirY = state.CurrentWaypointY - posY;
            float32 dirZ = state.CurrentWaypointZ - posZ;

            float32 distSq = dirX * dirX + dirY * dirY + dirZ * dirZ;
            float32 stoppingDistSq = agent.StoppingDistance * agent.StoppingDistance;

            // Check if we have reached the current waypoint (squared distance avoids sqrt)
            if (distSq < stoppingDistSq)
            {
                AdvanceWaypoints(state, pathBuffer);
                if (!state.HasDestination)
                {
                    WriteAgentTranslation(world, e, xf, wt, posX, posY, posZ);
                    ReserveStandingCell(agent, state, *navWorld, posX, posZ, m_AccumulatedTime);
                    return;
                }

                dirX = state.CurrentWaypointX - posX;
                dirY = state.CurrentWaypointY - posY;
                dirZ = state.CurrentWaypointZ - posZ;
                distSq = dirX * dirX + dirY * dirY + dirZ * dirZ;
            }

            // Normalize direction (sqrt needed here but only once per agent per frame)
            if (distSq > 1e-12f)
            {
                float32 invDist = 1.0f / std::sqrt(distSq);
                dirX *= invDist;
                dirY *= invDist;
                dirZ *= invDist;
            }

            float32 desiredVx = dirX * agent.Speed;
            float32 desiredVy = dirY * agent.Speed;
            float32 desiredVz = dirZ * agent.Speed;

            float32 t = std::min(1.0f, agent.Acceleration * deltaTime);
            state.VelocityX = Math::Lerp(state.VelocityX, desiredVx, t);
            state.VelocityY = Math::Lerp(state.VelocityY, desiredVy, t);
            state.VelocityZ = Math::Lerp(state.VelocityZ, desiredVz, t);

            const float32 nextX = posX + state.VelocityX * deltaTime;
            const float32 nextY = posY + state.VelocityY * deltaTime;
            const float32 nextZ = posZ + state.VelocityZ * deltaTime;

            if (proxyGridMap &&
                NewPositionOccupiedByOther(*proxyGridMap, nextX, nextZ, startFootprint,
                                           agent.AgentId, m_AccumulatedTime))
            {
                uint32 destCx = 0;
                uint32 destCz = 0;
                uint32 nextCx = 0;
                uint32 nextCz = 0;
                const bool destCell =
                    proxyGridMap->WorldToCell(state.DestinationX, state.DestinationZ, destCx, destCz) &&
                    proxyGridMap->WorldToCell(nextX, nextZ, nextCx, nextCz) &&
                    destCx == nextCx && destCz == nextCz;
                if (destCell)
                {
                    state.HasDestination = false;
                    state.VelocityX = 0.0f;
                    state.VelocityY = 0.0f;
                    state.VelocityZ = 0.0f;
                    WriteAgentTranslation(world, e, xf, wt, posX, posY, posZ);
                    ReserveStandingCell(agent, state, *navWorld, posX, posZ, m_AccumulatedTime);
                    return;
                }
                state.PathDirty = true;
                state.VelocityX = 0.0f;
                state.VelocityY = 0.0f;
                state.VelocityZ = 0.0f;
                WriteAgentTranslation(world, e, xf, wt, posX, posY, posZ);
                ReserveStandingCell(agent, state, *navWorld, posX, posZ, m_AccumulatedTime);
                return;
            }

            posX = nextX;
            posY = nextY;
            posZ = nextZ;

            WriteAgentTranslation(world, e, xf, wt, posX, posY, posZ);

            // Reserve cells along path for grid-based agents
            Pathfinding::NavMapHandle mapHandle{agent.NavMapIndex, agent.NavMapGeneration};
            if (navWorld->GetGridMap(mapHandle))
            {
                if (state.PathDirty)
                {
                    ReserveStandingCell(agent, state, *navWorld, posX, posZ, m_AccumulatedTime);
                }
                else
                {
                    ReserveCellsAlongPath(agent, state, *navWorld, posX, posZ, m_AccumulatedTime);
                }
            }
        });

    // Clear expired reservations on all grid maps using the current accumulated time
    world.Query<ECS::Read<Components::NavigationSurface>>().Each(
        [&](ECS::EntityHandle /*e*/, const Components::NavigationSurface& source)
        {
            auto* gridMap = ResolveNavigationSurfaceGrid(world, source);
            if (gridMap)
            {
                gridMap->ClearExpiredReservations(m_AccumulatedTime);
            }
        });

}

} // namespace GameEngine::PathfindingECS
