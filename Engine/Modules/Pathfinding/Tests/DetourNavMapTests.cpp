#include <gtest/gtest.h>
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/ObstacleAvoidance.h"
#include "Pathfinding/PathBuffer.h"

#include <DetourCommon.h>
#include <DetourNavMesh.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

// ---------------------------------------------------------------------------
// Test geometry: a subdivided 20x20 flat plane at Y=0, centered at origin.
// Grid of triangles to give Recast enough spans to work with.
// ---------------------------------------------------------------------------
namespace DetourTestHelpers
{
namespace
{

constexpr uint32 kGridRes = 10; // 10x10 quads = 200 triangles
static constexpr float32 kPlaneSize = 20.0f;
static constexpr float32 kPlaneHalf = kPlaneSize * 0.5f;

struct PlaneGeometry
{
    std::vector<float32> Vertices;
    std::vector<uint32> Indices;
    uint32 VertexCount = 0;
    uint32 TriangleCount = 0;
};

PlaneGeometry BuildPlane()
{
    PlaneGeometry g;
    const uint32 vertsPerSide = kGridRes + 1;
    g.VertexCount = vertsPerSide * vertsPerSide;
    g.Vertices.resize(g.VertexCount * 3);

    for (uint32 z = 0; z <= kGridRes; ++z)
    {
        for (uint32 x = 0; x <= kGridRes; ++x)
        {
            uint32 idx = (z * vertsPerSide + x) * 3;
            g.Vertices[idx + 0] = -kPlaneHalf + static_cast<float32>(x) * (kPlaneSize / kGridRes);
            g.Vertices[idx + 1] = 0.0f;
            g.Vertices[idx + 2] = -kPlaneHalf + static_cast<float32>(z) * (kPlaneSize / kGridRes);
        }
    }

    g.TriangleCount = kGridRes * kGridRes * 2;
    g.Indices.resize(g.TriangleCount * 3);
    uint32 ti = 0;
    for (uint32 z = 0; z < kGridRes; ++z)
    {
        for (uint32 x = 0; x < kGridRes; ++x)
        {
            uint32 topLeft = z * vertsPerSide + x;
            uint32 topRight = topLeft + 1;
            uint32 botLeft = topLeft + vertsPerSide;
            uint32 botRight = botLeft + 1;

            g.Indices[ti++] = topLeft;
            g.Indices[ti++] = botLeft;
            g.Indices[ti++] = topRight;

            g.Indices[ti++] = topRight;
            g.Indices[ti++] = botLeft;
            g.Indices[ti++] = botRight;
        }
    }
    return g;
}

NavMeshSettings MakeDetourSettings()
{
    NavMeshSettings s;
    s.CellSize = 0.3f;
    s.AgentRadius = 0.6f;
    s.AgentHeight = 2.0f;
    s.StepHeight = 0.9f;
    s.MaxSlope = 45.0f;
    return s;
}

// A chart whose tiles (0, 0) to (1, 1) meet at the plane's centre: 64 cells of 0.3 m is 19.2 m, so
// each tile holds a 10 m half of the 20 m plane and every path across it crosses a tile edge.
NavMeshChart MakePlaneChart(const NavMeshSettings& settings)
{
    const float32 tileSize = DetourNavMap::GetTileSize(settings);
    NavMeshChart chart;
    chart.Origin = ChartOrigin::AtWorld(-tileSize, -1.0f, -tileSize);
    chart.Span = 2.0f * tileSize;
    return chart;
}

// The plane's vertices moved into `chart`'s frame, the input BuildTile reads.
PlaneGeometry InChart(const PlaneGeometry& plane, const NavMeshChart& chart)
{
    PlaneGeometry local = plane;
    for (size_t i = 0; i < local.Vertices.size(); i += 3)
        chart.ToChart(plane.Vertices[i], plane.Vertices[i + 1], plane.Vertices[i + 2], &local.Vertices[i]);
    return local;
}

DetourNavMap::InputGeometry MakePlaneInput(const PlaneGeometry& plane)
{
    DetourNavMap::InputGeometry geo;
    geo.Vertices = plane.Vertices.data();
    geo.VertexCount = plane.VertexCount;
    geo.Indices = plane.Indices.data();
    geo.TriangleCount = plane.TriangleCount;
    return geo;
}

// Initializes `navMap` with the plane's chart and builds and adds its four tiles from `plane`.
bool BuildAllTiles(DetourNavMap& navMap, const NavMeshSettings& settings, const PlaneGeometry& plane)
{
    const NavMeshChart chart = MakePlaneChart(settings);
    if (!navMap.Initialize(settings, chart))
        return false;
    const PlaneGeometry local = InChart(plane, chart);
    for (int32 tz = 0; tz < 2; ++tz)
    {
        for (int32 tx = 0; tx < 2; ++tx)
        {
            NavMeshTileData tile;
            std::string error;
            if (!DetourNavMap::BuildTile(settings, {chart.Id, tx, tz}, MakePlaneInput(local), tile, error) ||
                !navMap.AddTile(tile))
                return false;
        }
    }
    return navMap.GetTileCount() > 0;
}

} // anonymous namespace
} // namespace DetourTestHelpers

