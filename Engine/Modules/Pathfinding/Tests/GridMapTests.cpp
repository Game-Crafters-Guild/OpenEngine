#include "Pathfinding/GridMap.h"
#include "Pathfinding/PathBuffer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

namespace
{

GridSettings MakeDefaultSettings(uint32 width = 16, uint32 depth = 16, float32 cellSize = 1.0f)
{
    GridSettings settings;
    settings.Type = GridType::Square;
    settings.CellSize = cellSize;
    settings.OriginX = 0.0f;
    settings.OriginY = 0.0f;
    settings.OriginZ = 0.0f;
    settings.Width = width;
    settings.Depth = depth;
    settings.MaxSlope = 45.0f;
    settings.MaxStepHeight = 0.4f;
    return settings;
}

} // namespace

class GridMapTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Map = std::make_unique<GridMap>(MakeDefaultSettings());
    }

    std::unique_ptr<GridMap> m_Map;
};

TEST_F(GridMapTest, ConstructionInitializesCorrectSizes)
{
    EXPECT_EQ(m_Map->GetSettings().Width, 16u);
    EXPECT_EQ(m_Map->GetSettings().Depth, 16u);
    EXPECT_EQ(m_Map->GetCellCount(), 256u);
}

TEST_F(GridMapTest, WorldToCellAndCellToWorldRoundTrip)
{
    float32 worldX = 0.0f, worldZ = 0.0f;
    m_Map->CellToWorld(3, 5, worldX, worldZ);

    EXPECT_FLOAT_EQ(worldX, 3.5f);
    EXPECT_FLOAT_EQ(worldZ, 5.5f);

    uint32 cellX = 0, cellZ = 0;
    EXPECT_TRUE(m_Map->WorldToCell(worldX, worldZ, cellX, cellZ));
    EXPECT_EQ(cellX, 3u);
    EXPECT_EQ(cellZ, 5u);
}

TEST_F(GridMapTest, WorldToCellReturnsFalseForOutOfBounds)
{
    uint32 cellX = 0, cellZ = 0;

    EXPECT_FALSE(m_Map->WorldToCell(-1.0f, 0.0f, cellX, cellZ));
    EXPECT_FALSE(m_Map->WorldToCell(0.0f, -1.0f, cellX, cellZ));
    EXPECT_FALSE(m_Map->WorldToCell(100.0f, 0.0f, cellX, cellZ));
    EXPECT_FALSE(m_Map->WorldToCell(0.0f, 100.0f, cellX, cellZ));
}

TEST_F(GridMapTest, SetCellCostAndGetCellCost)
{
    m_Map->SetCellCost(2, 3, 5.0f);
    EXPECT_FLOAT_EQ(m_Map->GetCellCost(2, 3), 5.0f);

    EXPECT_FLOAT_EQ(m_Map->GetCellCost(0, 0), 1.0f);
}

TEST_F(GridMapTest, SetCellBlockedAndIsCellBlocked)
{
    EXPECT_FALSE(m_Map->IsCellBlocked(4, 4));

    m_Map->SetCellBlocked(4, 4, true);
    EXPECT_TRUE(m_Map->IsCellBlocked(4, 4));

    m_Map->SetCellBlocked(4, 4, false);
    EXPECT_FALSE(m_Map->IsCellBlocked(4, 4));
}

TEST_F(GridMapTest, SetCellHeightAndGetCellHeight)
{
    m_Map->SetCellHeight(1, 1, 3.5f);
    EXPECT_FLOAT_EQ(m_Map->GetCellHeight(1, 1), 3.5f);

    EXPECT_FLOAT_EQ(m_Map->GetCellHeight(0, 0), 0.0f);
}

TEST_F(GridMapTest, IsPointNavigableReturnsFalseForBlockedCells)
{
    m_Map->SetCellBlocked(5, 5, true);

    EXPECT_FALSE(m_Map->IsPointNavigable(5.5f, 0.0f, 5.5f, 0.5f));
}

TEST_F(GridMapTest, IsPointNavigableReturnsTrueForOpenCells)
{
    EXPECT_TRUE(m_Map->IsPointNavigable(5.5f, 0.0f, 5.5f, 0.5f));
}

