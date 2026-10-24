#include <gtest/gtest.h>
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/NavSurfaceFile.h"

#include <cstring>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

namespace
{

// A 40 m flat quad under tiles (0, 0) to (1, 1) of a 0.5 m-cell chart, built into real Detour
// tiles so the round trip carries bytes a mesh accepts.
NavSurfaceData MakeSurface()
{
    NavMeshSettings settings;
    NavMeshChart chart;
    chart.Origin = ChartOrigin::AtWorld(102400.5, 12.0, -2048.25);
    chart.Span = static_cast<float32>(kNavChartTilesPerSide) * DetourNavMap::GetTileSize(settings);
    const std::vector<float32> vertices = {-8.0f, 0.0f, -8.0f, 72.0f, 0.0f, -8.0f, -8.0f, 0.0f, 72.0f, 72.0f, 0.0f, 72.0f};
    const std::vector<uint32> indices = {0, 2, 1, 1, 2, 3};
    DetourNavMap::InputGeometry input;
    input.Vertices = vertices.data();
    input.VertexCount = 4;
    input.Indices = indices.data();
    input.TriangleCount = 2;

    NavSurfaceData data;
    data.BakeSettingsHash = 0x1234'5678'9ABC'DEF0ull;
    data.Areas = DefaultNavSurfaceAreas();
    data.FlagNames = DefaultNavSurfaceFlagNames();
    NavSurfaceProfile& profile = data.Profiles.emplace_back();
    profile.Radius = settings.AgentRadius;
    profile.Height = settings.AgentHeight;
    profile.MaxSlope = settings.MaxSlope;
    profile.StepHeight = settings.StepHeight;
    profile.SettingsHash = 0xFEDC'BA98'7654'3210ull;
    NavSurfaceChart& surfaceChart = profile.Charts.emplace_back();
    surfaceChart.Chart = chart;
    for (int32 z = 1; z >= 0; --z)
    {
        for (int32 x = 1; x >= 0; --x)
        {
            NavMeshTileData built;
            std::string error;
            EXPECT_TRUE(DetourNavMap::BuildTile(settings, {chart.Id, x, z}, input, built, error)) << error;
            NavSurfaceTile& tile = surfaceChart.Tiles.emplace_back();
            tile.X = x;
            tile.Z = z;
            tile.PolygonCount = built.PolygonCount;
            tile.ContentHash = 0xA000u + static_cast<uint64>(z * 2 + x);
            tile.Data = built.Data;
        }
    }
    return data;
}

// Where the writer puts the fields the hostile-input tests change (the packed layout in
// NavSurfaceFile.h).
constexpr size_t kProfileTableOffsetAt = 21;
constexpr size_t kProfileChartTableOffsetAt = 33;
constexpr size_t kProfileBytes = 41;
constexpr size_t kChartRecordBytes = 68;
constexpr size_t kChartTileCountAt = 48;
constexpr size_t kChartIndexOffsetAt = 52;
constexpr size_t kChartDataOffsetAt = 60;
constexpr size_t kIndexEntryBytes = 32;
constexpr size_t kIndexOffsetAt = 8;
constexpr size_t kIndexSizeAt = 16;

template <typename T>
T ReadAt(const std::vector<uint8>& bytes, size_t at)
{
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

template <typename T>
void WriteAt(std::vector<uint8>& bytes, size_t at, T value)
{
    std::memcpy(bytes.data() + at, &value, sizeof(T));
}

// The byte offset of chart `c`'s record in the first profile's chart table.
size_t ChartRecordAt(const std::vector<uint8>& bytes, size_t c)
{
    const auto profileTable = ReadAt<uint64>(bytes, kProfileTableOffsetAt);
    return static_cast<size_t>(ReadAt<uint64>(bytes, profileTable + kProfileChartTableOffsetAt)) + c * kChartRecordBytes;
}

bool Reads(const std::vector<uint8>& bytes)
{
    NavSurfaceData read;
    std::string error;
    return ReadNavSurface(bytes, read, error);
}

const NavSurfaceTile* FindTile(const NavSurfaceChart& chart, int32 x, int32 z)
{
    for (const NavSurfaceTile& tile : chart.Tiles)
        if (tile.X == x && tile.Z == z)
            return &tile;
    return nullptr;
}

} // namespace

TEST(NavSurfaceFileTests, RoundTripRestoresEveryTileAndTheTables)
{
    const NavSurfaceData written = MakeSurface();
    std::vector<uint8> bytes;
    ASSERT_TRUE(WriteNavSurface(written, bytes));

    NavSurfaceData read;
    std::string error;
    ASSERT_TRUE(ReadNavSurface(bytes, read, error)) << error;

    EXPECT_EQ(read.BakeSettingsHash, written.BakeSettingsHash);
    EXPECT_EQ(read.TileCells, kNavMeshTileCells);
    EXPECT_EQ(read.Lifecycle, NavSurfaceLifecycle::Cooked);
    ASSERT_EQ(read.Areas.size(), written.Areas.size());
    for (size_t i = 0; i < read.Areas.size(); ++i)
    {
        EXPECT_EQ(read.Areas[i].Id, written.Areas[i].Id);
        EXPECT_EQ(read.Areas[i].Name, written.Areas[i].Name);
        EXPECT_EQ(read.Areas[i].DefaultCost, written.Areas[i].DefaultCost);
        EXPECT_EQ(read.Areas[i].DefaultFlags, written.Areas[i].DefaultFlags);
    }
    EXPECT_EQ(read.FlagNames, written.FlagNames);
    ASSERT_EQ(read.Profiles.size(), 1u);
    const NavSurfaceProfile& profile = read.Profiles[0];
    EXPECT_EQ(profile.SettingsHash, written.Profiles[0].SettingsHash);
    EXPECT_EQ(profile.Radius, written.Profiles[0].Radius);
    EXPECT_EQ(profile.StepHeight, written.Profiles[0].StepHeight);
    ASSERT_EQ(profile.Charts.size(), 1u);
    EXPECT_EQ(profile.Charts[0].Chart, written.Profiles[0].Charts[0].Chart);

    const NavSurfaceChart& writtenChart = written.Profiles[0].Charts[0];
    ASSERT_EQ(profile.Charts[0].Tiles.size(), writtenChart.Tiles.size());
    for (const NavSurfaceTile& original : writtenChart.Tiles)
    {
        const NavSurfaceTile* restored = FindTile(profile.Charts[0], original.X, original.Z);
        ASSERT_NE(restored, nullptr) << original.X << ", " << original.Z;
        EXPECT_EQ(restored->Data, original.Data);
        EXPECT_EQ(restored->PolygonCount, original.PolygonCount);
        EXPECT_EQ(restored->ContentHash, original.ContentHash);
    }
    // The index is in Morton order: (0, 0), (1, 0), (0, 1), (1, 1).
    EXPECT_EQ(profile.Charts[0].Tiles.front().X, 0);
    EXPECT_EQ(profile.Charts[0].Tiles.front().Z, 0);
    EXPECT_EQ(profile.Charts[0].Tiles.back().X, 1);
    EXPECT_EQ(profile.Charts[0].Tiles.back().Z, 1);

    // The restored bytes are tiles a mesh takes as they are.
    DetourNavMap map;
    ASSERT_TRUE(map.Initialize(NavMeshSettings{}, profile.Charts[0].Chart));
    for (const NavSurfaceTile& tile : profile.Charts[0].Tiles)
        EXPECT_TRUE(map.AddTile({{0, tile.X, tile.Z}, tile.Data, tile.PolygonCount, tile.ContentHash}));
    EXPECT_EQ(map.GetTileCount(), 4u);
}

TEST(NavSurfaceFileTests, AnotherFormatVersionIsRefused)
{
    std::vector<uint8> bytes;
    ASSERT_TRUE(WriteNavSurface(MakeSurface(), bytes));
    const uint32 otherVersion = kNavSurfaceFormatVersion + 1;
    std::memcpy(bytes.data() + 4, &otherVersion, sizeof(otherVersion));

    NavSurfaceData read;
    std::string error;
    EXPECT_FALSE(ReadNavSurface(bytes, read, error));
    EXPECT_EQ(error, "the file is format version 2 and this engine reads version 1: bake the surface again");
    EXPECT_TRUE(read.Profiles.empty());
}

TEST(NavSurfaceFileTests, ATruncatedFileIsRefusedAtEveryLength)
{
    std::vector<uint8> bytes;
    ASSERT_TRUE(WriteNavSurface(MakeSurface(), bytes));
    ASSERT_TRUE(Reads(bytes));
    for (size_t length = 0; length < bytes.size(); ++length)
        EXPECT_FALSE(Reads({bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length)})) << length;
}