// ---------------------------------------------------------------------------
// Fixture: builds a navmesh once for tests that need it
// ---------------------------------------------------------------------------
class DetourNavMapBuiltTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Plane = DetourTestHelpers::BuildPlane();
        auto settings = DetourTestHelpers::MakeDetourSettings();
        ASSERT_TRUE(DetourTestHelpers::BuildAllTiles(m_NavMap, settings, m_Plane))
            << "Failed to build navmesh from test plane geometry ("
            << m_Plane.VertexCount << " verts, " << m_Plane.TriangleCount << " tris)";
    }

    DetourTestHelpers::PlaneGeometry m_Plane;
    DetourNavMap m_NavMap;
    PathBuffer m_PathBuffer;
};

// ===========================================================================
// Unbuilt / stub tests (existing)
// ===========================================================================

TEST(DetourNavMapTests, ConstructAndDestroy)
{
    DetourNavMap navMap;
    // Should not crash on destruction
}

TEST(DetourNavMapTests, IsBuiltReturnsFalseInitially)
{
    DetourNavMap navMap;
    EXPECT_FALSE(navMap.IsBuilt());
}

TEST(DetourNavMapTests, FindPathReturnsFailedWhenNotBuilt)
{
    DetourNavMap navMap;
    PathBuffer pathBuffer;
    PathHandle outPath;

    PathRequest request;
    request.StartX = 0.0f; request.StartZ = 0.0f;
    request.EndX = 5.0f; request.EndZ = 5.0f;

    PathStatus status = navMap.FindPath(request, pathBuffer, outPath);
    EXPECT_EQ(status, PathStatus::Failed);
}

TEST(DetourNavMapTests, IsPointNavigableReturnsFalseWhenNotBuilt)
{
    DetourNavMap navMap;
    EXPECT_FALSE(navMap.IsPointNavigable(1.0f, 0.0f, 1.0f, 0.5f));
}

TEST(DetourNavMapTests, GetClosestNavigablePointReturnsFalseWhenNotBuilt)
{
    DetourNavMap navMap;
    float32 outX = 0.0f, outY = 0.0f, outZ = 0.0f;
    EXPECT_FALSE(navMap.GetClosestNavigablePoint(1.0f, 0.0f, 1.0f, 5.0f, outX, outY, outZ));
}

TEST(DetourNavMapTests, RaycastReturnsFalseWhenNotBuilt)
{
    DetourNavMap navMap;
    float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    EXPECT_FALSE(navMap.Raycast(0.0f, 0.0f, 0.0f, 5.0f, 0.0f, 5.0f, hitX, hitY, hitZ));
}

TEST(DetourNavMapTests, GetDebugCountsReturnZeroWhenNotBuilt)
{
    DetourNavMap navMap;
    EXPECT_EQ(navMap.GetDebugVertexCount(), 0u);
    EXPECT_EQ(navMap.GetDebugTriangleCount(), 0u);
}

// ===========================================================================
// Build tests
// ===========================================================================

TEST(DetourNavMapTests, BuildWithValidGeometrySucceeds)
{
    DetourNavMap navMap;
    auto plane = DetourTestHelpers::BuildPlane();
    auto settings = DetourTestHelpers::MakeDetourSettings();
    EXPECT_TRUE(DetourTestHelpers::BuildAllTiles(navMap, settings, plane));
    EXPECT_TRUE(navMap.IsBuilt());
    EXPECT_EQ(navMap.GetTileCount(), 4u);
}

