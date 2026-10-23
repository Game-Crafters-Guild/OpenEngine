#include <gtest/gtest.h>
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/ObstacleAvoidance.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

namespace
{

// A 256 m patch of smooth hills on a 1 m lattice (slopes under 26 degrees), cut into 8 x 8 tiles of
// 32 m at a 0.5 m cell for a 0.5 m, 1.8 m agent: the patch the far-origin measurement used.
constexpr int32 kSamples = 257;
constexpr int32 kTilesPerSide = 8;
constexpr float32 kCrowdStep = 1.0f / 60.0f;
constexpr int32 kCrowdSteps = 600;
constexpr float32 kCrowdSpeed = 1.5f;

float32 HillHeight(float32 x, float32 z)
{
    return 30.0f + 4.0f * std::sin(x * 0.045f) * std::cos(z * 0.037f) + 1.5f * std::sin((x + z) * 0.11f);
}

NavMeshSettings PatchSettings()
{
    NavMeshSettings settings;
    settings.CellSize = 0.5f;
    settings.AgentRadius = 0.5f;
    settings.AgentHeight = 1.8f;
    settings.MaxSlope = 45.0f;
    settings.StepHeight = 0.4f;
    return settings;
}

struct Triangles
{
    std::vector<float32> Vertices;
    std::vector<uint32> Indices;

