#include "DebugServer/NavigationDebugHandlers.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"

#include "Components/Name.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/PathBuffer.h"
#include "Pathfinding/SolverScratchStatistics.h"
#include "PathfindingECS/Components/NavigationSource.h"
#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"
#include "PathfindingECS/Systems/NavigationPathfindingStatistics.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

json HandleNavigationStats(const EditorDebugServer::RequestContext&)
{
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    auto* systems = loop ? loop->GetSystemManager() : nullptr;
    if (!systems)
        return Editor::RefuseRequest("No ECS system manager is running (the rendering loop has not started)");
    const auto* pathfinding = systems->GetSystem<PathfindingECS::NavigationPathfindingSystem>();
    if (!pathfinding)
        return Editor::RefuseRequest(
            "NavigationPathfindingSystem is not in the running schedule (the PathfindingECS systems are not registered)");

    const PathfindingECS::NavigationPathfindingStatistics& stats = pathfinding->GetStatistics();
    const Pathfinding::NavigationWorld* navigation = PathfindingECS::NavigationService::TryGet();
    const Pathfinding::SolverScratchStatistics scratch =
        navigation ? navigation->GetSolverScratchStatistics() : Pathfinding::SolverScratchStatistics{};
    return json{
        {"maxPathSolvesPerFrame", pathfinding->MaxPathSolvesPerFrame},
        {"frames", stats.Frames},
        {"frame",
         {{"newRequests", stats.NewRequests},
          {"solved", stats.Solved},
          {"backlog", stats.Backlog},
          {"oldestWaitFrames", stats.OldestWaitFrames},
          {"oldestWaitSeconds", stats.OldestWaitSeconds},
          {"longestSolvedWaitFrames", stats.LongestSolvedWaitFrames},
          {"longestSolvedWaitSeconds", stats.LongestSolvedWaitSeconds}}},
        {"totals", {{"newRequests", stats.TotalNewRequests}, {"solved", stats.TotalSolved}}},
        {"solverScratch",
         {{"solvers", scratch.Solvers},
          {"solversHoldingScratch", scratch.SolversHoldingScratch},
          {"heldBytes", scratch.HeldBytes},
          {"solverHighWaterBytes", scratch.SolverHighWaterBytes}}},
    };
}

const char* PathStatusName(Pathfinding::PathStatus status)
{
    switch (status)
    {
    case Pathfinding::PathStatus::Invalid: return "Invalid";
    case Pathfinding::PathStatus::Pending: return "Pending";
    case Pathfinding::PathStatus::Complete: return "Complete";
    case Pathfinding::PathStatus::Partial: return "Partial";
    case Pathfinding::PathStatus::Failed: return "Failed";
    }
    return "Invalid";
}

// A world point given as [x, y, z].
std::optional<std::array<float32, 3>> ReadPoint(const json& params, const char* name)
{
    if (!params.contains(name))
        return std::nullopt;
    const auto& point = params[name];
    if (!point.is_array() || point.size() != 3u || !point[0].is_number() || !point[1].is_number() ||
        !point[2].is_number())
        return std::nullopt;
    return std::array<float32, 3>{point[0].get<float32>(), point[1].get<float32>(), point[2].get<float32>()};
}

// The surface the request's entityId names, else the only one in the scene. Null, with the refusal
// in `outRefusal`, when there is none to pick.
std::optional<ECS::EntityHandle> ResolveSurfaceEntity(ECS::World& world, const json& params, json& outRefusal)
{
    if (params.contains("entityId"))
    {
        const ECS::EntityHandle named(params["entityId"].get<uint32_t>());
        if (!world.IsValid(named) || !world.HasComponent<Components::NavigationSurface>(named))
        {
            outRefusal = Editor::RefuseRequest("Entity " + std::to_string(named.id) + " has no NavigationSurface");
            return std::nullopt;
        }
        return named;
    }
    ECS::EntityHandle only{};
    uint32 surfaces = 0;
    world.Query<ECS::Read<Components::NavigationSurface>>().Each(
        [&](ECS::EntityHandle entity, const Components::NavigationSurface&) {
            ++surfaces;
            only = entity;
        });
    if (surfaces == 1)
        return only;
    outRefusal = surfaces == 0 ? Editor::RefuseRequest("The scene has no NavigationSurface: add one with Add Component > "
                                                       "Navigation > Navigation Surface")
                               : Editor::RefuseRequest("The scene has " + std::to_string(surfaces) +
                                                       " NavigationSurfaces: pass entityId to pick one");
    return std::nullopt;
}