// #4044: a body of the radius around the point keeps clear of every blocked cell's rectangle,
// wherever in its cell the point lies and on both sides of half a cell. Cell (3, 3) is [3, 4] x [3, 4].
TEST_F(GridMapTest, IsPointNavigableKeepsTheRadiusFromBlockedCells)
{
    m_Map->SetCellBlocked(3, 3, true);

    EXPECT_FALSE(m_Map->IsPointNavigable(2.9f, 0.0f, 3.5f, 0.45f)) << "0.1 m from the cell";
    EXPECT_TRUE(m_Map->IsPointNavigable(2.5f, 0.0f, 3.5f, 0.45f)) << "0.5 m from the cell";
    EXPECT_FALSE(m_Map->IsPointNavigable(3.5f, 0.0f, 3.5f, 0.45f)) << "inside the cell";
    EXPECT_TRUE(m_Map->IsPointNavigable(2.9f, 0.0f, 3.5f, 0.0f)) << "a point needs only its own cell open";
    EXPECT_FALSE(m_Map->IsPointNavigable(2.8f, 0.0f, 2.8f, 0.3f)) << "0.28 m from the cell's corner";
    EXPECT_TRUE(m_Map->IsPointNavigable(2.4f, 0.0f, 3.5f, 0.55f)) << "0.6 m from the cell";
    EXPECT_FALSE(m_Map->IsPointNavigable(2.5f, 0.0f, 3.5f, 0.55f)) << "0.5 m from the cell";
}

// Blocked cells inside `openCells` count as open, the start's own cell included;
// outside it they are walls.
TEST_F(GridMapTest, IsSegmentClearCountsTheOpenCellsAsOpen)
{
    m_Map->SetCellBlocked(5, 5, true); // the rectangle [5, 6] x [5, 6]
    const GridMap::CellRange aroundTheCell{4, 4, 6, 6};
    const GridMap::CellRange elsewhere{10, 10, 12, 12};

    EXPECT_TRUE(m_Map->IsSegmentClear(5.5f, 5.5f, 9.5f, 5.5f, 0.45f, &aroundTheCell))
        << "a start inside a blocked cell of the range";
    EXPECT_FALSE(m_Map->IsSegmentClear(5.5f, 5.5f, 9.5f, 5.5f, 0.45f, &elsewhere))
        << "a start inside a blocked cell outside the range";
    // Along z = 4.8 the segment passes the cell 0.2 m away.
    EXPECT_TRUE(m_Map->IsSegmentClear(2.5f, 4.8f, 9.5f, 4.8f, 0.45f, &aroundTheCell));
    EXPECT_FALSE(m_Map->IsSegmentClear(2.5f, 4.8f, 9.5f, 4.8f, 0.45f, &elsewhere));
}

// #4044: off the grid counts as blocked, so a body near the grid's edge does not fit.
TEST_F(GridMapTest, IsPointNavigableKeepsTheRadiusFromTheGridEdge)
{
    EXPECT_FALSE(m_Map->IsPointNavigable(0.3f, 0.0f, 5.5f, 0.45f)) << "0.3 m from the edge";
    EXPECT_TRUE(m_Map->IsPointNavigable(0.5f, 0.0f, 5.5f, 0.45f)) << "0.5 m from the edge";
}

// #4044: a hex grid's odd rows are shifted half a cell along X; the clearance test reads the
// shifted rectangle WorldToCell maps to the cell.
TEST_F(GridMapTest, IsPointNavigableReadsTheHexRowShift)
{
    GridSettings settings = MakeDefaultSettings();
    settings.Type = GridType::Hex;
    GridMap hex(settings);
    hex.SetCellBlocked(4, 5, true); // an odd row
    float32 centreX = 0.0f, centreZ = 0.0f;
    hex.CellToWorld(4, 5, centreX, centreZ);

    EXPECT_FALSE(hex.IsPointNavigable(centreX + 0.7f, 0.0f, centreZ, 0.3f)) << "0.2 m right of the cell";
    EXPECT_TRUE(hex.IsPointNavigable(centreX - 0.9f, 0.0f, centreZ, 0.3f)) << "0.4 m left of the cell";
}