TEST(DetourNavMapTests, TileWithoutGeometryBuildsEmpty)
{
    const auto settings = DetourTestHelpers::MakeDetourSettings();
    NavMeshTileData tile;
    std::string error;
    EXPECT_TRUE(DetourNavMap::BuildTile(settings, {0, 1, 0}, DetourNavMap::InputGeometry{}, tile, error)) << error;
    EXPECT_TRUE(tile.Data.empty());
    EXPECT_EQ(tile.Key, (NavTileKey{0, 1, 0}));
}

TEST(DetourNavMapTests, BuildProducesDebugMesh)
{
    DetourNavMap navMap;
    auto plane = DetourTestHelpers::BuildPlane();
    auto settings = DetourTestHelpers::MakeDetourSettings();
    ASSERT_TRUE(DetourTestHelpers::BuildAllTiles(navMap, settings, plane));

    EXPECT_GT(navMap.GetDebugVertexCount(), 0u);
    EXPECT_GT(navMap.GetDebugTriangleCount(), 0u);
    EXPECT_NE(navMap.GetDebugVertices(), nullptr);
    EXPECT_NE(navMap.GetDebugIndices(), nullptr);
}

TEST(DetourNavMapTests, RebuildReplacesNavMesh)
{
    DetourNavMap navMap;
    auto plane = DetourTestHelpers::BuildPlane();
    auto settings = DetourTestHelpers::MakeDetourSettings();

    ASSERT_TRUE(DetourTestHelpers::BuildAllTiles(navMap, settings, plane));
    EXPECT_GT(navMap.GetDebugVertexCount(), 0u);

    // A coarser cell builds the same four tiles over a larger area.
    settings.CellSize = 0.5f;
    ASSERT_TRUE(DetourTestHelpers::BuildAllTiles(navMap, settings, plane));
    EXPECT_TRUE(navMap.IsBuilt());
    EXPECT_EQ(navMap.GetTileCount(), 4u);
    EXPECT_GT(navMap.GetDebugVertexCount(), 0u);
}

TEST(DetourNavMapTests, BuildTileAddTileRemoveTileRoundTrip)
{
    const auto settings = DetourTestHelpers::MakeDetourSettings();
    const auto chart = DetourTestHelpers::MakePlaneChart(settings);
    const auto plane = DetourTestHelpers::InChart(DetourTestHelpers::BuildPlane(), chart);
    DetourNavMap navMap;
    ASSERT_TRUE(navMap.Initialize(settings, chart));
    EXPECT_EQ(navMap.GetTileCount(), 0u);

    NavMeshTileData tile;
    std::string error;
    ASSERT_TRUE(DetourNavMap::BuildTile(settings, {chart.Id, 1, 1}, DetourTestHelpers::MakePlaneInput(plane), tile,
                                        error))
        << error;
    ASSERT_FALSE(tile.Data.empty());
    EXPECT_GT(tile.PolygonCount, 0u);

    ASSERT_TRUE(navMap.AddTile(tile));
    EXPECT_EQ(navMap.GetTileCount(), 1u);
    EXPECT_EQ(navMap.GetPolygonCount(), tile.PolygonCount);
    EXPECT_EQ(navMap.GetTileDataBytes(), tile.Data.size());

    // Adding the same coordinates again replaces the tile rather than stacking a second one.
    ASSERT_TRUE(navMap.AddTile(tile));
    EXPECT_EQ(navMap.GetTileCount(), 1u);

    EXPECT_TRUE(navMap.RemoveTile({chart.Id, 1, 1}));
    EXPECT_EQ(navMap.GetTileCount(), 0u);
    EXPECT_FALSE(navMap.RemoveTile({chart.Id, 1, 1}));
}

