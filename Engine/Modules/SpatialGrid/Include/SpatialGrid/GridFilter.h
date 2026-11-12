#pragma once

#include "Types/Types.h"

#include <limits>

namespace GameEngine::SpatialGrid
{

/// Which movers a query can return.
struct GridFilter
{
    /// The id that no mover is ignored by.
    static constexpr uint32 kNoId = std::numeric_limits<uint32>::max();

    /// A mover passes when its mask and this mask share a bit.
    uint32 Mask = std::numeric_limits<uint32>::max();
    /// The id of a mover that the query never returns, usually the caller itself.
    uint32 IgnoreId = kNoId;
};

} // namespace GameEngine::SpatialGrid
