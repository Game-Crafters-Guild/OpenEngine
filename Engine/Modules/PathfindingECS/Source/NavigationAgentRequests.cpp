#include "NavigationAgentRequests.h"

#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"
#include "PhysicsECS/Components/PhysicsBody.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Pathfinding/INavigationMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/PathBuffer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <string>

namespace GameEngine::PathfindingECS
{
namespace
{
using Components::NavigationAgent;
using Components::NavigationAgentState;
using Components::NavigationSurface;
using Navigation::StatusReason;

// How far from walkable ground a destination may lie and still be moved onto it: up or down, and
// across in radii of the agent.
constexpr float32 kDestinationHeightTolerance = 5.0f;
constexpr float32 kDestinationRadiiAcross = 2.0f;
// How far the log line of a refused destination looks for the nearest walkable ground.
constexpr float32 kNearestGroundSearch = 100.0f;

bool IsBaked(const NavigationSurface& surface)
{
    return surface.BakePhase == Components::NavigationBakePhase::Baked;
}

// An order solves on a surface whose map holds a completed bake. A rebake, a bake that waits for
// terrain and a bake that failed keep those tiles serving, so agents walk them meanwhile.
bool ServesABake(const NavigationSurface& surface, Pathfinding::NavMapHandle map)
{
    return map.IsValid() && surface.MapHoldsBake;
}

// The mesh tiles a surface that has not finished baking has left to bake. A grid bakes whole in one
// update, so it has none to count.
uint32 PendingTilesOf(const NavigationSurface& surface)
{
    if (IsBaked(surface) || surface.Mode != Components::NavigationSurfaceMode::Mesh)
        return 0;
    return surface.BakeTotal > surface.BakeDone ? surface.BakeTotal - surface.BakeDone : 0;
}

std::string FormatMeters(float32 value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(value));
    return text;
}

std::string FormatPoint(float32 x, float32 y, float32 z)
{
    return "(" + FormatMeters(x) + ", " + FormatMeters(y) + ", " + FormatMeters(z) + ")";
}

void ReleasePath(NavigationAgentState& state)
{
    const Pathfinding::PathHandle path{state.PathIndex, state.PathGeneration};
    if (path.IsValid())
        NavigationService::Get().GetPathBuffer().FreePath(path);
    state.PathIndex = 0;
    state.PathGeneration = 0;
}

void LogOnce(ECS::EntityHandle agent, NavigationAgentState& state, StatusReason reason, const std::string& text)
{
    if (state.LoggedReason == reason)
        return;
    state.LoggedReason = reason;
    Logger::Log::Warning("[PathfindingECS] Navigation agent on entity {}: {}", agent.id, text);
}

std::string ReasonText(StatusReason reason, const NavigationAgentState& state)
{
    switch (reason)
    {
    case StatusReason::NoSurface:
        return "it has no navigation surface to walk: set its Surface to an entity with a NavigationSurface, or keep "
               "exactly one NavigationSurface in the scene";
    case StatusReason::SurfaceNotBaked:
        return "its request waits for its navigation surface to finish baking";
    case StatusReason::DestinationOffSurface:
        return "its destination " + FormatPoint(state.DestinationX, state.DestinationY, state.DestinationZ) +
               " is too far from the walkable ground of its navigation surface";
    case StatusReason::StartOffSurface:
        return "it stands too far from the walkable ground of its navigation surface: move it onto the surface";
    case StatusReason::NoRoute:
        return "no walkable route reaches " + FormatPoint(state.DestinationX, state.DestinationY, state.DestinationZ);
    case StatusReason::DynamicBody:
        return "an agent steers its Transform; a dynamic body would fight it: make the body kinematic or remove it";
    case StatusReason::None:
        break;
    }
    return {};
}

// Holds the agent's request back, Pending, until `reason` clears. The request keeps its destination
// and is solved once BindNavigationAgents no longer finds a reason.
void HoldBack(ECS::EntityHandle agent, NavigationAgentState& state, StatusReason reason, uint32 pendingTiles)
{
    ReleasePath(state);
    state.Status = Pathfinding::PathStatus::Pending;
    state.PathDirty = true;
    state.ReplanRequested = false;
    state.PathRequestSequence = 0;
    state.PendingTiles = pendingTiles;
    state.Reason = reason;
    LogOnce(agent, state, reason, ReasonText(reason, state));
}

float32 HorizontalDistance(float32 ax, float32 az, float32 bx, float32 bz)
{
    return std::sqrt((ax - bx) * (ax - bx) + (az - bz) * (az - bz));
}

// True when walkable ground lies within `reach` across of the point. A grid answers a closest-point
// query with its nearest cell to any point, however far, so the distance is checked too.
bool NearGround(const Pathfinding::INavigationMap& map, float32 px, float32 py, float32 pz, float32 reach)
{
    float32 x = 0.0f, y = 0.0f, z = 0.0f;
    return map.GetClosestNavigablePoint(px, py, pz, reach, x, y, z) && HorizontalDistance(x, z, px, pz) <= reach;
}

// Moves the destination SetDestination asked for onto the walkable ground under or beside it, or
// refuses the request with DestinationOffSurface and the distance to the nearest ground.
void CheckDestination(ECS::EntityHandle agent, NavigationAgentState& state, float32 agentRadius,
                      const Pathfinding::INavigationMap& map)
{
    state.DestinationUnchecked = false;
    const float32 across = kDestinationRadiiAcross * agentRadius;
    float32 x = 0.0f, y = 0.0f, z = 0.0f;
    const bool near = map.GetClosestNavigablePoint(state.DestinationX, state.DestinationY, state.DestinationZ,
                                                   std::max(kDestinationHeightTolerance, across), x, y, z) &&
                      HorizontalDistance(x, z, state.DestinationX, state.DestinationZ) <= across &&
                      std::abs(y - state.DestinationY) <= kDestinationHeightTolerance;
    if (near)
    {
        if (!map.IsPointNavigable(state.DestinationX, state.DestinationY, state.DestinationZ, 0.0f))
        {
            state.DestinationX = x;
            state.DestinationY = y;
            state.DestinationZ = z;
        }
        return;
    }

    std::string text = "its destination " + FormatPoint(state.DestinationX, state.DestinationY, state.DestinationZ) +
                       " is ";
    if (map.GetClosestNavigablePoint(state.DestinationX, state.DestinationY, state.DestinationZ, kNearestGroundSearch,
                                     x, y, z))
    {
        const float32 dx = x - state.DestinationX, dy = y - state.DestinationY, dz = z - state.DestinationZ;
        text += FormatMeters(std::sqrt(dx * dx + dy * dy + dz * dz)) + " m from the nearest walkable ground";
    }
    else
    {
        text += "more than " + FormatMeters(kNearestGroundSearch) + " m from walkable ground";
    }
    text += "; a destination is moved onto the ground only within " + FormatMeters(kDestinationHeightTolerance) +
            " m above or below it and " + FormatMeters(across) + " m across";

    ReleasePath(state);
    state.HasDestination = false;
    state.PathDirty = false;
    state.ReplanRequested = false;
    state.PathRequestSequence = 0;
    state.Status = Pathfinding::PathStatus::Failed;
    state.Reason = StatusReason::DestinationOffSurface;
    LogOnce(agent, state, StatusReason::DestinationOffSurface, text);
}

// The entity of the scene's only NavigationSurface, or an invalid handle when it has none or several.
ECS::EntityHandle FindOnlyNavigationSurface(ECS::World& world)
{
    ECS::EntityHandle only{};
    uint32 count = 0;
    world.Query<ECS::Read<NavigationSurface>>().Each([&](ECS::EntityHandle entity, const NavigationSurface&) {
        only = entity;
        ++count;
    });
    return count == 1 ? only : ECS::EntityHandle{};
}

// The surface entity an agent names (or the only surface) and the map that surface owns this update
// (invalid until the surface first bakes), or NoSurface when the entity has no surface.
struct AgentSurface
{
    ECS::EntityHandle Entity;
    const NavigationSurface* Surface = nullptr;
    Pathfinding::NavMapHandle Map;
    StatusReason Reason = StatusReason::None;
};

// Resolves each surface entity the agents name once per update. It remembers the first
// kSurfacesResolvedOnce entities (a scene has one surface or a few); an agent on any further one
// resolves its own.
class SurfaceResolver
{
public:
    explicit SurfaceResolver(ECS::World& world) : m_World(world) {}

