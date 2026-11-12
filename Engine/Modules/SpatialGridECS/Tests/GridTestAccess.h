#pragma once

#include "SpatialGrid/Grid.h"

#include <algorithm>

namespace GameEngine::SpatialGrid
{

// The private state of a grid that the tests read and write.
struct GridTestAccess
{
    // Writes a new position into the column of mover `id` with no build. Only a query that tests
    // this column entry can find the mover at its new position.
    static void MoveEntry(Grid& grid, uint32 id, const Mathematics::WorldPosition& position)
    {
        const auto at = static_cast<size_t>(std::find(grid.m_Id.begin(), grid.m_Id.end(), id) - grid.m_Id.begin());
        grid.m_X[at] = position.X;
        grid.m_Y[at] = position.Y;
        grid.m_Z[at] = position.Z;
    }

#if GE_DEBUG_INSTRUMENTATION
    static size_t LastQueryHitCapacity() { return Grid::LastQueryHitCapacity(); }
#endif
};

} // namespace GameEngine::SpatialGrid
