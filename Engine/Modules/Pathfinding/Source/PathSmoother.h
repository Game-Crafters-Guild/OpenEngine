#pragma once

#include "Pathfinding/GridMap.h"
#include "Pathfinding/PathfindingTypes.h"

#include <vector>

namespace GameEngine::Pathfinding
{

// Simplifies a grid path by removing intermediate waypoints where an agent of
// the path's radius passes straight between non-adjacent points: the view's
// IsSegmentClear keeps every shortcut at least the radius from blocked cells.
class PathSmoother
{
public:
    // Smooths the path in-place. Returns the new point count.
    // The input points are world-space coordinates from GridMap::FindPath.
    static uint32 Smooth(const GridMap::ReadView& grid, std::vector<PathPoint>& points, float32 agentRadius);
};

} // namespace GameEngine::Pathfinding
