#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

namespace GameEngine::SpatialGridECS
{

/// One mover that a SpatialGridView query found.
struct SpatialGridHit
{
    ECS::EntityHandle Entity;
    /// Square meters from the query's origin to the mover's center (along the segment for Segment).
    float64 DistanceSquared = 0.0;
};

} // namespace GameEngine::SpatialGridECS