json HandleFindPath(const EditorDebugServer::RequestContext& ctx)
{
    const auto from = ReadPoint(ctx.params, "from");
    const auto to = ReadPoint(ctx.params, "to");
    if (!from || !to)
        return Editor::RefuseRequest("Missing from or to — expected [x, y, z] world points");

    auto* world = EngineCore::GetInstance().GetPrimaryWorld();
    auto* navigation = PathfindingECS::NavigationService::TryGet();
    if (!world || !navigation)
        return Editor::RefuseRequest("No world or navigation service is running");

    json refusal;
    const std::optional<ECS::EntityHandle> resolved = ResolveSurfaceEntity(*world, ctx.params, refusal);
    if (!resolved)
        return refusal;
    const ECS::EntityHandle surfaceEntity = *resolved;

    const auto& surface = *world->GetComponent<Components::NavigationSurface>(surfaceEntity);
    const std::string bakeStatus = Navigation::FormatBakeStatus(surface);
    const Pathfinding::NavMapHandle map = PathfindingECS::ResolveNavigationSurfaceMap(*world, surface);
    if (surface.BakePhase != Components::NavigationBakePhase::Baked || !map.IsValid())
        return Editor::RefuseRequest("The surface on entity " + std::to_string(surfaceEntity.id) +
                                     " is not baked: " + bakeStatus);

    Pathfinding::PathRequest request;
    request.StartX = (*from)[0];
    request.StartY = (*from)[1];
    request.StartZ = (*from)[2];
    request.EndX = (*to)[0];
    request.EndY = (*to)[1];
    request.EndZ = (*to)[2];
    request.AgentRadius = surface.AgentRadius;
    request.AgentHeight = surface.AgentHeight;
    Pathfinding::PathHandle path;
    const Pathfinding::PathStatus status = navigation->FindPath(map, request, path);

    json points = json::array();
    float64 length = 0.0;
    if (path.IsValid())
    {
        auto& buffer = navigation->GetPathBuffer();
        const uint32 count = buffer.GetPointCount(path);
        for (uint32 i = 0; i < count; ++i)
        {
            const Pathfinding::PathPoint point = buffer.GetPoint(path, i);
            if (i > 0)
            {
                const Pathfinding::PathPoint previous = buffer.GetPoint(path, i - 1);
                length += std::sqrt(static_cast<float64>((point.X - previous.X) * (point.X - previous.X) +
                                                         (point.Y - previous.Y) * (point.Y - previous.Y) +
                                                         (point.Z - previous.Z) * (point.Z - previous.Z)));
            }
            points.push_back({point.X, point.Y, point.Z});
        }
        buffer.FreePath(path);
    }
    return json{{"status", PathStatusName(status)},
                {"surfaceEntityId", surfaceEntity.id},
                {"bakeStatus", bakeStatus},
                {"pointCount", points.size()},
                {"lengthMeters", length},
                {"points", std::move(points)}};
}

const char* BakeSourceKindName(Navigation::BakeSourceKind kind)
{
    return kind == Navigation::BakeSourceKind::Mesh ? "Mesh" : "Collider";
}

const char* SourcePolicyName(Components::NavigationSourcePolicy policy)
{
    switch (policy)
    {
    case Components::NavigationSourcePolicy::Auto: return "Auto";
    case Components::NavigationSourcePolicy::Include: return "Include";
    case Components::NavigationSourcePolicy::Exclude: return "Exclude";
    }
    return "Auto";
}

json HandleNavigationSources(const EditorDebugServer::RequestContext& ctx)
{
    auto* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world)
        return Editor::RefuseRequest("No world is running");
    json refusal;
    const std::optional<ECS::EntityHandle> surface = ResolveSurfaceEntity(*world, ctx.params, refusal);
    if (!surface)
        return refusal;

    std::vector<Navigation::BakeSource> sources;
    Navigation::ListBakeSources(*world, *surface, sources);
    json list = json::array();
    for (const Navigation::BakeSource& source : sources)
    {
        json entry{{"entityId", source.Entity.id}, {"kind", BakeSourceKindName(source.Kind)}};
        if (source.Kind == Navigation::BakeSourceKind::Mesh)
            entry["policy"] = SourcePolicyName(source.Policy);
        if (const auto* name = world->GetComponent<Components::Name>(source.Entity))
            entry["name"] = std::string(name->View());
        list.push_back(std::move(entry));
    }
    return json{{"surfaceEntityId", surface->id}, {"count", list.size()}, {"sources", std::move(list)}};
}

} // namespace

void RegisterNavigationDebugHandlers(EditorDebugServer& server)
{
    // get_navigation_stats — NavigationPathfindingSystem's solve budget (NavigationPathfindingStatistics).
    // Answered on the main thread between frames (the debug server's request pump), so it reads the
    // counters of a finished Update. No parameters. Reply:
    //   maxPathSolvesPerFrame: the budget, solves one frame may start.
    //   frames: Updates the system has run.
    //   frame: the newest Update: newRequests (requests that joined the queue), solved, backlog
    //     (requests left queued), oldestWaitFrames / oldestWaitSeconds (the oldest request still
    //     queued; 0 when none is) and longestSolvedWaitFrames / longestSolvedWaitSeconds (the longest
    //     wait among the requests it solved). A request solved in the frame it joined waited 0.
    //   totals: newRequests and solved since the system was created; read twice and subtract for a rate.
    server.RegisterHandler("get_navigation_stats", HandleNavigationStats);

    // find_path — one path on a baked NavigationSurface, solved on the main thread between frames
    // with the surface's agent. Params: from and to as [x, y, z] world points; entityId names the
    // surface (optional when the scene has one). Reply: status (Complete, Partial or Failed),
    // surfaceEntityId, bakeStatus (the inspector's line), pointCount, lengthMeters and points as
    // [x, y, z]. Refused, with the reason, when the surface is missing, ambiguous or not baked.
    server.RegisterHandler("find_path", HandleFindPath);

    // get_navigation_sources — every collider and mesh a bake of a NavigationSurface reads inside its
    // bounds (Navigation::ListBakeSources), gathered on the main thread between frames. Params:
    // entityId names the surface (optional when the scene has one). Reply: surfaceEntityId, count and
    // sources, each with entityId, name (when the entity has one), kind (Collider or Mesh) and, for a
    // mesh, policy (the NavigationSource policy that admitted it: Auto or Include); colliders carry
    // no policy. Refused, with the reason, when the surface is missing or ambiguous.
    server.RegisterHandler("get_navigation_sources", HandleNavigationSources);
}

} // namespace GameEngine