TEST(NavSurfaceFileTests, ATileLongerThanTheFileIsRefused)
{
    std::vector<uint8> bytes;
    ASSERT_TRUE(WriteNavSurface(MakeSurface(), bytes));
    const auto index = static_cast<size_t>(ReadAt<uint64>(bytes, ChartRecordAt(bytes, 0) + kChartIndexOffsetAt));
    const size_t last = index + 3 * kIndexEntryBytes;
    WriteAt<uint32>(bytes, last + kIndexSizeAt, ReadAt<uint32>(bytes, last + kIndexSizeAt) + 1);
    EXPECT_FALSE(Reads(bytes));
}

// Two index entries naming one byte range would read the bytes twice.
TEST(NavSurfaceFileTests, TilesSharingOneRangeAreRefused)
{
    std::vector<uint8> bytes;
    ASSERT_TRUE(WriteNavSurface(MakeSurface(), bytes));
    const auto index = static_cast<size_t>(ReadAt<uint64>(bytes, ChartRecordAt(bytes, 0) + kChartIndexOffsetAt));
    WriteAt<uint64>(bytes, index + kIndexEntryBytes + kIndexOffsetAt, 0);
    WriteAt<uint32>(bytes, index + kIndexEntryBytes + kIndexSizeAt, ReadAt<uint32>(bytes, index + kIndexSizeAt));
    EXPECT_FALSE(Reads(bytes));
}