namespace
{

// Where a built tile's sections start, as Detour lays them out.
struct TileLayout
{
    dtMeshHeader Header;
    size_t Polys = 0;
    size_t DetailMeshes = 0;
    size_t DetailTris = 0;
    size_t BvTree = 0;
};

TileLayout LayoutOf(const std::vector<uint8>& data)
{
    TileLayout layout;
    std::memcpy(&layout.Header, data.data(), sizeof(dtMeshHeader));
    const dtMeshHeader& h = layout.Header;
    layout.Polys = dtAlign4(sizeof(dtMeshHeader)) + dtAlign4(sizeof(float) * 3 * h.vertCount);
    layout.DetailMeshes = layout.Polys + dtAlign4(sizeof(dtPoly) * h.polyCount) + dtAlign4(sizeof(dtLink) * h.maxLinkCount);
    layout.DetailTris = layout.DetailMeshes + dtAlign4(sizeof(dtPolyDetail) * h.detailMeshCount) +
                        dtAlign4(sizeof(float) * 3 * h.detailVertCount);
    layout.BvTree = layout.DetailTris + dtAlign4(4 * h.detailTriCount);
    return layout;
}

template <typename T, typename Change>
NavMeshTileData Patched(const NavMeshTileData& tile, size_t at, Change change)
{
    NavMeshTileData patched = tile;
    T value;
    std::memcpy(&value, patched.Data.data() + at, sizeof(T));
    change(value);
    std::memcpy(patched.Data.data() + at, &value, sizeof(T));
    return patched;
}

} // namespace

// Detour reads a tile's bytes as its header describes them, without bounds checks: AddTile refuses
// bytes whose header does not match them or the key, or whose indices leave their arrays, and keeps
// the tile it holds.
TEST(DetourNavMapTests, AddTileRefusesBytesDetourCannotHold)
{
    const auto settings = DetourTestHelpers::MakeDetourSettings();
    const auto chart = DetourTestHelpers::MakePlaneChart(settings);
    const auto plane = DetourTestHelpers::InChart(DetourTestHelpers::BuildPlane(), chart);
    NavMeshTileData tile;
    std::string error;
    ASSERT_TRUE(DetourNavMap::BuildTile(settings, {chart.Id, 1, 1}, DetourTestHelpers::MakePlaneInput(plane), tile,
                                        error))
        << error;
    const TileLayout layout = LayoutOf(tile.Data);
    ASSERT_GT(layout.Header.bvNodeCount, 0);
    ASSERT_GT(layout.Header.detailTriCount, 0);

    // The bytes of tile (1, 1) under the key of tile (0, 1), into an empty map.
    {
        DetourNavMap empty;
        ASSERT_TRUE(empty.Initialize(settings, chart));
        NavMeshTileData wrongKey = tile;
        wrongKey.Key = {chart.Id, 0, 1};
        std::string refusal;
        EXPECT_FALSE(empty.AddTile(wrongKey, &refusal));
        EXPECT_EQ(refusal, "the tile's coordinates are not its key's");
        EXPECT_EQ(empty.GetTileCount(), 0u);
    }

    DetourNavMap navMap;
    ASSERT_TRUE(navMap.Initialize(settings, chart));
    ASSERT_TRUE(navMap.AddTile(tile));
    const uint32 polygons = navMap.GetPolygonCount();
    const int32 polyCount = layout.Header.polyCount;
    const int32 vertCount = layout.Header.vertCount;
    const int32 detailVertCount = layout.Header.detailVertCount;
    const auto expectRefused = [&](const NavMeshTileData& bad, const char* reason) {
        std::string refusal;
        EXPECT_FALSE(navMap.AddTile(bad, &refusal)) << reason;
        EXPECT_EQ(refusal, reason);
    };

    NavMeshTileData headerOnly = tile;
    headerOnly.Data.resize(sizeof(dtMeshHeader));
    expectRefused(headerOnly, "the tile's sections do not add up to its size");
    expectRefused(Patched<dtMeshHeader>(tile, 0,
                                        [](dtMeshHeader& h) { h.polyCount = int(DetourNavMap::kMaxPolygonsPerTile) + 1; }),
                  "the tile's header does not describe a tile this engine builds");
    expectRefused(Patched<dtPoly>(tile, layout.Polys, [&](dtPoly& p) { p.verts[0] = uint16(vertCount); }),
                  "a polygon of the tile names a vertex it does not have");
    expectRefused(Patched<dtPoly>(tile, layout.Polys, [&](dtPoly& p) { p.neis[0] = uint16(polyCount + 1); }),
                  "a polygon of the tile names a neighbour it does not have");
    expectRefused(Patched<dtPolyDetail>(tile, layout.DetailMeshes,
                                        [&](dtPolyDetail& d) { d.vertBase = uint32(detailVertCount); d.vertCount = 1; }),
                  "a detail mesh of the tile lies outside its arrays");
    expectRefused(Patched<uint8>(tile, layout.DetailTris, [](uint8& corner) { corner = 255; }),
                  "a detail triangle of the tile names a vertex it does not have");
    expectRefused(Patched<dtBVNode>(tile, layout.BvTree, [&](dtBVNode& n) { n.i = polyCount; }),
                  "a bounding-volume node of the tile names a polygon it does not have");

    EXPECT_EQ(navMap.GetTileCount(), 1u);
    EXPECT_EQ(navMap.GetPolygonCount(), polygons);
}