// #4044: smoothing keeps the agent's radius from blocked cells. The unsmoothed route from cell
// (0, 0) to cell (5, 3) passes the blocked cell (3, 3) at 0.71 m; the straight shortcut passes its
// corner at 0.34 m, inside a 0.45 m body.
TEST_F(GridMapTest, SmoothedPathKeepsTheAgentRadiusFromBlockedCells)
{
    m_Map->SetCellBlocked(3, 3, true);
    PathRequest request;
    request.StartX = 0.5f;
    request.StartZ = 0.5f;
    request.EndX = 5.5f;
    request.EndZ = 3.5f;
    request.AgentRadius = 0.45f;

    for (const bool smooth : {false, true})
    {
        request.SmoothPath = smooth;
        PathBuffer pathBuffer;
        PathHandle path;
        ASSERT_EQ(m_Map->FindPath(request, pathBuffer, path), PathStatus::Complete);
        // The nearest any point of the route comes to [3, 4] x [3, 4], sampled every millimetre.
        float32 nearest = std::numeric_limits<float32>::max();
        for (uint32 i = 1; i < pathBuffer.GetPointCount(path); ++i)
        {
            const PathPoint a = pathBuffer.GetPoint(path, i - 1);
            const PathPoint b = pathBuffer.GetPoint(path, i);
            const uint32 samples = 1 + static_cast<uint32>(std::hypot(b.X - a.X, b.Z - a.Z) * 1000.0f);
            for (uint32 s = 0; s <= samples; ++s)
            {
                const float32 t = static_cast<float32>(s) / static_cast<float32>(samples);
                const float32 dx = std::max({3.0f - (a.X + (b.X - a.X) * t), 0.0f, a.X + (b.X - a.X) * t - 4.0f});
                const float32 dz = std::max({3.0f - (a.Z + (b.Z - a.Z) * t), 0.0f, a.Z + (b.Z - a.Z) * t - 4.0f});
                nearest = std::min(nearest, std::hypot(dx, dz));
            }
        }
        EXPECT_GE(nearest, 0.45f - 1.0e-3f) << (smooth ? "smoothed" : "unsmoothed");
    }
}

TEST_F(GridMapTest, GetClosestNavigablePointFindsAdjacentCell)
{
    for (int32 dx = -1; dx <= 1; ++dx)
    {
        for (int32 dz = -1; dz <= 1; ++dz)
        {
            m_Map->SetCellBlocked(static_cast<uint32>(5 + dx), static_cast<uint32>(5 + dz), true);
        }
    }
    m_Map->SetCellBlocked(6, 4, false);

    float32 outX = 0.0f, outY = 0.0f, outZ = 0.0f;
    bool found = m_Map->GetClosestNavigablePoint(5.5f, 0.0f, 5.5f, 10.0f, outX, outY, outZ);

    EXPECT_TRUE(found);
    EXPECT_FLOAT_EQ(outX, 6.5f);
    EXPECT_FLOAT_EQ(outZ, 4.5f);
}

TEST_F(GridMapTest, GetClosestNavigablePointReturnsStartIfAlreadyNavigable)
{
    float32 outX = 0.0f, outY = 0.0f, outZ = 0.0f;
    bool found = m_Map->GetClosestNavigablePoint(5.5f, 0.0f, 5.5f, 10.0f, outX, outY, outZ);

    EXPECT_TRUE(found);
    EXPECT_FLOAT_EQ(outX, 5.5f);
    EXPECT_FLOAT_EQ(outZ, 5.5f);
}

TEST_F(GridMapTest, RaycastThroughOpenGridReturnsFalse)
{
    float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    bool hit = m_Map->Raycast(0.5f, 0.0f, 0.5f, 15.5f, 0.0f, 15.5f, hitX, hitY, hitZ);

    EXPECT_FALSE(hit);
}

TEST_F(GridMapTest, RaycastHittingBlockedCellReturnsTrue)
{
    m_Map->SetCellBlocked(8, 8, true);

    float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    bool hit = m_Map->Raycast(0.5f, 0.0f, 0.5f, 15.5f, 0.0f, 15.5f, hitX, hitY, hitZ);

    EXPECT_TRUE(hit);
    EXPECT_FLOAT_EQ(hitX, 8.5f);
    EXPECT_FLOAT_EQ(hitZ, 8.5f);
}

