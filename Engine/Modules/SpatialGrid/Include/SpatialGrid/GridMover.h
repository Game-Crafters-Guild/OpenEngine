#pragma once

#include "Mathematics/WorldPosition.h"
#include "Types/Types.h"

namespace GameEngine::SpatialGrid
{

/// One mover that Grid::Build indexes: a sphere with a mask and an id.
struct GridMover
{
    /// The center, in world meters.
    Mathematics::WorldPosition Position;
    /// Meters. 0 is a point.
    float32 Radius = 0.0f;
    /// A query finds the mover when this mask and the query's mask share a bit.
    uint32 Mask = 1u;
    /// The caller's identity of the mover. List queries return hits in ascending id.
    uint32 Id = 0u;
};

} // namespace GameEngine::SpatialGrid
