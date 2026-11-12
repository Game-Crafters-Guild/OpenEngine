#include "GridTestAccess.h"

#include "SpatialGrid/Grid.h"
#include "SpatialGrid/GridOverlap.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <random>
#include <span>
#include <vector>

namespace GameEngine::SpatialGrid
{

namespace
{

std::vector<GridMover> MakeMovers(uint32 count, uint32 seed)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float64> plane(0.0, 1024.0);
    std::uniform_real_distribution<float32> radius(0.1f, 30.0f);
    std::vector<GridMover> movers(count);
    for (uint32 i = 0; i < count; ++i)
        movers[i] = {{plane(random), plane(random) * 0.01, plane(random)}, radius(random), 1u, i};
    return movers;
}

constexpr uint32 kQueryKindCount = 6u;

// Runs query kind `kind` of Sphere, Cylinder, Box, Cone, Segment and Nearest around `center`.
uint32 RunQuery(const Grid& grid, uint32 kind, const Mathematics::WorldPosition& center, std::span<GridHit> out)
{
    const Mathematics::Vector3 direction = Mathematics::Vector3{1.0f, 0.0f, 1.0f}.NormalizeOr(Mathematics::Vector3{});
    switch (kind)
    {
    case 0u: return grid.Sphere(center, 40.0, GridFilter{}, out);
    case 1u: return grid.Cylinder(center, 40.0, 4.0, GridFilter{}, out);
    case 2u: return grid.Box(center, {40.0f, 4.0f, 20.0f}, GridFilter{}, out);
    case 3u: return grid.Cone(center, direction, 60.0, 0.6f, GridFilter{}, out);
    case 4u: return grid.Segment(center, {center.X + 80.0, center.Y, center.Z + 30.0}, 2.0, GridFilter{}, out);
    default: return grid.Nearest(center, 40.0, GridFilter{}, out);
    }
}

// (b) Movers given in another order give the same hits in the same order.
TEST(SpatialGridBuildTests, AnotherInputOrderGivesTheSameHits)
{
    std::vector<GridMover> movers = MakeMovers(6000, 9u);
    Grid first(16.0);
    first.Build(movers);
    std::shuffle(movers.begin(), movers.end(), std::mt19937(4u));
    Grid second(64.0);
    second.Build(movers);
    std::vector<GridHit> a(6000);
    std::vector<GridHit> b(6000);
    for (const float64 x : {100.0, 400.0, 900.0})
    {
        const Mathematics::WorldPosition center{x, 5.0, x};
        const uint32 count = first.Sphere(center, 40.0, GridFilter{}, a);
        ASSERT_EQ(second.Sphere(center, 40.0, GridFilter{}, b), count);
        ASSERT_GT(count, 0u);
        for (uint32 i = 0; i < count; ++i)
        {
            EXPECT_EQ(a[i].Id, b[i].Id);
            EXPECT_EQ(a[i].DistanceSquared, b[i].DistanceSquared);
        }
    }
}

// Every query kind gives the same id sequence for movers given in ascending id and in a shuffled order.
TEST(SpatialGridBuildTests, ShuffledInputGivesTheSameIdSequenceForEveryQueryKind)
{
    std::vector<GridMover> movers = MakeMovers(6000, 13u);
    Grid ordered(16.0);
    ordered.Build(movers);
    std::shuffle(movers.begin(), movers.end(), std::mt19937(5u));
    Grid shuffled(16.0);
    shuffled.Build(movers);
    std::vector<GridHit> a(6000);
    std::vector<GridHit> b(6000);
    for (uint32 kind = 0; kind < kQueryKindCount; ++kind)
    {
        for (const float64 x : {100.0, 400.0, 900.0})
        {
            SCOPED_TRACE(testing::Message() << "kind " << kind << " at " << x);
            const Mathematics::WorldPosition center{x, 5.0, x};
            const uint32 count = RunQuery(ordered, kind, center, a);
            ASSERT_EQ(RunQuery(shuffled, kind, center, b), count);
            ASSERT_GT(count, 1u);
            for (uint32 i = 0; i < count; ++i)
                ASSERT_EQ(a[i].Id, b[i].Id) << "hit " << i;
        }
    }
}

// Movers of one id in two levels, given in ascending id, are all found in ascending id. The far movers
// fill both levels, so that the query reads cells and does not scan a level.
TEST(SpatialGridBuildTests, EqualIdsInTwoLevelsAreFoundInAscendingId)
{
    std::vector<GridMover> movers = {
        {{2.0, 0.0, 2.0}, 0.5f, 1u, 1u},    {{20.0, 0.0, 2.0}, 0.5f, 1u, 5u},  {{4.0, 0.0, 4.0}, 12.0f, 1u, 5u},
        {{30.0, 0.0, 30.0}, 12.0f, 1u, 7u}, {{28.0, 0.0, 20.0}, 0.5f, 1u, 9u},
    };
    for (uint32 i = 0; i < 300u; ++i)
        movers.push_back({{5000.0 + 2.0 * i, 0.0, 5000.0}, 0.5f, 1u, 100u + i});
    for (uint32 i = 0; i < 40u; ++i)
        movers.push_back({{9000.0 + 30.0 * i, 0.0, 9000.0}, 12.0f, 1u, 400u + i});
    Grid grid(16.0);
    grid.Build(movers);
    std::vector<GridHit> out(movers.size());
    ASSERT_EQ(grid.Sphere({16.0, 0.0, 16.0}, 20.0, GridFilter{}, out), 5u);
    const std::vector<uint32> expected = {1u, 5u, 5u, 7u, 9u};
    for (size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(out[i].Id, expected[i]) << "hit " << i;
}

// The flight preset: 1,000 movers in a 100 km cube with cells of 8,192 m. A sphere of 5 km reads its cells,
// not the whole level: the farthest mover, moved into the sphere with no build, is not found.
TEST(SpatialGridBuildTests, AFlightCellSizeQueryReadsCellsAndMatchesAScan)
{
    std::mt19937 random(31u);
    std::uniform_real_distribution<float64> volume(0.0, 100000.0);
    std::vector<GridMover> movers(1000);
    for (uint32 i = 0; i < movers.size(); ++i)
        movers[i] = {{volume(random), volume(random), volume(random)}, 10.0f, 1u, i};
    Grid grid(8192.0);
    grid.Build(movers);
    constexpr float64 kRadius = 5000.0;
    const Mathematics::WorldPosition center = movers.front().Position;
    std::vector<GridHit> expected;
    for (const GridMover& mover : movers)
    {
        if (OverlapsSphere(center, kRadius, mover.Position, mover.Radius))
            expected.push_back({mover.Id, DistanceSquared(center, mover.Position)});
    }
    const auto nearer = [&center](const GridMover& a, const GridMover& b)
    { return DistanceSquared(center, a.Position) < DistanceSquared(center, b.Position); };
    const auto farthest = std::max_element(movers.begin(), movers.end(), nearer);
    GridTestAccess::MoveEntry(grid, farthest->Id, center);
    std::vector<GridHit> out(movers.size());
    ASSERT_EQ(grid.Sphere(center, kRadius, GridFilter{}, out), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(out[i].Id, expected[i].Id);
        EXPECT_EQ(out[i].DistanceSquared, expected[i].DistanceSquared);
    }
}

constexpr uint32 kWitnessId = 9000u;

// One level of `count` point movers in ascending id: `inQuery` first, then movers 3 m apart that no query of
// these tests reaches, then the witness in a cell that no query of these tests reads.
std::vector<GridMover> MakeLevelWithWitness(std::span<const Mathematics::WorldPosition> inQuery, uint32 count)
{
    std::vector<GridMover> movers;
    for (const Mathematics::WorldPosition& position : inQuery)
        movers.push_back({position, 0.0f, 1u, static_cast<uint32>(movers.size())});
    while (movers.size() + 1u < count)
    {
        const float64 x = 300.0 + 3.0 * static_cast<float64>(movers.size());
        movers.push_back({{x, 0.0, 0.0}, 0.0f, 1u, static_cast<uint32>(movers.size())});
    }
    movers.push_back({{1000.0, 1000.0, 1000.0}, 0.0f, 1u, kWitnessId});
    return movers;
}

std::vector<uint32> IdsOf(const std::vector<GridHit>& hits, uint32 count)
{
    std::vector<uint32> ids(count);
    for (uint32 i = 0; i < count; ++i)
        ids[i] = hits[i].Id;
    return ids;
}

// The ids that the sphere of 4 m at (16, 16, 16) finds in a level of `count` movers after the witness moved to
// its center with no build. The sphere covers 8 cells of 16 m. Before the move, the hits equal a scan.
std::vector<uint32> SphereIdsAfterTheWitnessMoved(uint32 count)
{
    const Mathematics::WorldPosition center{16.0, 16.0, 16.0};
    constexpr float64 kRadius = 4.0;
    const std::array<Mathematics::WorldPosition, 3> inSphere{{{14.0, 16.0, 16.0}, {16.0, 16.0, 16.0}, {18.0, 16.0, 16.0}}};
    const std::vector<GridMover> movers = MakeLevelWithWitness(inSphere, count);
    Grid grid(16.0);
    grid.Build(movers);
    std::vector<uint32> scan;
    for (const GridMover& mover : movers)
    {
        if (OverlapsSphere(center, kRadius, mover.Position, mover.Radius))
            scan.push_back(mover.Id);
    }
    std::vector<GridHit> out(movers.size());
    EXPECT_EQ(IdsOf(out, grid.Sphere(center, kRadius, GridFilter{}, out)), scan);
    GridTestAccess::MoveEntry(grid, kWitnessId, center);
    return IdsOf(out, grid.Sphere(center, kRadius, GridFilter{}, out));
}

// The ids, nearest the start first, that a sphere of 1 m swept from (8, 8, 8) to (40, 8, 8) finds in a level of
// `count` movers after the witness moved onto the segment with no build. The segment covers 3 cells of 16 m.
// Before the move, the hits equal a scan: the movers on the segment are in ascending id along it.
std::vector<uint32> SegmentIdsAfterTheWitnessMoved(uint32 count)
{
    const Mathematics::WorldPosition start{8.0, 8.0, 8.0};
    const Mathematics::WorldPosition end{40.0, 8.0, 8.0};
    constexpr float64 kRadius = 1.0;
    const std::array<Mathematics::WorldPosition, 3> onSegment{{{10.0, 8.0, 8.0}, {20.0, 8.5, 8.0}, {30.0, 8.0, 8.0}}};
    const std::vector<GridMover> movers = MakeLevelWithWitness(onSegment, count);
    Grid grid(16.0);
    grid.Build(movers);
    std::vector<uint32> scan;
    for (const GridMover& mover : movers)
    {
        float64 alongSquared = 0.0;
        if (OverlapsSegment(start, end, kRadius, mover.Position, mover.Radius, alongSquared))
            scan.push_back(mover.Id);
    }
    std::vector<GridHit> out(movers.size());
    EXPECT_EQ(IdsOf(out, grid.Segment(start, end, kRadius, GridFilter{}, out)), scan);
    GridTestAccess::MoveEntry(grid, kWitnessId, {25.0, 8.0, 8.0});
    return IdsOf(out, grid.Segment(start, end, kRadius, GridFilter{}, out));
}

// A level with four movers for each cell of the query: the query reads its cells. The witness is in the run of
// another cell, so the query does not find it.
TEST(SpatialGridBuildTests, AQueryReadsItsCellsInALevelWithFourMoversForEachCell)
{
    EXPECT_EQ(SphereIdsAfterTheWitnessMoved(32u), (std::vector<uint32>{0u, 1u, 2u}));
    EXPECT_EQ(SegmentIdsAfterTheWitnessMoved(12u), (std::vector<uint32>{0u, 1u, 2u}));
}

// One mover fewer: the query tests every mover of the level, and finds the witness at its place in the order.
TEST(SpatialGridBuildTests, AQueryTestsEveryMoverOfALevelWithFewerThanFourForEachCell)
{
    EXPECT_EQ(SphereIdsAfterTheWitnessMoved(31u), (std::vector<uint32>{0u, 1u, 2u, kWitnessId}));
    EXPECT_EQ(SegmentIdsAfterTheWitnessMoved(11u), (std::vector<uint32>{0u, 1u, kWitnessId, 2u}));
}

} // namespace
} // namespace GameEngine::SpatialGrid