TEST_F(GridMapTest, FindPathOnOpenGridReturnsComplete)
{
    PathBuffer pathBuffer;
    PathHandle outPath;

    PathRequest request;
    request.StartX = 0.5f;
    request.StartZ = 0.5f;
    request.EndX = 10.5f;
    request.EndZ = 10.5f;

    PathStatus status = m_Map->FindPath(request, pathBuffer, outPath);

    EXPECT_EQ(status, PathStatus::Complete);
    EXPECT_TRUE(outPath.IsValid());
    EXPECT_GT(pathBuffer.GetPointCount(outPath), 0u);
}

TEST_F(GridMapTest, FindPathWhenBlockedReturnsFailed)
{
    for (uint32 z = 0; z < 16; ++z)
    {
        m_Map->SetCellBlocked(8, z, true);
    }

    PathBuffer pathBuffer;
    PathHandle outPath;

    PathRequest request;
    request.StartX = 0.5f;
    request.StartZ = 0.5f;
    request.EndX = 15.5f;
    request.EndZ = 0.5f;

    PathStatus status = m_Map->FindPath(request, pathBuffer, outPath);

    EXPECT_EQ(status, PathStatus::Failed);
}

TEST_F(GridMapTest, FindPathFromAToBProducesValidWaypoints)
{
    PathBuffer pathBuffer;
    PathHandle outPath;

    PathRequest request;
    request.StartX = 0.5f;
    request.StartZ = 0.5f;
    request.EndX = 5.5f;
    request.EndZ = 0.5f;

    PathStatus status = m_Map->FindPath(request, pathBuffer, outPath);

    ASSERT_EQ(status, PathStatus::Complete);

    uint32 count = pathBuffer.GetPointCount(outPath);
    ASSERT_GT(count, 1u);

    PathPoint first = pathBuffer.GetPoint(outPath, 0);
    PathPoint last = pathBuffer.GetPoint(outPath, count - 1);

    EXPECT_NEAR(first.X, 0.5f, 1.0f);
    EXPECT_NEAR(first.Z, 0.5f, 1.0f);
    EXPECT_NEAR(last.X, 5.5f, 1.0f);
    EXPECT_NEAR(last.Z, 0.5f, 1.0f);
}

TEST_F(GridMapTest, ReserveCellAndGetCellReservation)
{
    m_Map->ReserveCell(3, 4, 42, 100.0f);

    CellReservation reservation = m_Map->GetCellReservation(3, 4);
    EXPECT_EQ(reservation.AgentId, 42u);
    EXPECT_FLOAT_EQ(reservation.ExpirationTime, 100.0f);
    EXPECT_TRUE(reservation.IsActive(50.0f));
    EXPECT_FALSE(reservation.IsActive(200.0f));
}

TEST_F(GridMapTest, ClearReservationsForAgentLeavesOtherAgents)
{
    m_Map->ReserveCell(1, 1, 10, 50.0f);
    m_Map->ReserveCell(2, 2, 20, 50.0f);

    m_Map->ClearReservationsForAgent(10);

    EXPECT_EQ(m_Map->GetCellReservation(1, 1).AgentId, 0u);
    EXPECT_EQ(m_Map->GetCellReservation(2, 2).AgentId, 20u);
}

TEST_F(GridMapTest, ClearThenReserveDropsCellsBehindTheAgent)
{
    m_Map->ReserveCell(1, 1, 10, 50.0f);
    m_Map->ReserveCell(2, 1, 10, 50.0f);
    m_Map->ClearReservationsForAgent(10);
    m_Map->ReserveCell(2, 1, 10, 50.0f);

    EXPECT_EQ(m_Map->GetCellReservation(1, 1).AgentId, 0u);
    EXPECT_EQ(m_Map->GetCellReservation(2, 1).AgentId, 10u);
}

TEST_F(GridMapTest, ClearReservationsForAgentSkipsCellsStolenByAnotherAgent)
{
    m_Map->ReserveCell(3, 3, 10, 50.0f);
    m_Map->ReserveCell(3, 3, 20, 50.0f);
    m_Map->ClearReservationsForAgent(10);

    EXPECT_EQ(m_Map->GetCellReservation(3, 3).AgentId, 20u);
}

TEST_F(GridMapTest, OccupancySticksWhenSameAgentRestampsSoft)
{
    m_Map->ReserveCell(4, 4, 7, 50.0f, true);
    m_Map->ReserveCell(4, 4, 7, 50.0f, false);
    EXPECT_NE(m_Map->GetCellReservation(4, 4).Occupancy, 0u);
}

