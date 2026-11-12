#pragma once

#include "Types/Types.h"

namespace GameEngine::SpatialGrid
{

/// One mover that a query found.
struct GridHit
{
    /// The GridMover::Id of the mover.
    uint32 Id = 0u;
    /// Square meters: the squared distance from the query's origin to the mover's center. The origin is the
    /// center of a sphere, cylinder or box, the apex of a cone, and the start of a segment, where it is the
    /// squared distance along the segment to the point nearest the mover.
    float64 DistanceSquared = 0.0;
};

} // namespace GameEngine::SpatialGrid
