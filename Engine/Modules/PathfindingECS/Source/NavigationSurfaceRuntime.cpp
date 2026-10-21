#include "PathfindingECS/NavigationSurfaceRuntime.h"
#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/NavigationService.h"
#include "ECS/Entity.h"
#include "Pathfinding/NavigationWorld.h"

namespace GameEngine::PathfindingECS
{
namespace
{
// The smallest valid grid: a placeholder until the first bake resets it to the surface's bounds.
Pathfinding::GridSettings PlaceholderGridSettings()
{
    Pathfinding::GridSettings settings;
    settings.Width = 1;
    settings.Depth = 1;
    return settings;
}
} // namespace

Pathfinding::NavMapHandle ResolveNavigationSurfaceMap(const ECS::World& world,
    const Components::NavigationSurface& surface)
{
    if (surface.OwnerWorldId == 0 || surface.OwnerWorldId != world.GetWorldId())
        return {};
    auto* navigation = NavigationService::TryGet();
    const Pathfinding::NavMapHandle handle{surface.NavMapIndex, surface.NavMapGeneration};
    return navigation && navigation->GetMap(handle) ? handle : Pathfinding::NavMapHandle{};
}

Pathfinding::GridMap* ResolveNavigationSurfaceGrid(const ECS::World& world,
    const Components::NavigationSurface& surface)
{
    const auto handle = ResolveNavigationSurfaceMap(world, surface);
    return handle.IsValid() ? NavigationService::Get().GetGridMap(handle) : nullptr;
}

Pathfinding::DetourNavMap* ResolveNavigationSurfaceMesh(const ECS::World& world,
    const Components::NavigationSurface& surface)
{
    const auto handle = ResolveNavigationSurfaceMap(world, surface);
    return handle.IsValid() ? NavigationService::Get().GetDetourNavMap(handle) : nullptr;
}

Pathfinding::NavMapHandle CreateNavigationSurfaceMap(const ECS::World& world,
    Components::NavigationSurface& surface)
{
    auto* navigation = NavigationService::TryGet();
    if (!navigation || navigation->HasActivePathJobs() || ResolveNavigationSurfaceMap(world, surface).IsValid())
        return {};
    const auto handle = surface.Mode == Components::NavigationSurfaceMode::Grid
                            ? navigation->AddGridMap(PlaceholderGridSettings())
                            : navigation->AddDetourNavMap();
    if (!handle.IsValid())
        return {};
    surface.NavMapIndex = handle.Index;
    surface.NavMapGeneration = handle.Generation;
    surface.OwnerWorldId = world.GetWorldId();
    return handle;
}

void ForgetNavigationSurfaceMap(Components::NavigationSurface& surface)
{
    surface.NavMapIndex = 0;
    surface.NavMapGeneration = 0;
    surface.OwnerWorldId = 0;
}

void ReleaseNavigationMap(Pathfinding::NavMapHandle handle)
{
    auto* navigation = NavigationService::TryGet();
    if (!navigation)
        return;
    // The crowd steers on the mesh's Detour data; it must not outlive it.
    auto& crowd = navigation->GetCrowdManager();
    if (const auto* mesh = navigation->GetDetourNavMap(handle); mesh && crowd.IsBoundTo(*mesh))
        crowd.Shutdown();
    navigation->RemoveMap(handle);
}

void ResetNavigationSurfaceMap(const ECS::World& world, Components::NavigationSurface& surface)
{
    if (const auto handle = ResolveNavigationSurfaceMap(world, surface); handle.IsValid())
        ReleaseNavigationMap(handle);
    ForgetNavigationSurfaceMap(surface);
}
} // namespace GameEngine::PathfindingECS