TEST_F(GridMapTest, ClearedSlotCanBeReusedByAnotherAgent)
{
    m_Map->ReserveCell(1, 1, 10, 50.0f);
    m_Map->ClearReservationsForAgent(10);
    m_Map->ReserveCell(5, 5, 20, 50.0f);

    EXPECT_EQ(m_Map->GetCellReservation(1, 1).AgentId, 0u);
    EXPECT_EQ(m_Map->GetCellReservation(5, 5).AgentId, 20u);
}

TEST_F(GridMapTest, ClearExpiredReservationsRemovesOldReservations)
{
    m_Map->ReserveCell(1, 1, 10, 5.0f);
    m_Map->ReserveCell(2, 2, 20, 15.0f);

    m_Map->ClearExpiredReservations(10.0f);

    CellReservation r1 = m_Map->GetCellReservation(1, 1);
    CellReservation r2 = m_Map->GetCellReservation(2, 2);

    EXPECT_EQ(r1.AgentId, 0u);
    EXPECT_EQ(r2.AgentId, 20u);
    EXPECT_EQ(m_Map->GetReservedAgentCount(), 1u);
}

TEST_F(GridMapTest, ReservedAgentCountTracksLiveAgentsNotLifetime)
{
    EXPECT_EQ(m_Map->GetReservedAgentCount(), 0u);
    for (uint32 i = 1; i <= 200; ++i)
    {
        m_Map->ReserveCell(i % 16, (i / 16) % 16, i, 50.0f);
        m_Map->ClearReservationsForAgent(i);
    }
    EXPECT_EQ(m_Map->GetReservedAgentCount(), 0u);

    m_Map->ReserveCell(3, 3, 7, 50.0f);
    EXPECT_EQ(m_Map->GetReservedAgentCount(), 1u);
}

TEST_F(GridMapTest, ManyAgentsStayAtConcurrentCount)
{
    constexpr uint32 kAgents = 512;
    for (uint32 i = 1; i <= kAgents; ++i)
    {
        const uint32 x = i % 16;
        const uint32 z = (i / 16) % 16;
        for (uint32 k = 0; k < 8; ++k)
            m_Map->ReserveCell((x + k) % 16, z, i, 50.0f);
    }
    EXPECT_EQ(m_Map->GetReservedAgentCount(), kAgents);
    for (uint32 i = 1; i <= kAgents; ++i)
        m_Map->ClearReservationsForAgent(i);
    EXPECT_EQ(m_Map->GetReservedAgentCount(), 0u);
}

TEST_F(GridMapTest, IsOccupiedByOtherIgnoresSelfAndExpired)
{
    m_Map->ReserveCell(2, 2, 9, 10.0f, true);
    EXPECT_TRUE(m_Map->IsOccupiedByOther(2, 2, 0, 0.0f));
    EXPECT_FALSE(m_Map->IsOccupiedByOther(2, 2, 9, 0.0f));
    EXPECT_FALSE(m_Map->IsOccupiedByOther(2, 2, 0, 20.0f));
    m_Map->ReserveCell(3, 3, 9, 10.0f, false);
    EXPECT_FALSE(m_Map->IsOccupiedByOther(3, 3, 0, 0.0f));
}

TEST_F(GridMapTest, DynamicBlockingBlocksAndClears)
{
    // Cell (5,5) should not be dynamically blocked initially
    EXPECT_FALSE(m_Map->IsCellDynamicBlocked(5, 5));
    EXPECT_FALSE(m_Map->IsCellBlocked(5, 5));

    // Set dynamic blocking
    m_Map->SetCellDynamicBlocked(5, 5, true);
    EXPECT_TRUE(m_Map->IsCellDynamicBlocked(5, 5));

    // Block an entire column dynamically to test FindPath avoidance
    for (uint32 z = 0; z < 16; ++z)
        m_Map->SetCellDynamicBlocked(8, z, true);

    // FindPath through column 8 should fail
    {
        PathBuffer pathBuffer;
        PathHandle outPath;
        PathRequest request;
        request.StartX = 0.5f; request.StartZ = 0.5f;
        request.EndX = 15.5f;  request.EndZ = 0.5f;
        PathStatus status = m_Map->FindPath(request, pathBuffer, outPath);
        EXPECT_EQ(status, PathStatus::Failed);
    }

    // Clear all dynamic blocking
    m_Map->ClearAllDynamicBlocked();
    EXPECT_FALSE(m_Map->IsCellDynamicBlocked(5, 5));
    EXPECT_FALSE(m_Map->IsCellDynamicBlocked(8, 0));

    // Path should now succeed
    {
        PathBuffer pathBuffer;
        PathHandle outPath;
        PathRequest request;
        request.StartX = 0.5f; request.StartZ = 0.5f;
        request.EndX = 15.5f;  request.EndZ = 0.5f;
        PathStatus status = m_Map->FindPath(request, pathBuffer, outPath);
        EXPECT_EQ(status, PathStatus::Complete);
    }
}