    AgentSurface Resolve(ECS::EntityHandle entity)
    {
        for (std::size_t i = 0; i < m_Count; ++i)
            if (m_Known[i].Entity == entity)
                return m_Known[i];
        AgentSurface surface;
        surface.Entity = entity;
        surface.Surface =
            entity.IsValid() && m_World.IsValid(entity) ? m_World.GetComponent<NavigationSurface>(entity) : nullptr;
        if (surface.Surface)
            surface.Map = ResolveNavigationSurfaceMap(m_World, *surface.Surface);
        else
            surface.Reason = StatusReason::NoSurface;
        if (m_Count < m_Known.size())
            m_Known[m_Count++] = surface;
        return surface;
    }

private:
    static constexpr std::size_t kSurfacesResolvedOnce = 8;
    ECS::World& m_World;
    std::array<AgentSurface, kSurfacesResolvedOnce> m_Known{};
    std::size_t m_Count = 0;
};

void BindAgent(ECS::EntityHandle entity, const NavigationAgent& agent, NavigationAgentState& state,
               const AgentSurface& resolved, bool dynamicBody)
{
    const Pathfinding::NavMapHandle map = dynamicBody ? Pathfinding::NavMapHandle{} : resolved.Map;
    state.NavMapIndex = map.Index;
    state.NavMapGeneration = map.Generation;
    if (!state.HasDestination)
        return;

    if (dynamicBody)
        return HoldBack(entity, state, StatusReason::DynamicBody, 0);
    if (resolved.Reason == StatusReason::NoSurface)
        return HoldBack(entity, state, StatusReason::NoSurface, 0);
    if (!ServesABake(*resolved.Surface, resolved.Map))
        return HoldBack(entity, state, StatusReason::SurfaceNotBaked, PendingTilesOf(*resolved.Surface));

    // Whatever held the order back has cleared: it joins the solve queue.
    state.PendingTiles = 0;
    if (HoldsRequestBack(state.Reason))
        state.Reason = StatusReason::None;
    // The agent's own radius; the surface's, the one its mesh is built for, when the agent has none.
    const float32 agentRadius = agent.Radius > 0.0f ? agent.Radius : resolved.Surface->AgentRadius;
    if (state.DestinationUnchecked)
        if (const auto* navigationMap = NavigationService::Get().GetMap(resolved.Map))
            CheckDestination(entity, state, agentRadius, *navigationMap);
}

void ResetRequest(NavigationAgentState& state)
{
    if (NavigationService::TryGet())
        ReleasePath(state);
    state.HasDestination = false;
    state.PathDirty = false;
    state.ReplanRequested = false;
    state.PathRequestSequence = 0;
    state.DestinationUnchecked = false;
    state.Status = Pathfinding::PathStatus::Invalid;
    state.Reason = StatusReason::None;
    state.LoggedReason = StatusReason::None;
    state.PendingTiles = 0;
    state.VelocityX = 0.0f;
    state.VelocityY = 0.0f;
    state.VelocityZ = 0.0f;
}
} // namespace

