#pragma once

#include "Pathfinding/PathfindingTypes.h"
#include "Types/Types.h"

namespace GameEngine::Components
{
struct NavigationSurface;
} // namespace GameEngine::Components

namespace GameEngine::Pathfinding
{
class DetourNavMap;
struct NavSurfaceData;
} // namespace GameEngine::Pathfinding

namespace GameEngine::PathfindingECS
{

// A mesh surface's baked data (.navsurface) as the surface makes and checks it. One profile,
// Default (id 0), with the surface's own agent, and one chart.

// The surface's tiles in `mesh`, with their content hashes, as the data a .navsurface file holds.
Pathfinding::NavSurfaceData MakeNavigationBakedData(const Components::NavigationSurface& surface,
                                                    const Pathfinding::DetourNavMap& mesh);

// True when `data` was baked for the surface's cell size and agent in `chart`: the tiles it holds
// are the tiles a bake of the surface makes now, unless their content changed.
bool NavigationBakedDataMatches(const Pathfinding::NavSurfaceData& data, const Components::NavigationSurface& surface,
                                const Pathfinding::NavMeshChart& chart);

} // namespace GameEngine::PathfindingECS
