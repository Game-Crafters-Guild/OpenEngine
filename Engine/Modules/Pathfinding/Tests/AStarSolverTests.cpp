#include "AStarSolver.h"
#include "Pathfinding/GridMap.h"

#include <gtest/gtest.h>

#include <random>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

namespace
{

GridSettings MakeAStarSettings(uint32 width, uint32 depth, GridType type = GridType::Square)
{
    GridSettings settings;
    settings.Type = type;
    settings.CellSize = 1.0f;
    settings.OriginX = 0.0f;
    settings.OriginY = 0.0f;
    settings.OriginZ = 0.0f;
    settings.Width = width;
    settings.Depth = depth;
    settings.MaxSlope = 45.0f;
    settings.MaxStepHeight = 0.4f;
    return settings;
}

// Solves inside a path-solve window on the grid, as GridMap::FindPath does.
bool SolveInWindow(AStarSolver& solver, const GridMap& grid, uint32 startIndex, uint32 endIndex,
                   uint32 excludeAgentId, float32 currentTime, uint32 agentCellRadius,
                   std::vector<uint32>& outPathCellIndices)
{
    grid.OpenPathSolveWindow();
    const bool found = solver.Solve(grid.View(), startIndex, endIndex, excludeAgentId, currentTime,
                                    agentCellRadius, outPathCellIndices);
    grid.ClosePathSolveWindow();
    return found;
}

} // namespace

class AStarSolverTest : public ::testing::Test
{
protected:
    AStarSolver m_Solver;
};

TEST_F(AStarSolverTest, SimpleStraightLinePath)
{
    GridMap grid(MakeAStarSettings(10, 1));

    std::vector<uint32> path;
    bool found = SolveInWindow(m_Solver, grid, 0, 9, 0, 0.0f, 0, path);

    ASSERT_TRUE(found);
    EXPECT_EQ(path.front(), 0u);
    EXPECT_EQ(path.back(), 9u);

    for (uint32 i = 0; i < path.size(); ++i)
    {
        EXPECT_EQ(path[i], i);
    }
}

// A grid fitted to a large terrain is hundreds of thousands of cells; a path that detours around a
// long wall expands most of them. The solve finds it however many cells that takes.
TEST_F(AStarSolverTest, LongDetourOnALargeGridIsFound)
{
    constexpr uint32 kSide = 512;
    GridMap grid(MakeAStarSettings(kSide, kSide));
    // A wall across the grid with its only gap at the far end.
    for (uint32 x = 0; x + 1 < kSide; ++x)
        grid.SetCellBlocked(x, kSide / 2, true);

    std::vector<uint32> path;
    const uint32 start = grid.CellIndex(0, kSide / 2 - 4);
    const uint32 end = grid.CellIndex(0, kSide / 2 + 4);
    ASSERT_TRUE(SolveInWindow(m_Solver, grid, start, end, 0, 0.0f, 0, path));
    EXPECT_EQ(path.front(), start);
    EXPECT_EQ(path.back(), end);
    EXPECT_GT(path.size(), 2u * (kSide - 2)) << "the path goes around the wall's far end";
}

TEST_F(AStarSolverTest, PathAroundWall)
{
    GridMap grid(MakeAStarSettings(10, 10));
    for (uint32 z = 1; z < 10; ++z)
    {
        grid.SetCellBlocked(5, z, true);
    }

    uint32 startIndex = grid.CellIndex(0, 5);
    uint32 endIndex = grid.CellIndex(9, 5);

    std::vector<uint32> path;
    bool found = SolveInWindow(m_Solver, grid, startIndex, endIndex, 0, 0.0f, 0, path);

    ASSERT_TRUE(found);
    EXPECT_EQ(path.front(), startIndex);
    EXPECT_EQ(path.back(), endIndex);

    for (uint32 cellIndex : path)
    {
        uint32 cx = 0, cz = 0;
        grid.CellFromIndex(cellIndex, cx, cz);
        EXPECT_FALSE(grid.IsCellBlocked(cx, cz));
    }
}

