#include "AStarSolver.h"
#include "HexGridHelpers.h"
#include "Mathematics/Angle.h"
#include "Pathfinding/GridMap.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
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
    uint32 expansions = 0;
    const GridSolveEnd end = solver.Solve(grid.View(), startIndex, endIndex, excludeAgentId, currentTime,
                                          agentCellRadius, outPathCellIndices, expansions);
    grid.ClosePathSolveWindow();
    return end == GridSolveEnd::Reached;
}

// One solve inside a path-solve window; returns how it ended and the cells it expanded.
GridSolveEnd SolveCountingExpansions(AStarSolver& solver, const GridMap& grid, uint32 startIndex, uint32 endIndex,
                                     uint32 agentCellRadius, uint32& outExpansions)
{
    std::vector<uint32> path;
    grid.OpenPathSolveWindow();
    const GridSolveEnd end = solver.Solve(grid.View(), startIndex, endIndex, 0, 0.0f, agentCellRadius, path,
                                          outExpansions);
    grid.ClosePathSolveWindow();
    return end;
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

// A wall written across a grid after a solve on it (the regions were labelled) separates the two
// sides: the next solve across fails before the search, so a fresh solver never sizes its scratch,
// which a solve does before it expands the first cell.
TEST_F(AStarSolverTest, AWallWrittenAfterASolveSeparatesTheRegions)
{
    GridMap grid(MakeAStarSettings(32, 32));
    const uint32 west = grid.CellIndex(2, 16);
    const uint32 east = grid.CellIndex(29, 16);
    std::vector<uint32> path;
    ASSERT_TRUE(SolveInWindow(m_Solver, grid, west, east, 0, 0.0f, 0, path));

    for (uint32 z = 0; z < 32; ++z)
        grid.SetCellBlocked(16, z, true);
    AStarSolver freshSolver;
    EXPECT_FALSE(SolveInWindow(freshSolver, grid, west, east, 0, 0.0f, 0, path));
    EXPECT_EQ(freshSolver.HeldBytes(), 0u) << "the solve across the wall searched";
}

// A ridge raised across a grid after a solve, higher than an agent steps, separates the two sides
// as a wall does: heights are part of the regions.
TEST_F(AStarSolverTest, ARidgeRaisedAfterASolveSeparatesTheRegions)
{
    GridMap grid(MakeAStarSettings(32, 32));
    const uint32 west = grid.CellIndex(2, 16);
    const uint32 east = grid.CellIndex(29, 16);
    std::vector<uint32> path;
    ASSERT_TRUE(SolveInWindow(m_Solver, grid, west, east, 0, 0.0f, 0, path));

    for (uint32 z = 0; z < 32; ++z)
        grid.SetCellHeight(16, z, 2.0f);
    AStarSolver freshSolver;
    EXPECT_FALSE(SolveInWindow(freshSolver, grid, west, east, 0, 0.0f, 0, path));
    EXPECT_EQ(freshSolver.HeldBytes(), 0u) << "the solve across the ridge searched";
}

// A* refuses only a step greater than MaxStepHeight, so the regions join across a ridge exactly
// MaxStepHeight high and the solve across completes. Heights 0 and MaxStepHeight differ by exactly
// MaxStepHeight in float32.
TEST_F(AStarSolverTest, ARidgeOfExactlyTheStepHeightJoinsTheRegions)
{
    GridMap grid(MakeAStarSettings(32, 32));
    const float32 stepHeight = grid.GetSettings().MaxStepHeight;
    for (uint32 z = 0; z < 32; ++z)
        grid.SetCellHeight(16, z, stepHeight);
    const uint32 west = grid.CellIndex(2, 16);
    const uint32 east = grid.CellIndex(29, 16);
    std::vector<uint32> path;
    EXPECT_TRUE(SolveInWindow(m_Solver, grid, west, east, 0, 0.0f, 0, path)) << "the step at the limit split the regions";
}

// Opening one cell of a wall that separated two regions joins them: the solve across, which failed
// before the opening, finds the path through the gap.
TEST_F(AStarSolverTest, OpeningAWallJoinsTheRegions)
{
    GridMap grid(MakeAStarSettings(32, 32));
    for (uint32 z = 0; z < 32; ++z)
        grid.SetCellBlocked(16, z, true);
    const uint32 west = grid.CellIndex(2, 16);
    const uint32 east = grid.CellIndex(29, 16);
    std::vector<uint32> path;
    ASSERT_FALSE(SolveInWindow(m_Solver, grid, west, east, 0, 0.0f, 0, path));

    const uint32 gap = grid.CellIndex(16, 30);
    grid.SetCellBlocked(16, 30, false);
    ASSERT_TRUE(SolveInWindow(m_Solver, grid, west, east, 0, 0.0f, 0, path));
    EXPECT_NE(std::find(path.begin(), path.end(), gap), path.end()) << "the path crosses the wall at the gap";
}

// Lowering a ridge that separated two regions joins them: the solve across, which failed while the
// ridge stood higher than an agent steps, finds its path once the ridge is back at the height of
// the cells either side of it.
TEST_F(AStarSolverTest, LoweringARidgeJoinsTheRegions)
{
    GridMap grid(MakeAStarSettings(32, 32));
    for (uint32 z = 0; z < 32; ++z)
        grid.SetCellHeight(16, z, 2.0f);
    const uint32 west = grid.CellIndex(2, 16);
    const uint32 east = grid.CellIndex(29, 16);
    std::vector<uint32> path;
    ASSERT_FALSE(SolveInWindow(m_Solver, grid, west, east, 0, 0.0f, 0, path));

    for (uint32 z = 0; z < 32; ++z)
        grid.SetCellHeight(16, z, 0.0f);
    EXPECT_TRUE(SolveInWindow(m_Solver, grid, west, east, 0, 0.0f, 0, path)) << "the lowered ridge still split the regions";
}

// Dynamic blocking is not part of the regions: obstacles are cleared and stamped again every update,
// so a wall of dynamic cells cleared after a solve leaves the solve across it free to find its path.
TEST_F(AStarSolverTest, ADynamicWallClearedAfterASolveLeavesTheRegionsJoined)
{
    GridMap grid(MakeAStarSettings(32, 32));
    for (uint32 z = 0; z < 32; ++z)
        grid.SetCellDynamicBlocked(16, z, true);
    const uint32 west = grid.CellIndex(2, 16);
    const uint32 east = grid.CellIndex(29, 16);
    std::vector<uint32> path;
    ASSERT_FALSE(SolveInWindow(m_Solver, grid, west, east, 0, 0.0f, 0, path));

    grid.ClearAllDynamicBlocked();
    EXPECT_TRUE(SolveInWindow(m_Solver, grid, west, east, 0, 0.0f, 0, path));
}

// Buildings placed during play are dynamic obstacles, which the regions do not see. A goal they shut
// in is in the start's region, and the solve ends Unreachable after a few cells of the enclosure, not
// after every cell the start reaches.
TEST_F(AStarSolverTest, AGoalShutInByDynamicObstaclesEndsAfterItsEnclosure)
{
    constexpr uint32 kSide = 256;
    constexpr int32 kPocketX = 200, kPocketZ = 190;
    // The 256 cells the search from the start closes alone, plus the nine-cell pocket twice, with room.
    constexpr uint32 kExpansionBound = 1024;
    GridMap grid(MakeAStarSettings(kSide, kSide));
    for (int32 dz = -2; dz <= 2; ++dz)
    {
        for (int32 dx = -2; dx <= 2; ++dx)
        {
            if (std::abs(dx) == 2 || std::abs(dz) == 2)
                grid.SetCellDynamicBlocked(static_cast<uint32>(kPocketX + dx), static_cast<uint32>(kPocketZ + dz), true);
        }
    }

    uint32 expansions = 0;
    const uint32 start = grid.CellIndex(20, 30);
    const uint32 goal = grid.CellIndex(kPocketX, kPocketZ);
    EXPECT_EQ(SolveCountingExpansions(m_Solver, grid, start, goal, 0, expansions), GridSolveEnd::Unreachable);
    EXPECT_LT(expansions, kExpansionBound) << "the solve expanded the start's region";

    // From inside the pocket out, the start side runs out of cells first.
    EXPECT_EQ(SolveCountingExpansions(m_Solver, grid, goal, start, 0, expansions), GridSolveEnd::Unreachable);
    EXPECT_LT(expansions, kExpansionBound);
}

// A goal pressed against a wall, where an agent wider than a cell does not fit, is never reached: the
// solve ends without a search.
TEST_F(AStarSolverTest, AGoalTheFootprintDoesNotFitOnEndsWithoutASearch)
{
    GridMap grid(MakeAStarSettings(64, 64));
    for (uint32 z = 0; z < 64; ++z)
        grid.SetCellBlocked(40, z, true);

    uint32 expansions = 0;
    const uint32 goal = grid.CellIndex(39, 32);
    EXPECT_EQ(SolveCountingExpansions(m_Solver, grid, grid.CellIndex(4, 4), goal, 1, expansions),
              GridSolveEnd::Unreachable);
    EXPECT_EQ(expansions, 0u);
    EXPECT_EQ(SolveCountingExpansions(m_Solver, grid, grid.CellIndex(4, 4), goal, 0, expansions),
              GridSolveEnd::Reached)
        << "a one-cell agent fits";
}

// A* steps diagonally between equal heights when both cardinal cells beside the step are open,
// whatever their height. Two low quadrants that touch only at a corner, the other two a cliff,
// are one region in both diagonal orientations.
TEST_F(AStarSolverTest, ADiagonalStepBetweenTwoCliffsJoinsTheRegions)
{
    for (const bool lowQuadrantsOnTheRisingDiagonal : {false, true})
    {
        GridMap grid(MakeAStarSettings(8, 8));
        for (uint32 z = 0; z < 8; ++z)
        {
            for (uint32 x = 0; x < 8; ++x)
            {
                const bool low = ((x < 4) == (z < 4)) == lowQuadrantsOnTheRisingDiagonal;
                grid.SetCellHeight(x, z, low ? 0.0f : 2.0f);
            }
        }
        // The corner cells are (3, 4) and (4, 3), or (3, 3) and (4, 4); block the cell beyond each.
        const uint32 first = lowQuadrantsOnTheRisingDiagonal ? grid.CellIndex(0, 0) : grid.CellIndex(0, 7);
        const uint32 second = lowQuadrantsOnTheRisingDiagonal ? grid.CellIndex(7, 7) : grid.CellIndex(7, 0);
        if (lowQuadrantsOnTheRisingDiagonal)
        {
            grid.SetCellBlocked(2, 3, true);
            grid.SetCellBlocked(5, 4, true);
        }
        else
        {
            grid.SetCellBlocked(2, 4, true);
            grid.SetCellBlocked(5, 3, true);
        }
        std::vector<uint32> path;
        EXPECT_TRUE(SolveInWindow(m_Solver, grid, first, second, 0, 0.0f, 0, path))
            << (lowQuadrantsOnTheRisingDiagonal ? "rising" : "falling") << " diagonal";
    }
}

namespace
{

// The cells A* may step to from `cell`, by its rules written out again: a neighbour on the grid
// (square: eight, a diagonal only past two open cardinal cells; hex: six, odd-r) with an open
// footprint, within MaxStepHeight and MaxSlope. Every cost on these grids is 1.
std::vector<uint32> StepsFrom(const GridMap& grid, uint32 cell, uint32 agentCellRadius)
{
    constexpr float32 kDiagonal = 1.41421356f;
    const GridSettings& settings = grid.GetSettings();
    const int32 width = static_cast<int32>(settings.Width);
    const int32 depth = static_cast<int32>(settings.Depth);
    uint32 cellX = 0, cellZ = 0;
    grid.CellFromIndex(cell, cellX, cellZ);
    const int32 x = static_cast<int32>(cellX), z = static_cast<int32>(cellZ);
    const auto onGrid = [width, depth](int32 nx, int32 nz) { return nx >= 0 && nz >= 0 && nx < width && nz < depth; };
    const auto open = [&grid, &onGrid](int32 nx, int32 nz) {
        return onGrid(nx, nz) && !grid.IsCellBlocked(static_cast<uint32>(nx), static_cast<uint32>(nz));
    };

    int32 offsetX[8], offsetZ[8];
    uint32 offsetCount = 0;
    if (settings.Type == GridType::Hex)
    {
        HexGrid::GetNeighborOffsets(cellZ, offsetX, offsetZ, offsetCount);
    }
    else
    {
        static constexpr int32 kSquareX[] = {0, 0, 1, -1, 1, -1, 1, -1};
        static constexpr int32 kSquareZ[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        std::copy(std::begin(kSquareX), std::end(kSquareX), offsetX);
        std::copy(std::begin(kSquareZ), std::end(kSquareZ), offsetZ);
        offsetCount = 8;
    }

    std::vector<uint32> steps;
    for (uint32 offset = 0; offset < offsetCount; ++offset)
    {
        const int32 nx = x + offsetX[offset], nz = z + offsetZ[offset];
        const bool diagonal = settings.Type == GridType::Square && offsetX[offset] != 0 && offsetZ[offset] != 0;
        if (!onGrid(nx, nz) || (diagonal && (!open(nx, z) || !open(x, nz))))
            continue;
        bool footprintOpen = true;
        const int32 radius = static_cast<int32>(agentCellRadius);
        for (int32 fz = nz - radius; fz <= nz + radius; ++fz)
            for (int32 fx = nx - radius; fx <= nx + radius; ++fx)
                footprintOpen = footprintOpen && open(fx, fz);
        if (!footprintOpen)
            continue;
        const float32 rise = std::abs(grid.GetCellHeight(static_cast<uint32>(nx), static_cast<uint32>(nz)) -
                                      grid.GetCellHeight(cellX, cellZ));
        const float32 run = (diagonal ? kDiagonal : 1.0f) * settings.CellSize;
        if (rise > settings.MaxStepHeight || Mathematics::ToDegrees(std::atan2(rise, run)) > settings.MaxSlope)
            continue;
        steps.push_back(grid.CellIndex(static_cast<uint32>(nx), static_cast<uint32>(nz)));
    }
    return steps;
}

// Whether A*'s steps lead from start to end, by breadth-first search.
bool ReachesByBreadthFirstSearch(const GridMap& grid, uint32 start, uint32 end, uint32 agentCellRadius)
{
    if (start == end)
        return true;
    uint32 x = 0, z = 0, endX = 0, endZ = 0;
    grid.CellFromIndex(start, x, z);
    grid.CellFromIndex(end, endX, endZ);
    if (grid.IsCellBlocked(x, z) || grid.IsCellBlocked(endX, endZ))
        return false;
    std::vector<uint8> seen(grid.GetCellCount(), 0);
    std::vector<uint32> frontier{start};
    seen[start] = 1;
    while (!frontier.empty())
    {
        const uint32 cell = frontier.back();
        frontier.pop_back();
        for (const uint32 next : StepsFrom(grid, cell, agentCellRadius))
        {
            if (next == end)
                return true;
            if (seen[next] == 0)
            {
                seen[next] = 1;
                frontier.push_back(next);
            }
        }
    }
    return false;
}

// The cells A*'s steps reach from `start`, the start included, by breadth-first search.
uint32 ReachableCellCount(const GridMap& grid, uint32 start, uint32 agentCellRadius)
{
    std::vector<uint8> seen(grid.GetCellCount(), 0);
    std::vector<uint32> frontier{start};
    seen[start] = 1;
    uint32 count = 1;
    while (!frontier.empty())
    {
        const uint32 cell = frontier.back();
        frontier.pop_back();
        for (const uint32 next : StepsFrom(grid, cell, agentCellRadius))
        {
            if (seen[next] != 0)
                continue;
            seen[next] = 1;
            ++count;
            frontier.push_back(next);
        }
    }
    return count;
}

// Two grids with the same cells: `wallsStatic` holds the walls as static blocking, which the
// regions see, and `wallsDynamic` as dynamic blocking, which they do not. Plateau steps of 0.25,
// 0.35 and 0.6 are a step taken, a step the 15-degree slope limit refuses, and a cliff.
void FillTwinGrids(GridMap& wallsStatic, GridMap& wallsDynamic, std::mt19937& random, float32 wallDensity)
{
    static constexpr float32 kHeights[] = {0.0f, 0.25f, 0.6f};
    constexpr uint32 kPlateau = 4;
    std::uniform_real_distribution<float32> unit(0.0f, 1.0f);
    const GridSettings& settings = wallsStatic.GetSettings();
    const uint32 plateausPerRow = settings.Width / kPlateau + 1;
    std::vector<float32> plateaus(plateausPerRow * (settings.Depth / kPlateau + 1));
    for (float32& plateau : plateaus)
        plateau = kHeights[random() % 3];
    for (uint32 z = 0; z < settings.Depth; ++z)
    {
        for (uint32 x = 0; x < settings.Width; ++x)
        {
            const float32 height = plateaus[(z / kPlateau) * plateausPerRow + x / kPlateau];
            wallsStatic.SetCellHeight(x, z, height);
            wallsDynamic.SetCellHeight(x, z, height);
            const float32 roll = unit(random);
            if (roll < wallDensity)
            {
                wallsStatic.SetCellBlocked(x, z, true);
                wallsDynamic.SetCellDynamicBlocked(x, z, true);
            }
            else if (roll < wallDensity + 0.03f)
            {
                wallsStatic.SetCellDynamicBlocked(x, z, true);
                wallsDynamic.SetCellDynamicBlocked(x, z, true);
            }
        }
    }
}

} // namespace

// The regions and the flood from the goal never fail a solve the search completes and never change a
// path: each solve on seeded random grids matches its twin with dynamic walls and a breadth-first oracle.
// Enough solves end on each early exit that the comparison covers both.
TEST_F(AStarSolverTest, RegionsNeverFailASolveTheSearchCompletes)
{
    constexpr uint32 kPairsPerGrid = 300;
    struct GridSize
    {
        uint32 Width;
        uint32 Depth;
    };
    // The larger grid has room for solves longer than 256 expansions, where the flood from the goal runs.
    static constexpr GridSize kSizes[] = {{24, 20}, {72, 60}};
    std::mt19937 random(11);
    uint32 completed = 0, failedOnTheRegions = 0, endedOnTheGoalSide = 0;
    for (const GridSize size : kSizes)
    {
        for (const GridType type : {GridType::Square, GridType::Hex})
        {
            for (const float32 maxSlope : {45.0f, 15.0f})
            {
                for (const float32 wallDensity : {0.1f, 0.25f, 0.4f})
                {
                    GridSettings settings = MakeAStarSettings(size.Width, size.Depth, type);
                    settings.MaxSlope = maxSlope;
                    GridMap wallsStatic(settings);
                    GridMap wallsDynamic(settings);
                    FillTwinGrids(wallsStatic, wallsDynamic, random, wallDensity);
                    for (uint32 pair = 0; pair < kPairsPerGrid; ++pair)
                    {
                        const uint32 start = static_cast<uint32>(random() % (size.Width * size.Depth));
                        const uint32 end = static_cast<uint32>(random() % (size.Width * size.Depth));
                        const uint32 radius = pair % 2;
                        AStarSolver freshSolver;
                        std::vector<uint32> staticPath, dynamicPath;
                        const bool staticFound = SolveInWindow(freshSolver, wallsStatic, start, end, 0, 0.0f, radius, staticPath);
                        const bool dynamicFound = SolveInWindow(m_Solver, wallsDynamic, start, end, 0, 0.0f, radius, dynamicPath);
                        ASSERT_EQ(staticFound, dynamicFound) << "pair " << pair << " slope " << maxSlope << " walls " << wallDensity;
                        ASSERT_EQ(staticPath, dynamicPath) << "pair " << pair;
                        ASSERT_EQ(staticFound, ReachesByBreadthFirstSearch(wallsStatic, start, end, radius)) << "pair " << pair;
                        completed += staticFound ? 1u : 0u;
                        uint32 expansions = 0;
                        const GridSolveEnd dynamicEnd = SolveCountingExpansions(m_Solver, wallsDynamic, start, end, radius, expansions);
                        endedOnTheGoalSide += (dynamicEnd == GridSolveEnd::Unreachable && expansions > 0u &&
                                               expansions < ReachableCellCount(wallsDynamic, start, radius)) ? 1u : 0u;
                        failedOnTheRegions +=
                            SolveCountingExpansions(freshSolver, wallsStatic, start, end, radius, expansions) == GridSolveEnd::OtherRegion
                                ? 1u
                                : 0u;
                    }
                }
            }
        }
    }
    EXPECT_GT(completed, 500u) << completed << " solves completed; too few for the paths to be compared";
    EXPECT_GT(failedOnTheRegions, 200u) << failedOnTheRegions << " solves failed on the regions; too few for the early exit to be compared";
    EXPECT_GT(endedOnTheGoalSide, 50u) << endedOnTheGoalSide << " solves ended on the goal side; too few for that exit to be compared";
}