TEST(DetourNavMapTests, InitializeRefusesARotatedChart)
{
    const auto settings = DetourTestHelpers::MakeDetourSettings();
    NavMeshChart chart = DetourTestHelpers::MakePlaneChart(settings);
    chart.Rotation = Mathematics::Quaternion(0.70710678f, 0.0f, 0.70710678f, 0.0f);
    DetourNavMap navMap;
    EXPECT_FALSE(navMap.Initialize(settings, chart));
    EXPECT_FALSE(navMap.IsBuilt());
}

// ===========================================================================
// Pathfinding tests (on built navmesh)
// ===========================================================================

// The boundary of a flat square is its rim, inset by the agent radius: no tile border and no edge
// between two polygons inside it is a boundary edge.
TEST_F(DetourNavMapBuiltTest, BoundaryEdgesOutlineOnlyTheRim)
{
    const uint32 edges = m_NavMap.GetDebugBoundaryEdgeCount();
    ASSERT_GT(edges, 0u);
    const float32* vertices = m_NavMap.GetDebugVertices();
    const uint32* indices = m_NavMap.GetDebugBoundaryIndices();
    ASSERT_NE(indices, nullptr);
    for (uint32 i = 0; i < edges * 2; ++i)
    {
        const float32* v = &vertices[static_cast<size_t>(indices[i]) * 3];
        EXPECT_GT(std::max(std::abs(v[0]), std::abs(v[2])), DetourTestHelpers::kPlaneHalf - 2.0f)
            << "boundary vertex inside the plane at (" << v[0] << ", " << v[2] << ")";
    }
}

TEST_F(DetourNavMapBuiltTest, FindPathOnFlatPlaneReturnsComplete)
{
    PathHandle outPath;
    PathRequest request;
    request.StartX = -5.0f; request.StartY = 0.0f; request.StartZ = -5.0f;
    request.EndX = 5.0f; request.EndY = 0.0f; request.EndZ = 5.0f;
    request.AgentRadius = 0.6f;
    request.AgentHeight = 2.0f;

    PathStatus status = m_NavMap.FindPath(request, m_PathBuffer, outPath);
    ASSERT_EQ(status, PathStatus::Complete);
    ASSERT_TRUE(outPath.IsValid());

    uint32 pointCount = m_PathBuffer.GetPointCount(outPath);
    ASSERT_GE(pointCount, 2u) << "Path should have at least start and end";

    // First waypoint should be near start
    PathPoint first = m_PathBuffer.GetPoint(outPath, 0);
    EXPECT_NEAR(first.X, -5.0f, 2.0f);
    EXPECT_NEAR(first.Z, -5.0f, 2.0f);

    // Last waypoint should be near end
    PathPoint last = m_PathBuffer.GetPoint(outPath, pointCount - 1);
    EXPECT_NEAR(last.X, 5.0f, 2.0f);
    EXPECT_NEAR(last.Z, 5.0f, 2.0f);
}

TEST_F(DetourNavMapBuiltTest, PathCrossesTileBordersComplete)
{
    // (-8, -8) and (8, 8) sit in opposite tiles of the 2 x 2 layout.
    PathHandle outPath;
    PathRequest request;
    request.StartX = -8.0f;
    request.StartZ = -8.0f;
    request.EndX = 8.0f;
    request.EndZ = 8.0f;
    request.AgentRadius = 0.6f;
    request.AgentHeight = 2.0f;
    EXPECT_EQ(m_NavMap.FindPath(request, m_PathBuffer, outPath), PathStatus::Complete);
}

