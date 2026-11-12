#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

#include <limits>

namespace GameEngine::SpatialGridECS
{

/// Which movers a SpatialGridView query can return.
struct SpatialGridFilter
{
    /// A mover passes when its SpatialGridMover::Mask and this mask share a bit.
    uint32 Mask = std::numeric_limits<uint32>::max();
    /// An entity that the query never returns, usually the caller itself.
    ECS::EntityHandle Ignore;
};

} // namespace GameEngine::SpatialGridECS
