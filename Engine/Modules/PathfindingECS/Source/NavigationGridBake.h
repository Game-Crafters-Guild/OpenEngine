#pragma once

#include "Pathfinding/PathfindingTypes.h"
#include "Types/Types.h"

#include <memory>

namespace GameEngine::Pathfinding
{
class GridMap;
} // namespace GameEngine::Pathfinding

namespace GameEngine::PathfindingECS
{

struct NavigationStaticGeometry;
struct NavigationTerrainSource;

// Bakes a grid for `settings` from the terrain and the static geometry. A cell's ground is the
// highest of the terrain and the up-facing static triangles under its centre; a cell with no ground
// is blocked, and so is one whose ground differs from a neighbour's by more than MaxSlope allows
// over the distance between them, and one in which a static face steeper than MaxSlope, anywhere
// in the cell, rises above the climb between neighbouring cells (WalkableClimb of `stepHeight` and
// MaxSlope over CellSize) and starts below `agentHeight`, both above the cell's ground.
// Null when a cell lies on a terrain tile that is not resident: `outTerrainTileX` and
// `outTerrainTileZ` name it.
std::unique_ptr<Pathfinding::GridMap> BakeNavigationGrid(const Pathfinding::GridSettings& settings,
                                                         const NavigationTerrainSource& terrain,
                                                         const NavigationStaticGeometry& geometry,
                                                         float32 stepHeight, float32 agentHeight,
                                                         int32& outTerrainTileX, int32& outTerrainTileZ);

} // namespace GameEngine::PathfindingECS