void BindNavigationAgents(ECS::World& world)
{
    const ECS::EntityHandle onlySurface = FindOnlyNavigationSurface(world);
    SurfaceResolver surfaces(world);
    const auto surfaceOf = [&](const NavigationAgent& agent) {
        return surfaces.Resolve(agent.Surface.IsValid() ? agent.Surface : onlySurface);
    };

    // The query's archetype tells whether an agent has a physics body, so no agent is looked up.
    world.Query<ECS::Read<NavigationAgent>, ECS::Write<NavigationAgentState>, ECS::Read<Components::PhysicsBody>>()
        .Each([&](ECS::EntityHandle entity, const NavigationAgent& agent, NavigationAgentState& state,
                  const Components::PhysicsBody& body) {
            BindAgent(entity, agent, state, surfaceOf(agent), body.motionType == Physics::MotionType::Dynamic);
        });
    auto withoutBody = world.Query<ECS::Read<NavigationAgent>, ECS::Write<NavigationAgentState>>();
    withoutBody.Without<Components::PhysicsBody>();
    withoutBody.Each([&](ECS::EntityHandle entity, const NavigationAgent& agent, NavigationAgentState& state) {
        BindAgent(entity, agent, state, surfaceOf(agent), false);
    });
}

bool HoldsRequestBack(StatusReason reason)
{
    return reason == StatusReason::NoSurface || reason == StatusReason::SurfaceNotBaked ||
           reason == StatusReason::DynamicBody;
}