// Two charts naming one index and one tile block would read them twice.
TEST(NavSurfaceFileTests, ChartsSharingOneIndexAreRefused)
{
    NavSurfaceData data = MakeSurface();
    data.Profiles[0].Charts.push_back(data.Profiles[0].Charts[0]);
    data.Profiles[0].Charts[1].Chart.Id = 1;
    std::vector<uint8> bytes;
    ASSERT_TRUE(WriteNavSurface(data, bytes));
    ASSERT_TRUE(Reads(bytes));
    const size_t first = ChartRecordAt(bytes, 0);
    const size_t second = ChartRecordAt(bytes, 1);
    WriteAt<uint64>(bytes, second + kChartIndexOffsetAt, ReadAt<uint64>(bytes, first + kChartIndexOffsetAt));
    WriteAt<uint64>(bytes, second + kChartDataOffsetAt, ReadAt<uint64>(bytes, first + kChartDataOffsetAt));
    EXPECT_FALSE(Reads(bytes));
}

TEST(NavSurfaceFileTests, ATileCountPastTheIndexIsRefused)
{
    std::vector<uint8> bytes;
    ASSERT_TRUE(WriteNavSurface(MakeSurface(), bytes));
    WriteAt<uint32>(bytes, ChartRecordAt(bytes, 0) + kChartTileCountAt, 0xFFFFFFFFu);
    EXPECT_FALSE(Reads(bytes));
}

// A profile with no chart holds no tile: the file reads, and a surface that loads it bakes.
TEST(NavSurfaceFileTests, AProfileWithNoChartReads)
{
    NavSurfaceData data = MakeSurface();
    data.Profiles[0].Charts.clear();
    std::vector<uint8> bytes;
    ASSERT_TRUE(WriteNavSurface(data, bytes));
    NavSurfaceData read;
    std::string error;
    ASSERT_TRUE(ReadNavSurface(bytes, read, error)) << error;
    ASSERT_EQ(read.Profiles.size(), 1u);
    EXPECT_TRUE(read.Profiles[0].Charts.empty());
}

// Two profiles naming one chart table would read its charts twice, however few bytes they take.
TEST(NavSurfaceFileTests, ProfilesSharingOneChartTableAreRefused)
{
    NavSurfaceData data = MakeSurface();
    NavSurfaceProfile& second = data.Profiles.emplace_back(data.Profiles[0]);
    second.Id = 1;
    for (NavSurfaceChart& chart : second.Charts)
        chart.Tiles.clear();
    data.Profiles[0].Charts[0].Tiles.clear();
    std::vector<uint8> bytes;
    ASSERT_TRUE(WriteNavSurface(data, bytes));
    ASSERT_TRUE(Reads(bytes));
    const auto profileTable = static_cast<size_t>(ReadAt<uint64>(bytes, kProfileTableOffsetAt));
    WriteAt<uint64>(bytes, profileTable + kProfileBytes + kProfileChartTableOffsetAt,
                    ReadAt<uint64>(bytes, profileTable + kProfileChartTableOffsetAt));
    EXPECT_FALSE(Reads(bytes));
}