TEST_F(DetourNavMapBuiltTest, RemovedTileLeavesItsNeighboursPathable)
{
    ASSERT_TRUE(m_NavMap.RemoveTile({0, 1, 1}));

    PathHandle outPath;
    PathRequest request;
    request.AgentRadius = 0.6f;
    request.AgentHeight = 2.0f;
    // Tile (0, 0) to tile (1, 0): both still present.
    request.StartX = -8.0f;
    request.StartZ = -8.0f;
    request.EndX = 8.0f;
    request.EndZ = -8.0f;
    EXPECT_EQ(m_NavMap.FindPath(request, m_PathBuffer, outPath), PathStatus::Complete);

    // A destination inside the removed tile has no polygon to land on.
    request.EndX = 8.0f;
    request.EndZ = 8.0f;
    EXPECT_EQ(m_NavMap.FindPath(request, m_PathBuffer, outPath), PathStatus::Failed);
}

TEST_F(DetourNavMapBuiltTest, FindPathSameStartAndEndProducesPath)
{
    PathHandle outPath;
    PathRequest request;
    request.StartX = 0.0f; request.StartY = 0.0f; request.StartZ = 0.0f;
    request.EndX = 0.0f; request.EndY = 0.0f; request.EndZ = 0.0f;
    request.AgentRadius = 0.6f;
    request.AgentHeight = 2.0f;

    PathStatus status = m_NavMap.FindPath(request, m_PathBuffer, outPath);
    EXPECT_NE(status, PathStatus::Failed);
}

TEST_F(DetourNavMapBuiltTest, FindPathToUnreachablePointFails)
{
    PathHandle outPath;
    PathRequest request;
    // Point far outside the navmesh bounds
    request.StartX = 0.0f; request.StartY = 0.0f; request.StartZ = 0.0f;
    request.EndX = 500.0f; request.EndY = 0.0f; request.EndZ = 500.0f;
    request.AgentRadius = 0.6f;
    request.AgentHeight = 2.0f;

    PathStatus status = m_NavMap.FindPath(request, m_PathBuffer, outPath);
    // Should fail or return partial — the end point is off the mesh
    EXPECT_NE(status, PathStatus::Complete);
}

// ===========================================================================
// Point query tests
// ===========================================================================

TEST_F(DetourNavMapBuiltTest, IsPointNavigableOnMeshReturnsTrue)
{
    // Center of the plane should be navigable
    EXPECT_TRUE(m_NavMap.IsPointNavigable(0.0f, 0.0f, 0.0f, 0.5f));
}

TEST_F(DetourNavMapBuiltTest, IsPointNavigableOffMeshReturnsFalse)
{
    // Way outside the mesh
    EXPECT_FALSE(m_NavMap.IsPointNavigable(100.0f, 0.0f, 100.0f, 0.5f));
}

TEST_F(DetourNavMapBuiltTest, GetClosestNavigablePointOnMesh)
{
    float32 outX = 0.0f, outY = 0.0f, outZ = 0.0f;
    bool found = m_NavMap.GetClosestNavigablePoint(3.0f, 0.0f, 3.0f, 5.0f, outX, outY, outZ);
    EXPECT_TRUE(found);
    // Should snap to a point on the navmesh near the query
    EXPECT_NEAR(outX, 3.0f, 1.5f);
    EXPECT_NEAR(outZ, 3.0f, 1.5f);
}

TEST_F(DetourNavMapBuiltTest, GetClosestNavigablePointFarOffMesh)
{
    float32 outX = 0.0f, outY = 0.0f, outZ = 0.0f;
    // Query point very far away with small search radius
    bool found = m_NavMap.GetClosestNavigablePoint(500.0f, 0.0f, 500.0f, 1.0f, outX, outY, outZ);
    EXPECT_FALSE(found);
}

// ===========================================================================
// Raycast tests
// ===========================================================================

TEST_F(DetourNavMapBuiltTest, RaycastOnOpenPlaneReturnsNoHit)
{
    float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    // Ray across the open plane — no obstacle, should reach end
    bool hit = m_NavMap.Raycast(-3.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f, hitX, hitY, hitZ);
    EXPECT_FALSE(hit) << "Open plane raycast should not hit anything";
}

