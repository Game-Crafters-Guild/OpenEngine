#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationScriptApi.h"

#include "Components/Navigation/NavigationSurface.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"

#include "NavigationBakedData.h"
#include "NavigationSourceCollector.h"

#include "AssetCore/Asset.h"
#include "Assets/AssetManager.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Mathematics/Vector3.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/NavSurfaceFile.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <unordered_set>

namespace GameEngine::Navigation
{

namespace
{

using Components::NavigationBakeFailure;
using Components::NavigationBakePhase;
using Components::NavigationSurface;
using Components::NavigationSurfaceMode;

std::string TilePair(int32 x, int32 z)
{
    return "(" + std::to_string(x) + ", " + std::to_string(z) + ")";
}

std::string Units(NavigationSurfaceMode mode)
{
    return mode == NavigationSurfaceMode::Grid ? "cells" : "tiles";
}

std::string FormatFailure(const NavigationSurface& surface)
{
    switch (surface.BakeFailure)
    {
    case NavigationBakeFailure::NoGeometry:
        return "Failed: no geometry inside the bounds; the bake reads the terrain, static collider shapes and "
               "mesh renderers";
    case NavigationBakeFailure::SphericalTerrain:
        return "Failed: spherical terrains are not baked";
    case NavigationBakeFailure::TooLarge:
        if (surface.Mode == NavigationSurfaceMode::Mesh)
            return "Failed: the bounds need " + FormatGroupedInteger(surface.BakeTotal) +
                   " tiles and a mesh surface spans at most " +
                   std::to_string(Pathfinding::kNavChartTilesPerSide) + " x " +
                   std::to_string(Pathfinding::kNavChartTilesPerSide) + " tiles (" +
                   FormatGroupedInteger(static_cast<uint64>(Pathfinding::kNavChartTilesPerSide *
                                                            Pathfinding::kNavMeshTileCells * surface.CellSize)) +
                   " m on a side at this Cell Size): shrink the bounds or raise Cell Size";
        return "Failed: the bounds need " + FormatGroupedInteger(surface.BakeTotal) + " " + Units(surface.Mode) +
               ": raise Cell Size or shrink the bounds";
    case NavigationBakeFailure::TileBuild:
        return "Failed: tile " + TilePair(surface.BlockingTileX, surface.BlockingTileZ) +
               " did not build (the log says why): raise Cell Size or shrink the bounds";
    case NavigationBakeFailure::None:
        break;
    }
    return "Failed";
}

std::string ModeName(NavigationSurfaceMode mode)
{
    return mode == NavigationSurfaceMode::Grid ? "Grid" : "Mesh";
}

std::string FormatBaked(const NavigationSurface& surface)
{
    // The numbers count the bake's tiles or cells, whatever Mode says now.
    const std::string count = FormatGroupedInteger(surface.BakeTotal) + " " + Units(surface.BakedMode);
    if (surface.SettingsChanged)
    {
        const std::string mode = surface.BakedMode != surface.Mode ? ModeName(surface.BakedMode) + ", " : "";
        return "Out of date: the settings changed since the last bake (" + mode + count + "). Bake to apply.";
    }
    std::string text = "Baked: " + count + ", " + FormatGroupedInteger(surface.BakedWalkable) +
                       (surface.BakedMode == NavigationSurfaceMode::Grid ? " walkable, " : " polygons, ") +
                       FormatMebibytes(surface.BakedBytes) + ", version " + std::to_string(surface.BakeVersion);
    if (surface.StaleTiles > 0)
        text += ". Stale: " + FormatGroupedInteger(surface.StaleTiles) + " tiles, " +
                TilePair(surface.BlockingTileX, surface.BlockingTileZ) +
                " among them, whose ground changed since the bake: rebaking";
    else if (surface.TilesDirty + surface.TilesBaking > 0)
        text += ". Rebaking: " + FormatGroupedInteger(surface.TilesDirty + surface.TilesBaking) + " tiles";
    return text;
}

// The model's name in the asset registry (its file name as spelled on disk, without the
// extension), else its GUID.
std::string ModelName(const GUID& model)
{
    auto& engine = EngineCore::GetInstance();
    AssetMetadata metadata{};
    if (engine.IsInitialized() && engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(model, metadata) &&
        !metadata.Name.empty())
        return metadata.Name;
    return model.ToString();
}

} // namespace

bool RequestBake(ECS::World& world, ECS::EntityHandle surface)
{
    auto* component = world.GetComponentForWrite<NavigationSurface>(surface);
    if (!component)
        return false;
    component->BakeRequested = true;
    return true;
}

bool MarkDirty(ECS::World& world, ECS::EntityHandle surface, Mathematics::Vector3 min, Mathematics::Vector3 max)
{
    const auto* component = world.GetComponent<NavigationSurface>(surface);
    if (!component || component->Mode != NavigationSurfaceMode::Mesh)
        return false;
    PathfindingECS::NavigationService::QueueDirtyBox(
        world.GetWorldId(), surface,
        Mathematics::Rect::FromEdges(std::min(min.x, max.x), std::min(min.z, max.z), std::max(min.x, max.x),
                                     std::max(min.z, max.z)));
    return true;
}

std::string FormatAgentStatus(const AgentStatus& status)
{
    std::string reason;
    switch (status.Reason)
    {
    case StatusReason::None: break;
    case StatusReason::NoSurface:
        reason = "no surface: set Surface, or keep one Navigation Surface in the scene";
        break;
    case StatusReason::SurfaceNotBaked:
        reason = status.PendingTiles > 0
                     ? "the surface is baking, " + FormatGroupedInteger(status.PendingTiles) + " tiles left"
                     : "the surface is not baked";
        break;
    case StatusReason::DestinationOffSurface: reason = "the destination is off the surface"; break;
    case StatusReason::StartOffSurface: reason = "the agent stands off the surface"; break;
    case StatusReason::NoRoute: reason = "no walkable route to the destination"; break;
    case StatusReason::DynamicBody:
        reason = "a dynamic body fights the movement: make the body kinematic or remove it";
        break;
    }

    switch (status.State)
    {
    case AgentState::Idle: return "Idle: no order";
    case AgentState::Waiting: return reason.empty() ? "Waiting for its path" : "Waiting: " + reason;
    case AgentState::Walking:
        return status.PartialPath ? "Walking to the reachable point nearest the destination" : "Walking";
    case AgentState::Arrived:
        return status.PartialPath ? "Arrived at the reachable point nearest the destination" : "Arrived";
    case AgentState::Failed: return reason.empty() ? "Failed" : "Failed: " + reason;
    }
    return {};
}

std::string FormatBakeStatus(const NavigationSurface& surface)
{
    switch (surface.BakePhase)
    {
    case NavigationBakePhase::NotBaked:
        return "Not baked";
    case NavigationBakePhase::Waiting:
        if (surface.BakeWait == Components::NavigationBakeWait::TerrainData)
            return "Waiting for: the terrain's heightfield to be built";
        if (surface.BakeWait == Components::NavigationBakeWait::BakedData)
            return "Waiting for: the baked data (.navsurface) to load";
        if (surface.BakeWait == Components::NavigationBakeWait::Model)
            return "Waiting for: model " + ModelName(surface.BlockingModel.ToGuid()) + " is still loading";
        return "Waiting for: terrain tile " + TilePair(surface.BlockingTileX, surface.BlockingTileZ) +
               " is not resident";
    case NavigationBakePhase::Baking:
        return "Baking: " + FormatGroupedInteger(surface.BakeDone) + " of " + FormatGroupedInteger(surface.BakeTotal) +
               " " + Units(surface.Mode);
    case NavigationBakePhase::Baked:
        return FormatBaked(surface);
    case NavigationBakePhase::Failed:
        return FormatFailure(surface);
    }
    return {};
}

bool SurfaceBounds(ECS::World& world, ECS::EntityHandle surface, Mathematics::Vector3& outMin,
                   Mathematics::Vector3& outMax)
{
    const auto* component = world.GetComponent<NavigationSurface>(surface);
    if (!component)
        return false;
    const auto* transform = world.GetComponent<Components::WorldTransform>(surface);
    const PathfindingECS::NavigationTerrainSource terrain =
        component->FitBounds ? PathfindingECS::ResolveNavigationTerrainSource(world) : PathfindingECS::NavigationTerrainSource{};
    // Only a fitted surface with no planar terrain reads the geometry's bounds.
    const PathfindingECS::NavigationStaticGeometry geometry = component->FitBounds && !terrain.Query.IsValid()
                                                                  ? PathfindingECS::GatherNavigationStaticGeometry(world)
                                                                  : PathfindingECS::NavigationStaticGeometry{};
    const PathfindingECS::NavigationBounds bounds =
        PathfindingECS::NavigationSurfaceBounds(*component, transform, terrain, geometry);
    if (bounds.Empty)
        return false;
    outMin = Mathematics::Vector3(bounds.Min[0], bounds.Min[1], bounds.Min[2]);
    outMax = Mathematics::Vector3(bounds.Max[0], bounds.Max[1], bounds.Max[2]);
    return true;
}

bool ListBakeSources(ECS::World& world, ECS::EntityHandle surface, std::vector<BakeSource>& out)
{
    out.clear();
    const auto* component = world.GetComponent<NavigationSurface>(surface);
    if (!component)
        return false;
    const auto* transform = world.GetComponent<Components::WorldTransform>(surface);
    const PathfindingECS::NavigationTerrainSource terrain = PathfindingECS::ResolveNavigationTerrainSource(world);
    const PathfindingECS::NavigationStaticGeometry geometry = PathfindingECS::GatherNavigationStaticGeometry(world);
    const PathfindingECS::NavigationBounds bounds =
        PathfindingECS::NavigationSurfaceBounds(*component, transform, terrain, geometry);
    // One entry per entity and kind: an entity with several collider shapes is one collider source.
    std::unordered_set<uint64> listed;
    for (const auto& item : geometry.Items)
    {
        if (!item.Bounds.OverlapsXZ(bounds.Min[0], bounds.Min[2], bounds.Max[0], bounds.Max[2]))
            continue;
        const uint64 key = (static_cast<uint64>(item.Source.Entity.id) << 8) | static_cast<uint64>(item.Source.Kind);
        if (listed.insert(key).second)
            out.push_back(item.Source);
    }
    return true;
}

uint64 BakeUnitCount(const NavigationSurface& surface, const Mathematics::Vector3& min, const Mathematics::Vector3& max)
{
    const float32 low[3] = {min.x, min.y, min.z};
    const float32 high[3] = {max.x, max.y, max.z};
    PathfindingECS::NavigationBounds bounds;
    bounds.Include(low);
    bounds.Include(high);
    return PathfindingECS::NavigationBakeExtentOf(surface, bounds).Count();
}

bool WriteBakedSurface(ECS::World& world, ECS::EntityHandle surface, std::vector<uint8>& out)
{
    out.clear();
    const auto* component = world.GetComponent<NavigationSurface>(surface);
    if (!component || component->BakePhase != NavigationBakePhase::Baked ||
        component->BakedMode != NavigationSurfaceMode::Mesh)
        return false;
    const auto* mesh = PathfindingECS::ResolveNavigationSurfaceMesh(world, *component);
    if (!mesh || !mesh->IsBuilt())
        return false;
    return Pathfinding::WriteNavSurface(PathfindingECS::MakeNavigationBakedData(*component, *mesh), out);
}

} // namespace GameEngine::Navigation