TEST_F(GridMapTest, TopologyVersionIncrements)
{
    const uint32 v0 = m_Map->GetTopologyVersion();

    m_Map->SetCellCost(1, 1, 3.0f);
    const uint32 v1 = m_Map->GetTopologyVersion();
    EXPECT_GT(v1, v0);

    m_Map->SetCellBlocked(2, 2, true);
    const uint32 v2 = m_Map->GetTopologyVersion();
    EXPECT_GT(v2, v1);

    m_Map->SetCellHeight(3, 3, 5.0f);
    const uint32 v3 = m_Map->GetTopologyVersion();
    EXPECT_GT(v3, v2);

    EXPECT_EQ(v3, v0 + 3u);
}

TEST_F(GridMapTest, ReadViewReadsTheMapsCells)
{
    m_Map->SetCellCost(2, 3, 4.0f);
    m_Map->SetCellHeight(2, 3, 0.25f);
    m_Map->SetCellBlocked(5, 6, true);
    m_Map->SetCellDynamicBlocked(7, 8, true);
    m_Map->ReserveCell(2, 3, 42, 100.0f, true);

    m_Map->OpenPathSolveWindow();
    const GridMap::ReadView view = m_Map->View();
    EXPECT_FLOAT_EQ(view.GetCellCost(2, 3), 4.0f);
    EXPECT_FLOAT_EQ(view.GetCellHeight(2, 3), 0.25f);
    EXPECT_TRUE(view.IsCellBlocked(5, 6));
    EXPECT_TRUE(view.IsCellBlocked(7, 8));
    EXPECT_FALSE(view.IsCellBlocked(2, 3));
    const CellReservation reservation = view.Reservation(2, 3);
    EXPECT_EQ(reservation.AgentId, 42u);
    EXPECT_FLOAT_EQ(reservation.ExpirationTime, 100.0f);
    EXPECT_EQ(reservation.Occupancy, 1u);
    m_Map->ClosePathSolveWindow();
}

#if !defined(NDEBUG)
TEST(GridMapReadView, WriterAssertsWhileViewOpen)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    GridMap map(MakeDefaultSettings());
    EXPECT_DEATH(
        {
            map.OpenPathSolveWindow();
            const GridMap::ReadView view = map.View();
            map.SetCellBlocked(1, 1, !view.IsCellBlocked(1, 1));
        },
        "while path solves read it");
    EXPECT_DEATH(
        {
            map.OpenPathSolveWindow();
            const GridMap::ReadView view = map.View();
            map.ReserveCell(1, 1, view.Reservation(1, 1).AgentId + 1, 10.0f);
        },
        "while path solves read it");
    EXPECT_DEATH(static_cast<void>(map.View()), "outside a path-solve window");
}
#endif

#if defined(NDEBUG)
// Release only: in asserting builds every writer asserts before it locks, so only Release can
// show that an open path-solve window holds the reservation lock a writer waits on.
TEST_F(GridMapTest, ReservationWriterWaitsForAnOpenPathSolveWindow)
{
    GridMap& map = *m_Map;
    map.OpenPathSolveWindow();
    std::atomic<bool> wrote{false};
    std::thread writer([&] {
        map.ReserveCell(1, 1, 7, 100.0f, false);
        wrote.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool wroteInsideWindow = wrote.load();
    map.ClosePathSolveWindow();
    writer.join();
    EXPECT_FALSE(wroteInsideWindow) << "a reservation write landed while a path-solve window was open";
    EXPECT_TRUE(wrote.load());
}
#endif
