#pragma once

#include "Pathfinding/PathfindingTypes.h"

namespace GameEngine::Pathfinding
{

class PathBuffer;

class INavigationMap
{
public:
    virtual ~INavigationMap() = default;

    virtual PathStatus FindPath(const PathRequest& request, PathBuffer& pathBuffer, PathHandle& outPath) const = 0;

    // Whether the map serves the point. What `radius` means depends on the map: on a navigation
    // mesh it is a search tolerance (a point up to `radius` from the mesh is navigable, so a larger
    // radius accepts more); on a grid it is a body's clearance (a disc of `radius` around the point
    // must keep clear of every blocked cell and the grid's edge, so a larger radius accepts less).
    // A radius of 0 asks only whether the point lies on the map's walkable area on either; on a
    // mesh it also bounds the height, so the point must sit at the mesh's own height.
    virtual bool IsPointNavigable(float32 x, float32 y, float32 z, float32 radius) const = 0;

    virtual bool GetClosestNavigablePoint(float32 x, float32 y, float32 z,
                                           float32 searchRadius,
                                           float32& outX, float32& outY, float32& outZ) const = 0;

    virtual bool Raycast(float32 startX, float32 startY, float32 startZ,
                         float32 endX, float32 endY, float32 endZ,
                         float32& hitX, float32& hitY, float32& hitZ) const = 0;
};

} // namespace GameEngine::Pathfinding