TEST_F(DetourNavMapBuiltTest, RaycastToEdgeOfMeshHits)
{
    float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    // Ray from center to well beyond the mesh boundary
    bool hit = m_NavMap.Raycast(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 50.0f, hitX, hitY, hitZ);
    // Should hit the mesh boundary (ray leaves navigable area)
    EXPECT_TRUE(hit);
    // Hit point Z should be near the mesh edge (~9.4 after agent radius erosion)
    EXPECT_GT(hitZ, 5.0f);
    EXPECT_LT(hitZ, 11.0f);
}

// ===========================================================================
// CrowdManager tests
// ===========================================================================

TEST_F(DetourNavMapBuiltTest, CrowdManagerInitializesWithBuiltNavMesh)
{
    CrowdManager crowd;
    EXPECT_TRUE(crowd.Initialize(m_NavMap, 32));
    EXPECT_TRUE(crowd.IsInitialized());
    crowd.Shutdown();
    EXPECT_FALSE(crowd.IsInitialized());
}

TEST_F(DetourNavMapBuiltTest, CrowdManagerFailsWithUnbuiltNavMesh)
{
    DetourNavMap unbuilt;
    CrowdManager crowd;
    EXPECT_FALSE(crowd.Initialize(unbuilt, 32));
    EXPECT_FALSE(crowd.IsInitialized());
}

TEST_F(DetourNavMapBuiltTest, CrowdAgentAddRemoveLifecycle)
{
    CrowdManager crowd;
    ASSERT_TRUE(crowd.Initialize(m_NavMap, 32));

    CrowdAgentParams params;
    params.Radius = 0.5f;
    params.Height = 2.0f;
    params.MaxSpeed = 3.5f;
    params.MaxAcceleration = 8.0f;

    CrowdAgentHandle agent = crowd.AddAgent(0.0f, 0.0f, 0.0f, params);
    EXPECT_NE(agent, kInvalidCrowdAgent);
    EXPECT_TRUE(crowd.IsAgentActive(agent));

    crowd.RemoveAgent(agent);
    EXPECT_FALSE(crowd.IsAgentActive(agent));

    crowd.Shutdown();
}

TEST_F(DetourNavMapBuiltTest, CrowdAgentMovesTowardTarget)
{
    CrowdManager crowd;
    ASSERT_TRUE(crowd.Initialize(m_NavMap, 32));

    CrowdAgentParams params;
    params.Radius = 0.5f;
    params.Height = 2.0f;
    params.MaxSpeed = 5.0f;
    params.MaxAcceleration = 10.0f;

    CrowdAgentHandle agent = crowd.AddAgent(-3.0f, 0.0f, 0.0f, params);
    ASSERT_NE(agent, kInvalidCrowdAgent);

    float32 initX = 0.0f, initY = 0.0f, initZ = 0.0f;
    crowd.GetAgentPosition(agent, initX, initY, initZ);

    crowd.SetAgentTarget(agent, 5.0f, 0.0f, 0.0f);

    // 3 seconds at 60fps
    for (int i = 0; i < 180; ++i)
        crowd.Update(1.0f / 60.0f);

    float32 posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    crowd.GetAgentPosition(agent, posX, posY, posZ);
    EXPECT_TRUE(crowd.IsAgentActive(agent));

    // Agent should have moved significantly toward target at X=5
    EXPECT_GT(posX, 3.0f)
        << "Agent should have reached near target. Start=" << initX << " End=" << posX;

    crowd.Shutdown();
}

TEST_F(DetourNavMapBuiltTest, CrowdManagerSetAgentParamsUpdatesAgent)
{
    CrowdManager crowd;
    ASSERT_TRUE(crowd.Initialize(m_NavMap, 32));

    CrowdAgentParams params;
    params.Radius = 0.5f;
    params.Height = 2.0f;
    params.MaxSpeed = 3.5f;
    params.MaxAcceleration = 8.0f;

    CrowdAgentHandle agent = crowd.AddAgent(0.0f, 0.0f, 0.0f, params);
    ASSERT_NE(agent, kInvalidCrowdAgent);

    CrowdAgentParams newParams = params;
    newParams.Radius = 1.0f;
    newParams.MaxSpeed = 5.0f;
    newParams.AvoidanceQuality = ObstacleAvoidanceQuality::Low;

    crowd.SetAgentParams(agent, newParams);
    EXPECT_TRUE(crowd.IsAgentActive(agent));

    // Verify agent still functions after param update
    crowd.SetAgentTarget(agent, 3.0f, 0.0f, 0.0f);
    for (int i = 0; i < 60; ++i)
        crowd.Update(1.0f / 60.0f);

    float32 posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    crowd.GetAgentPosition(agent, posX, posY, posZ);
    EXPECT_GT(posX, 0.5f) << "Agent should move after SetAgentParams";

    crowd.Shutdown();
}