    DetourNavMap::InputGeometry Input() const
    {
        DetourNavMap::InputGeometry input;
        input.Vertices = Vertices.data();
        input.VertexCount = static_cast<uint32>(Vertices.size() / 3);
        input.Indices = Indices.data();
        input.TriangleCount = static_cast<uint32>(Indices.size() / 3);
        return input;
    }
};

// The lattice quads under tile `key` and its border, placed in the world at `offset` metres on X
// and Z as fp32 world coordinates (what a terrain hands the bake), then moved into `chart`'s frame.
Triangles TileInput(const NavMeshSettings& settings, const NavMeshChart& chart, float32 offset, NavTileKey key)
{
    float32 minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
    DetourNavMap::GetTileBounds(settings, key.X, key.Z, minX, minZ, maxX, maxZ);
    const float32 border = DetourNavMap::GetTileBorder(settings);
    const int32 x0 = std::max(0, static_cast<int32>(std::floor(minX - border)));
    const int32 z0 = std::max(0, static_cast<int32>(std::floor(minZ - border)));
    const int32 x1 = std::min(kSamples - 1, static_cast<int32>(std::ceil(maxX + border)));
    const int32 z1 = std::min(kSamples - 1, static_cast<int32>(std::ceil(maxZ + border)));
    const int32 width = x1 - x0 + 1;
    Triangles out;
    for (int32 z = z0; z <= z1; ++z)
    {
        for (int32 x = x0; x <= x1; ++x)
        {
            const float32 world[3] = {offset + static_cast<float32>(x), HillHeight(static_cast<float32>(x), static_cast<float32>(z)),
                                      offset + static_cast<float32>(z)};
            float32 local[3];
            chart.ToChart(world[0], world[1], world[2], local);
            out.Vertices.insert(out.Vertices.end(), local, local + 3);
        }
    }
    for (int32 z = 0; z + 1 <= z1 - z0; ++z)
    {
        for (int32 x = 0; x + 1 < width; ++x)
        {
            const uint32 a = static_cast<uint32>(z * width + x);
            const uint32 c = a + static_cast<uint32>(width);
            out.Indices.insert(out.Indices.end(), {a, c, a + 1, a + 1, c, c + 1});
        }
    }
    return out;
}

NavMeshChart ChartAt(float32 offset)
{
    NavMeshChart chart;
    chart.Origin = ChartOrigin::AtWorld(offset, 0.0, offset);
    chart.Span = static_cast<float32>(kNavChartTilesPerSide) * DetourNavMap::GetTileSize(PatchSettings());
    return chart;
}

// Builds the patch at `offset` into `map` and returns every tile's bytes in row order.
std::vector<std::vector<uint8>> BuildPatch(float32 offset, DetourNavMap& map)
{
    const NavMeshSettings settings = PatchSettings();
    const NavMeshChart chart = ChartAt(offset);
    std::vector<std::vector<uint8>> bytes;
    EXPECT_TRUE(map.Initialize(settings, chart));
    for (int32 z = 0; z < kTilesPerSide; ++z)
    {
        for (int32 x = 0; x < kTilesPerSide; ++x)
        {
            const Triangles input = TileInput(settings, chart, offset, {chart.Id, x, z});
            NavMeshTileData tile;
            std::string error;
            EXPECT_TRUE(DetourNavMap::BuildTile(settings, {chart.Id, x, z}, input.Input(), tile, error)) << error;
            EXPECT_TRUE(map.AddTile(tile));
            bytes.push_back(tile.Data);
        }
    }
    return bytes;
}

// The XZ distance a crowd agent covers in 600 steps of 1/60 s at 1.5 m/s, heading from the patch's
// corner (8, 8) towards the far corner, on the patch built at `offset`.
float64 CrowdDistance(float32 offset, DetourNavMap& map)
{
    CrowdManager crowd;
    EXPECT_TRUE(crowd.Initialize(map, 4));
    CrowdAgentParams params;
    params.Radius = 0.5f;
    params.Height = 1.8f;
    params.MaxSpeed = kCrowdSpeed;
    params.MaxAcceleration = 1000.0f;
    const CrowdAgentHandle agent = crowd.AddAgent(offset + 8.0f, HillHeight(8.0f, 8.0f), offset + 8.0f, params);
    EXPECT_NE(agent, kInvalidCrowdAgent);
    crowd.SetAgentTarget(agent, offset + 248.0f, HillHeight(248.0f, 248.0f), offset + 248.0f);
    // The path request is served inside the first updates; the walk is timed from the first step
    // the agent moves.
    float32 startX = 0.0f, startY = 0.0f, startZ = 0.0f;
    crowd.GetAgentPosition(agent, startX, startY, startZ);
    int32 steps = 0;
    for (int32 i = 0; i < 100 && steps == 0; ++i)
    {
        crowd.Update(kCrowdStep);
        float32 vx = 0.0f, vy = 0.0f, vz = 0.0f;
        crowd.GetAgentVelocity(agent, vx, vy, vz);
        if (vx * vx + vz * vz > 0.0f)
            steps = 1;
    }
    for (; steps < kCrowdSteps; ++steps)
        crowd.Update(kCrowdStep);
    float32 endX = 0.0f, endY = 0.0f, endZ = 0.0f;
    crowd.GetAgentPosition(agent, endX, endY, endZ);
    const float64 dx = static_cast<float64>(endX) - startX;
    const float64 dz = static_cast<float64>(endZ) - startZ;
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

// A chart keeps Detour at its own origin, so the same ground built 100 km out gives the same tile
// bytes as at the world origin, and a crowd agent walks the same distance at both: its steps of
// 17.7 mm are far above the fp32 spacing near the chart's origin.
TEST(DetourNavMapChartTests, FarChartBuildsTheSameTilesAndTheCrowdWalksTheSameDistance)
{
    DetourNavMap nearMap;
    DetourNavMap farMap;
    const auto nearBytes = BuildPatch(0.0f, nearMap);
    const auto farBytes = BuildPatch(100000.0f, farMap);
    ASSERT_EQ(nearBytes.size(), farBytes.size());
    for (size_t i = 0; i < nearBytes.size(); ++i)
        EXPECT_EQ(nearBytes[i], farBytes[i]) << "tile " << i;
    ASSERT_GT(nearMap.GetPolygonCount(), 0u);

    const float64 expected = kCrowdSpeed * kCrowdStep * kCrowdSteps;
    const float64 nearDistance = CrowdDistance(0.0f, nearMap);
    const float64 farDistance = CrowdDistance(100000.0f, farMap);
    RecordProperty("CrowdDistanceAtOrigin", std::to_string(nearDistance));
    RecordProperty("CrowdDistanceAt100km", std::to_string(farDistance));
    EXPECT_NEAR(nearDistance, expected, 0.1);
    // The far agent's position reads back as fp32 world metres, 7.8 mm apart at 100 km.
    EXPECT_NEAR(farDistance, nearDistance, 0.03);
}

TEST(DetourNavMapChartTests, BuildTileTakesANegativeTileCoordinate)
{
    const NavMeshSettings settings = PatchSettings();
    const NavMeshChart chart = ChartAt(0.0f);
    // Ground under tile (-1, -2): chart-local X in [-32, 0), Z in [-64, -32), as a flat quad
    // with the border.
    const NavTileKey key{chart.Id, -1, -2};
    Triangles input;
    input.Vertices = {-40.0f, 0.0f, -72.0f, 8.0f, 0.0f, -72.0f, -40.0f, 0.0f, -24.0f, 8.0f, 0.0f, -24.0f};
    input.Indices = {0, 2, 1, 1, 2, 3};
    NavMeshTileData tile;
    std::string error;
    ASSERT_TRUE(DetourNavMap::BuildTile(settings, key, input.Input(), tile, error)) << error;
    ASSERT_FALSE(tile.Data.empty());
    EXPECT_EQ(tile.Key, key);

    DetourNavMap map;
    ASSERT_TRUE(map.Initialize(settings, chart));
    ASSERT_TRUE(map.AddTile(tile));
    EXPECT_EQ(map.GetTileCount(), 1u);
    EXPECT_TRUE(map.IsPointNavigable(-16.0f, 0.0f, -48.0f, 0.5f));
    EXPECT_FALSE(map.IsPointNavigable(16.0f, 0.0f, 48.0f, 0.5f));
    EXPECT_TRUE(map.RemoveTile(key));
    EXPECT_EQ(map.GetTileCount(), 0u);
}

// Detour's 10 salt bits repeat after 1,023 reuses of a pool slot. One tile swapped 1,100 times
// moves through the pool, so a polygon reference taken before the first swap is refused after
// every swap and never resolves to a polygon of a later tile.
TEST(DetourNavMapChartTests, AReferenceHeldAcrossTheSaltWrapIsRefused)
{
    const NavMeshSettings settings = PatchSettings();
    const NavMeshChart chart = ChartAt(0.0f);
    const NavTileKey key{chart.Id, 1, 1};
    const Triangles input = TileInput(settings, chart, 0.0f, key);
    NavMeshTileData tile;
    std::string error;
    ASSERT_TRUE(DetourNavMap::BuildTile(settings, key, input.Input(), tile, error)) << error;
    ASSERT_FALSE(tile.Data.empty());

    DetourNavMap map;
    ASSERT_TRUE(map.Initialize(settings, chart));
    ASSERT_TRUE(map.AddTile(tile));
    const auto* navMesh = static_cast<const dtNavMesh*>(map.GetDetourNavMesh());
    const auto* query = static_cast<const dtNavMeshQuery*>(map.GetDetourNavMeshQuery());
    dtQueryFilter filter;
    const float32 centre[3] = {48.0f, HillHeight(48.0f, 48.0f), 48.0f};
    const float32 extent[3] = {2.0f, 4.0f, 2.0f};
    dtPolyRef held = 0;
    float32 nearest[3];
    ASSERT_TRUE(dtStatusSucceed(query->findNearestPoly(centre, extent, &filter, &held, nearest)));
    ASSERT_NE(held, 0u);

    for (int32 swap = 1; swap <= 1100; ++swap)
    {
        ASSERT_TRUE(map.AddTile(tile));
        const dtMeshTile* heldTile = nullptr;
        const dtPoly* heldPoly = nullptr;
        ASSERT_FALSE(navMesh->isValidPolyRef(held)) << "swap " << swap;
        ASSERT_TRUE(dtStatusFailed(navMesh->getTileAndPolyByRef(held, &heldTile, &heldPoly))) << "swap " << swap;
    }
    EXPECT_EQ(map.GetTileCount(), 1u);
}