StatusReason ClassifyFailedSolve(const Pathfinding::INavigationMap& map, const Pathfinding::PathRequest& request)
{
    // The reach across within which a solve itself finds the walkable ground under its two ends.
    const float32 reach = 4.0f * request.AgentRadius;
    if (!NearGround(map, request.StartX, request.StartY, request.StartZ, reach))
        return StatusReason::StartOffSurface;
    if (!NearGround(map, request.EndX, request.EndY, request.EndZ, reach))
        return StatusReason::DestinationOffSurface;
    return StatusReason::NoRoute;
}

void ReportRequestReason(ECS::EntityHandle agent, NavigationAgentState& state, StatusReason reason)
{
    state.Reason = reason;
    if (reason == StatusReason::None)
    {
        state.LoggedReason = StatusReason::None;
        return;
    }
    LogOnce(agent, state, reason, ReasonText(reason, state));
}

} // namespace GameEngine::PathfindingECS

namespace GameEngine::Navigation
{

bool SetDestination(ECS::World& world, ECS::EntityHandle agent, Mathematics::Vector3 worldPoint)
{
    auto* state = world.HasComponent<Components::NavigationAgent>(agent)
                      ? world.GetComponentForWrite<Components::NavigationAgentState>(agent)
                      : nullptr;
    if (!state)
        return false;
    PathfindingECS::ResetRequest(*state);
    state->DestinationX = worldPoint.x;
    state->DestinationY = worldPoint.y;
    state->DestinationZ = worldPoint.z;
    state->HasDestination = true;
    state->PathDirty = true;
    state->DestinationUnchecked = true;
    state->Status = Pathfinding::PathStatus::Pending;
    ++state->Order;
    return true;
}

void ClearDestination(ECS::World& world, ECS::EntityHandle agent)
{
    if (!world.HasComponent<Components::NavigationAgent>(agent))
        return;
    if (auto* state = world.GetComponentForWrite<Components::NavigationAgentState>(agent))
        PathfindingECS::ResetRequest(*state);
}

AgentStatus GetStatus(ECS::World& world, ECS::EntityHandle agent)
{
    const auto* state = world.HasComponent<Components::NavigationAgent>(agent)
                            ? world.GetComponent<Components::NavigationAgentState>(agent)
                            : nullptr;
    if (!state)
        return {};
    AgentStatus status;
    status.Reason = state->Reason;
    status.PendingTiles = state->PendingTiles;
    status.PartialPath = state->Status == Pathfinding::PathStatus::Partial;
    switch (state->Status)
    {
    case Pathfinding::PathStatus::Invalid: status.State = AgentState::Idle; break;
    case Pathfinding::PathStatus::Pending: status.State = AgentState::Waiting; break;
    case Pathfinding::PathStatus::Complete:
    case Pathfinding::PathStatus::Partial:
        status.State = state->HasDestination ? AgentState::Walking : AgentState::Arrived;
        break;
    case Pathfinding::PathStatus::Failed: status.State = AgentState::Failed; break;
    }
    return status;
}

} // namespace GameEngine::Navigation