TEST_F(DetourNavMapBuiltTest, CrowdManagerDoubleInitialize)
{
    CrowdManager crowd;
    ASSERT_TRUE(crowd.Initialize(m_NavMap, 32));

    CrowdAgentParams params;
    params.Radius = 0.5f; params.Height = 2.0f;
    params.MaxSpeed = 5.0f; params.MaxAcceleration = 10.0f;

    CrowdAgentHandle firstAgent = crowd.AddAgent(0.0f, 0.0f, 0.0f, params);
    EXPECT_NE(firstAgent, kInvalidCrowdAgent);

    // Re-initialize (should not leak, should reset)
    ASSERT_TRUE(crowd.Initialize(m_NavMap, 16));
    EXPECT_TRUE(crowd.IsInitialized());

    // Old agent should be gone
    EXPECT_FALSE(crowd.IsAgentActive(firstAgent));

    // New agent on re-initialized crowd should work
    CrowdAgentHandle newAgent = crowd.AddAgent(-2.0f, 0.0f, 0.0f, params);
    ASSERT_NE(newAgent, kInvalidCrowdAgent);

    crowd.SetAgentTarget(newAgent, 4.0f, 0.0f, 0.0f);
    for (int i = 0; i < 120; ++i)
        crowd.Update(1.0f / 60.0f);

    float32 posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    crowd.GetAgentPosition(newAgent, posX, posY, posZ);
    EXPECT_GT(posX, 0.0f) << "Agent should move after double-init";

    crowd.Shutdown();
}

TEST_F(DetourNavMapBuiltTest, CrowdAgentVelocityAndTargetReadback)
{
    CrowdManager crowd;
    ASSERT_TRUE(crowd.Initialize(m_NavMap, 32));

    CrowdAgentParams params;
    params.Radius = 0.5f; params.Height = 2.0f;
    params.MaxSpeed = 5.0f; params.MaxAcceleration = 10.0f;

    CrowdAgentHandle agent = crowd.AddAgent(-3.0f, 0.0f, 0.0f, params);
    ASSERT_NE(agent, kInvalidCrowdAgent);

    crowd.SetAgentTarget(agent, 5.0f, 0.0f, 0.0f);

    // Read back target
    float32 tgtX = 0.0f, tgtY = 0.0f, tgtZ = 0.0f;
    crowd.GetAgentTarget(agent, tgtX, tgtY, tgtZ);
    EXPECT_NEAR(tgtX, 5.0f, 1.0f);

    // Simulate
    for (int i = 0; i < 60; ++i)
        crowd.Update(1.0f / 60.0f);

    // Check velocity is non-zero and in +X direction
    float32 vx = 0.0f, vy = 0.0f, vz = 0.0f;
    crowd.GetAgentVelocity(agent, vx, vy, vz);
    float32 speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    EXPECT_GT(speed, 0.1f) << "Agent should have non-zero velocity";
    EXPECT_GT(vx, 0.0f) << "Velocity should be in +X direction";

    crowd.Shutdown();
}

TEST_F(DetourNavMapBuiltTest, GetClosestNavigablePointRejectsAABBCorner)
{
    float32 outX = 0.0f, outY = 0.0f, outZ = 0.0f;

    // Query point far off navmesh corner -- XZ distance to nearest poly > searchRadius
    // even though each axis individually is within the AABB extent
    bool foundTight = m_NavMap.GetClosestNavigablePoint(14.0f, 0.0f, 14.0f, 5.0f,
                                                         outX, outY, outZ);
    EXPECT_FALSE(foundTight) << "Sphere check should reject AABB corner result";

    // Larger radius should succeed
    bool foundLarge = m_NavMap.GetClosestNavigablePoint(14.0f, 0.0f, 14.0f, 10.0f,
                                                         outX, outY, outZ);
    EXPECT_TRUE(foundLarge);
    EXPECT_GT(outX, 7.0f);
    EXPECT_GT(outZ, 7.0f);
}
