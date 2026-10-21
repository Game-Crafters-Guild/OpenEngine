#include "AStarSolverPool.h"

#include "AStarSolver.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/SolverScratchStatistics.h"

#include <gtest/gtest.h>

#include <vector>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

namespace
{

constexpr uint32 kSize = 64;
constexpr uint64 kBytes = uint64{11} * kSize * kSize;

GridSettings Square()
{
    GridSettings settings;
    settings.Width = kSize;
    settings.Depth = kSize;
    settings.CellSize = 1.0f;
    return settings;
}

// Solves corner to corner on `solver`, inside a path-solve window as GridMap::FindPath does.
void SolveCornerToCorner(AStarSolver& solver, const GridMap& grid)
{
    std::vector<uint32> cells;
    grid.OpenPathSolveWindow();
    ASSERT_TRUE(solver.Solve(grid.View(), 0, kSize * kSize - 1, 0, 0.0f, 0, cells));
    grid.ClosePathSolveWindow();
}

} // namespace

// A release frees an idle solver's scratch and never a leased one's: the solve holding the lease keeps
// reading its buffers.
TEST(AStarSolverPoolTests, ReleaseFreesIdleSolversAndNeverALeasedOne)
{
    const GridMap grid(Square());
    AStarSolverPool pool;
    const AStarSolverPool::Lease leased = pool.Acquire();
    SolveCornerToCorner(leased.Solver(), grid);
    {
        const AStarSolverPool::Lease returned = pool.Acquire();
        SolveCornerToCorner(returned.Solver(), grid);
    }

    pool.ReleaseScratchAbove(0);

    EXPECT_EQ(leased.Solver().HeldBytes(), kBytes);
    const SolverScratchStatistics statistics = pool.Statistics();
    EXPECT_EQ(statistics.Solvers, 2u);
    EXPECT_EQ(statistics.HeldBytes, 0u) << "the leased solver is counted at its return";
}