TEST_F(AStarSolverTest, DiagonalMovementPreferredWhenShorter)
{
    GridMap grid(MakeAStarSettings(10, 10));

    uint32 startIndex = grid.CellIndex(0, 0);
    uint32 endIndex = grid.CellIndex(5, 5);

    std::vector<uint32> path;
    bool found = SolveInWindow(m_Solver, grid, startIndex, endIndex, 0, 0.0f, 0, path);

    ASSERT_TRUE(found);

    // Optimal diagonal path: 6 cells (start + 5 diagonal moves)
    EXPECT_LE(path.size(), 7u);
}

TEST_F(AStarSolverTest, HexGridPathOnOpenGrid)
{
    GridMap grid(MakeAStarSettings(10, 10, GridType::Hex));

    uint32 startIndex = grid.CellIndex(0, 0);
    uint32 endIndex = grid.CellIndex(5, 0);

    std::vector<uint32> path;
    bool found = SolveInWindow(m_Solver, grid, startIndex, endIndex, 0, 0.0f, 0, path);

    ASSERT_TRUE(found);
    EXPECT_EQ(path.front(), startIndex);
    EXPECT_EQ(path.back(), endIndex);
    EXPECT_EQ(path.size(), 6u);
}

TEST_F(AStarSolverTest, NoPathWhenFullyBlocked)
{
    GridMap grid(MakeAStarSettings(5, 5));
    grid.SetCellBlocked(1, 0, true);
    grid.SetCellBlocked(0, 1, true);
    grid.SetCellBlocked(1, 1, true);

    uint32 startIndex = grid.CellIndex(0, 0);
    uint32 endIndex = grid.CellIndex(4, 4);

    std::vector<uint32> path;
    bool found = SolveInWindow(m_Solver, grid, startIndex, endIndex, 0, 0.0f, 0, path);

    EXPECT_FALSE(found);
}

TEST_F(AStarSolverTest, StartEqualsEndReturnsSinglePoint)
{
    GridMap grid(MakeAStarSettings(5, 5));

    uint32 cellIndex = grid.CellIndex(2, 2);

    std::vector<uint32> path;
    bool found = SolveInWindow(m_Solver, grid, cellIndex, cellIndex, 0, 0.0f, 0, path);

    ASSERT_TRUE(found);
    ASSERT_EQ(path.size(), 1u);
    EXPECT_EQ(path[0], cellIndex);
}

TEST_F(AStarSolverTest, CostAwarePrefersLowCostRoute)
{
    // 5x3 grid. Row 2 has high cost. Path from (0,1) to (4,1) should go through row 0.
    GridMap grid(MakeAStarSettings(5, 3));

    for (uint32 x = 0; x < 5; ++x)
    {
        grid.SetCellCost(x, 2, 10.0f);
    }

    uint32 startIndex = grid.CellIndex(0, 1);
    uint32 endIndex = grid.CellIndex(4, 1);

    std::vector<uint32> path;
    bool found = SolveInWindow(m_Solver, grid, startIndex, endIndex, 0, 0.0f, 0, path);

    ASSERT_TRUE(found);

    bool usedRow2 = false;
    for (uint32 cellIndex : path)
    {
        uint32 cx = 0, cz = 0;
        grid.CellFromIndex(cellIndex, cx, cz);
        if (cz == 2)
        {
            usedRow2 = true;
            break;
        }
    }

    EXPECT_FALSE(usedRow2);
}

