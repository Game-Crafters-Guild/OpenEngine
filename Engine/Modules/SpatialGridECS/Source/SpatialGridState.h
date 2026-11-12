#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::SpatialGrid
{
class Grid;
}

namespace GameEngine::Components
{

// The world's grid, a singleton that SpatialGridBuildSystem sets and SpatialGridView reads.
struct SpatialGridState
{
    const SpatialGrid::Grid* Grid = nullptr;
    // The movers that the last build did not index: a radius that is NaN, infinite or negative.
    uint32 RefusedMovers = 0u;
};

static_assert(std::is_trivially_copyable_v<SpatialGridState>);

} // namespace GameEngine::Components
