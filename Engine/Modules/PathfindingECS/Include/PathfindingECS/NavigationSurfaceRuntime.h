#pragma once

#include "Pathfinding/PathfindingTypes.h"

namespace GameEngine::ECS { class World; }
namespace GameEngine::Components { struct NavigationSurface; }
namespace GameEngine::Pathfinding { class DetourNavMap; class GridMap; }

namespace GameEngine::PathfindingECS
{
// The NavigationWorld map a NavigationSurface names is a runtime resource owned by the component
// that created it, stamped with the creating world. Every reader and publisher goes through these
// functions; nothing else writes the binding. Main thread only, and never while path solves run
// (HasActivePathJobs): NavigationWorld maps are not synchronized.
//
// The map a surface owns keeps its handle across re-bakes (the grid is reset in place, the mesh
// re-initialized in place), so an agent bound to the handle stays bound and an undo that restores
// the component's bytes restores the same binding.

// The handle of the live map `surface` owns in `world`, or an invalid handle when the binding is
// absent, stamped for another world, or names a map the service no longer holds. Says nothing about
// the bake: callers that need a baked map read BakePhase.
Pathfinding::NavMapHandle ResolveNavigationSurfaceMap(const ECS::World& world,
    const Components::NavigationSurface& surface);
// The same map as a grid or a mesh; null when it is the other kind or does not resolve.
Pathfinding::GridMap* ResolveNavigationSurfaceGrid(const ECS::World& world,
    const Components::NavigationSurface& surface);
Pathfinding::DetourNavMap* ResolveNavigationSurfaceMesh(const ECS::World& world,
    const Components::NavigationSurface& surface);

// Creates an empty map of the surface's Mode in the current service and binds it to `world`.
// Returns an invalid handle when the service is absent, path solves are running, or `surface` already
// resolves to a live map in `world`. A stale binding is overwritten without releasing whatever it
// named.
Pathfinding::NavMapHandle CreateNavigationSurfaceMap(const ECS::World& world,
    Components::NavigationSurface& surface);

// Releases the map when `surface` resolves in `world`, otherwise only clears the binding: a copy
// of a bound component in another world names a map it does not own and never releases it. Also
// the component teardown hook (RegisterNavigationWorldHooks). Safe to repeat.
void ResetNavigationSurfaceMap(const ECS::World& world, Components::NavigationSurface& surface);

// Releases the map `handle` names, shutting the crowd down first when it steers on that map. For
// the owner of a binding (NavigationSurfaceSystem) dropping a map no component names any more.
void ReleaseNavigationMap(Pathfinding::NavMapHandle handle);

// Clears the binding without touching the map it names. For a copy of a bound component made
// inside the same world (entity duplication), which would otherwise share, and on destruction
// release, the original's map.
void ForgetNavigationSurfaceMap(Components::NavigationSurface& surface);
} // namespace GameEngine::PathfindingECS