TEST_F(AStarSolverTest, ReservationPenaltyAvoidsReservedCells)
{
    GridMap grid(MakeAStarSettings(3, 3));

    grid.ReserveCell(1, 1, 99, 100.0f);

    uint32 startIndex = grid.CellIndex(0, 0);
    uint32 endIndex = grid.CellIndex(2, 2);

    std::vector<uint32> pathWithReservation;
    bool found1 = SolveInWindow(m_Solver, grid, startIndex, endIndex, 0, 0.0f, 0, pathWithReservation);

    std::vector<uint32> pathWithoutPenalty;
    bool found2 = SolveInWindow(m_Solver, grid, startIndex, endIndex, 99, 0.0f, 0, pathWithoutPenalty);

    ASSERT_TRUE(found1);
    ASSERT_TRUE(found2);

    uint32 centerIndex = grid.CellIndex(1, 1);

    bool reservedPathUsesCenter = false;
    for (uint32 idx : pathWithReservation)
    {
        if (idx == centerIndex)
        {
            reservedPathUsesCenter = true;
            break;
        }
    }

    bool excludedPathUsesCenter = false;
    for (uint32 idx : pathWithoutPenalty)
    {
        if (idx == centerIndex)
        {
            excludedPathUsesCenter = true;
            break;
        }
    }

    EXPECT_TRUE(excludedPathUsesCenter);
    EXPECT_FALSE(reservedPathUsesCenter);
}

TEST_F(AStarSolverTest, OccupancyBlocksOtherAgentsButAllowsDest)
{
    GridMap grid(MakeAStarSettings(3, 3));
    grid.ReserveCell(1, 1, 99, 100.0f, true);

    uint32 startIndex = grid.CellIndex(0, 0);
    uint32 endIndex = grid.CellIndex(2, 2);
    uint32 centerIndex = grid.CellIndex(1, 1);

    std::vector<uint32> path;
    ASSERT_TRUE(SolveInWindow(m_Solver, grid, startIndex, endIndex, 0, 0.0f, 0, path));
    for (uint32 idx : path)
        EXPECT_NE(idx, centerIndex);

    std::vector<uint32> intoOccupied;
    ASSERT_TRUE(SolveInWindow(m_Solver, grid, startIndex, centerIndex, 0, 0.0f, 0, intoOccupied));
    EXPECT_EQ(intoOccupied.back(), centerIndex);
}

TEST_F(AStarSolverTest, OccupancyOnOnlyCorridorIsTax)
{
    GridMap grid(MakeAStarSettings(3, 1));
    grid.ReserveCell(1, 0, 99, 100.0f, true);

    std::vector<uint32> path;
    ASSERT_TRUE(SolveInWindow(m_Solver, grid, grid.CellIndex(0, 0), grid.CellIndex(2, 0), 0, 0.0f, 0, path))
        << "Occupancy must not Failed a walkable dest when it is the only corridor cell";
    ASSERT_EQ(path.size(), 3u);
    EXPECT_EQ(path[1], grid.CellIndex(1, 0));
}

TEST_F(AStarSolverTest, StepHeightBlocksPath)
{
    // 3x1 grid with a tall step in the middle cell
    GridSettings settings;
    settings.Width = 3;
    settings.Depth = 1;
    settings.CellSize = 1.0f;
    settings.MaxStepHeight = 0.3f;
    settings.MaxSlope = 89.0f; // effectively no slope limit
    GridMap grid(settings);

    // Middle cell is too high to step onto
    grid.SetCellHeight(1, 0, 1.0f);

    AStarSolver solver;
    std::vector<uint32> path;
    bool found = SolveInWindow(solver, grid, 0, 2, 0, 0.0f, 0, path);
    EXPECT_FALSE(found) << "Path should be blocked by step height";
}

TEST_F(AStarSolverTest, SlopeBlocksPath)
{
    // 3x1 grid with a steep slope in the middle cell
    GridSettings settings;
    settings.Width = 3;
    settings.Depth = 1;
    settings.CellSize = 1.0f;
    settings.MaxStepHeight = 100.0f; // effectively no step limit
    settings.MaxSlope = 30.0f;
    GridMap grid(settings);

    // Middle cell height creates a slope > 30 degrees (tan(30) ~= 0.577, so height > 0.577 * cellSize)
    grid.SetCellHeight(1, 0, 1.0f); // slope = atan2(1.0, 1.0) = 45 degrees > 30

    AStarSolver solver;
    std::vector<uint32> path;
    bool found = SolveInWindow(solver, grid, 0, 2, 0, 0.0f, 0, path);
    EXPECT_FALSE(found) << "Path should be blocked by slope limit";
}

// One solver reused across many solves, on grids whose cells change between solves and whose
// sizes alternate (the buffers shrink and grow), returns what a fresh solver returns every time.
TEST_F(AStarSolverTest, ReusedSolverMatchesAFreshSolver)
{
    GridMap smallGrid(MakeAStarSettings(12, 12));
    GridMap largeGrid(MakeAStarSettings(32, 32));
    std::mt19937 random(7);

    uint32 foundCount = 0;
    for (uint32 query = 0; query < 400; ++query)
    {
        GridMap& grid = (query % 3 == 0) ? smallGrid : largeGrid;
        const uint32 side = grid.GetSettings().Width;
        grid.SetCellBlocked(static_cast<uint32>(random() % side), static_cast<uint32>(random() % side),
                            random() % 4 == 0);

        const uint32 cellCount = side * side;
        const uint32 start = static_cast<uint32>(random() % cellCount);
        const uint32 end = static_cast<uint32>(random() % cellCount);
        const uint32 radius = (query % 4 == 0) ? 1u : 0u;

        AStarSolver freshSolver;
        std::vector<uint32> freshPath;
        const bool freshFound = SolveInWindow(freshSolver, grid, start, end, 0, 0.0f, radius, freshPath);
        std::vector<uint32> reusedPath;
        const bool reusedFound = SolveInWindow(m_Solver, grid, start, end, 0, 0.0f, radius, reusedPath);

        ASSERT_EQ(reusedFound, freshFound) << "query " << query;
        ASSERT_EQ(reusedPath, freshPath) << "query " << query;
        foundCount += freshFound ? 1u : 0u;
    }
    EXPECT_GT(foundCount, 100u) << "most queries find a path, so the comparison covers searches";
}

// The visit stamps wrap after 65,535 solves. A cell last visited 65,535 solves ago, and a cell
// never visited, both read as unvisited in the solve after the wrap.
TEST_F(AStarSolverTest, VisitStampWrapReadsEveryCellAsUnvisited)
{
    constexpr uint32 kSolvesPerStampWrap = 65536;
    GridMap grid(MakeAStarSettings(16, 16));
    // A two-cell pocket in the corner: a solve inside it visits only (0,0) and (1,0).
    grid.SetCellBlocked(2, 0, true);
    grid.SetCellBlocked(0, 1, true);
    grid.SetCellBlocked(1, 1, true);
    grid.SetCellBlocked(2, 1, true);

    const uint32 start = grid.CellIndex(8, 8);
    std::vector<uint32> path;
    ASSERT_TRUE(SolveInWindow(m_Solver, grid, start, grid.CellIndex(9, 8), 0, 0.0f, 0, path));
    for (uint32 solve = 2; solve < kSolvesPerStampWrap; ++solve)
        ASSERT_TRUE(SolveInWindow(m_Solver, grid, grid.CellIndex(0, 0), grid.CellIndex(1, 0), 0, 0.0f, 0, path));

    const uint32 farEnd = grid.CellIndex(15, 15);
    AStarSolver freshSolver;
    std::vector<uint32> freshPath;
    ASSERT_TRUE(SolveInWindow(freshSolver, grid, start, farEnd, 0, 0.0f, 0, freshPath));
    ASSERT_TRUE(SolveInWindow(m_Solver, grid, start, farEnd, 0, 0.0f, 0, path));
    EXPECT_EQ(path, freshPath);
}
