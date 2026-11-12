#include "GridTestAccess.h"

#include "SpatialGrid/Grid.h"
#include "SpatialGrid/GridOverlap.h"
#include "Memory/AllocationCountScope.h"
#include "Memory/ScratchScope.h"
#include "Types/Fnv1a.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>
#include <random>
#include <vector>

namespace GameEngine::SpatialGrid
{
namespace
{

using Mathematics::Vector3;
using Mathematics::WorldPosition;

constexpr uint32 kMoverCount = 5000;
constexpr uint32 kQueryCount = 200;

enum class Layout : uint8
{
    Uniform,
    Clustered,
    OneCell,
    OnEdge,
};

std::vector<GridMover> MakeMovers(Layout layout, float64 offset, uint32 seed)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float64> plane(0.0, 256.0);
    std::uniform_real_distribution<float64> height(0.0, 8.0);
    std::uniform_real_distribution<float64> cluster(0.0, 12.0);
    std::uniform_real_distribution<float64> inCell(0.0, 15.9);
    std::uniform_int_distribution<int32> edge(0, 16);
    std::vector<GridMover> movers(kMoverCount);
    for (uint32 i = 0; i < kMoverCount; ++i)
    {
        WorldPosition p;
        switch (layout)
        {
        case Layout::Uniform: p = {plane(random), height(random), plane(random)}; break;
        case Layout::Clustered:
            p = (i % 2 == 0) ? WorldPosition{cluster(random), height(random), cluster(random)}
                             : WorldPosition{plane(random), height(random), plane(random)};
            break;
        case Layout::OneCell: p = {inCell(random), inCell(random), inCell(random)}; break;
        case Layout::OnEdge: p = {16.0 * edge(random), 0.0, 16.0 * edge(random)}; break;
        }
        // One mover in twenty is large, so the upper levels and their widening take part.
        const float32 radius = (i % 20 == 0) ? 12.0f : 0.5f;
        movers[i] = {{p.X + offset, p.Y, p.Z + offset}, radius, (i % 3 == 0) ? 2u : 1u, i * 7u + 3u};
    }
    return movers;
}

template <typename Test>
std::vector<GridHit> Scan(const std::vector<GridMover>& movers, const GridFilter& filter, bool byDistance, Test&& test)
{
    std::vector<GridHit> hits;
    for (const GridMover& mover : movers)
    {
        float64 key = 0.0;
        if ((mover.Mask & filter.Mask) != 0u && mover.Id != filter.IgnoreId && test(mover, key))
            hits.push_back({mover.Id, key});
    }
    std::sort(hits.begin(), hits.end(),
              [byDistance](const GridHit& a, const GridHit& b)
              {
                  if (byDistance && a.DistanceSquared != b.DistanceSquared)
                      return a.DistanceSquared < b.DistanceSquared;
                  return a.Id < b.Id;
              });
    return hits;
}

void ExpectSame(const std::vector<GridHit>& expected, uint32 matched, const std::vector<GridHit>& out)
{
    ASSERT_EQ(matched, expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        ASSERT_EQ(out[i].Id, expected[i].Id) << "hit " << i;
        ASSERT_EQ(out[i].DistanceSquared, expected[i].DistanceSquared) << "hit " << i;
    }
}

void CheckEveryQuery(Layout layout, float64 offset)
{
    const std::vector<GridMover> movers = MakeMovers(layout, offset, 17u);
    Grid grid(16.0);
    grid.Build(movers);
    std::mt19937 random(91u);
    std::uniform_real_distribution<float64> plane(-8.0, 264.0);
    std::uniform_real_distribution<float64> size(0.5, 24.0);
    std::uniform_real_distribution<float32> axis(-1.0f, 1.0f);
    std::vector<GridHit> out(kMoverCount);
    for (uint32 q = 0; q < kQueryCount; ++q)
    {
        const WorldPosition c{plane(random) + offset, 2.0, plane(random) + offset};
        const WorldPosition e{plane(random) + offset, 4.0, plane(random) + offset};
        const float64 r = size(random);
        const GridFilter filter{(q % 4 == 0) ? 2u : 3u, movers[q].Id};
        Vector3 direction{axis(random), axis(random) * 0.2f, axis(random)};
        direction = direction.NormalizeOr(Vector3{1.0f, 0.0f, 0.0f});
        const Vector3 half{static_cast<float32>(r), 3.0f, static_cast<float32>(r) * 0.5f};
        const float32 angle = 0.6f;
        const float64 cosAngle = std::cos(static_cast<float64>(angle));
        const float64 sinAngle = std::sin(static_cast<float64>(angle));
        SCOPED_TRACE(testing::Message() << "query " << q);

        ExpectSame(Scan(movers, filter, false, [&](const GridMover& m, float64& key)
                        { key = DistanceSquared(c, m.Position); return OverlapsSphere(c, r, m.Position, m.Radius); }),
                   grid.Sphere(c, r, filter, out), out);
        ExpectSame(Scan(movers, filter, false, [&](const GridMover& m, float64& key)
                        { key = DistanceSquared(c, m.Position); return OverlapsCylinder(c, r, 3.0, m.Position, m.Radius); }),
                   grid.Cylinder(c, r, 3.0, filter, out), out);
        ExpectSame(Scan(movers, filter, false, [&](const GridMover& m, float64& key)
                        { key = DistanceSquared(c, m.Position); return OverlapsBox(c, half, m.Position, m.Radius); }),
                   grid.Box(c, half, filter, out), out);
        ExpectSame(Scan(movers, filter, false, [&](const GridMover& m, float64& key)
                        {
                            key = DistanceSquared(c, m.Position);
                            return OverlapsCone(c, direction, r * 2.0, cosAngle, sinAngle, m.Position, m.Radius);
                        }),
                   grid.Cone(c, direction, r * 2.0, angle, filter, out), out);
        ExpectSame(Scan(movers, filter, true, [&](const GridMover& m, float64& key)
                        { return OverlapsSegment(c, e, 1.0, m.Position, m.Radius, key); }),
                   grid.Segment(c, e, 1.0, filter, out), out);
        ExpectSame(Scan(movers, filter, true, [&](const GridMover& m, float64& key)
                        { key = DistanceSquared(c, m.Position); return OverlapsSphere(c, r, m.Position, m.Radius); }),
                   grid.Nearest(c, r, filter, out), out);
        EXPECT_EQ(grid.Count(c, r, filter), grid.Sphere(c, r, filter, out));
        if (testing::Test::HasFatalFailure())
            return;
    }
}

// (a) The cell path finds what a scan finds, in the same order, for every query and layout, near and far.
TEST(SpatialGridQueryTests, EveryQueryMatchesAScanForEveryLayoutAndDistance)
{
    for (const float64 offset : {0.0, 300000.0, 1.0e9})
    {
        for (const Layout layout : {Layout::Uniform, Layout::Clustered, Layout::OneCell, Layout::OnEdge})
        {
            SCOPED_TRACE(testing::Message() << "offset " << offset << " layout " << static_cast<int>(layout));
            CheckEveryQuery(layout, offset);
            if (HasFatalFailure())
                return;
        }
    }
}

// Meters from p to the solid cone, from the cone's definition in angles: the points in `range` of the apex
// whose angle to the axis is at most `halfAngle`.
float64 DistanceToSolidCone(const WorldPosition& apex, const Vector3& axis, float64 range, float64 halfAngle,
                            const WorldPosition& p)
{
    const float64 vx = p.X - apex.X;
    const float64 vy = p.Y - apex.Y;
    const float64 vz = p.Z - apex.Z;
    const float64 s = vx * axis.x + vy * axis.y + vz * axis.z;
    const float64 d = std::hypot(vx - s * axis.x, vy - s * axis.y, vz - s * axis.z);
    const float64 distance = std::hypot(s, d);
    const float64 angle = std::atan2(d, s);
    if (angle <= halfAngle)
        return std::max(0.0, distance - range);
    const float64 outside = angle - halfAngle;
    if (outside >= std::numbers::pi / 2.0)
        return distance;
    if (distance * std::cos(outside) <= range)
        return distance * std::sin(outside);
    return std::hypot(s - range * std::cos(halfAngle), d - range * std::sin(halfAngle));
}

// A narrow cone returns no mover behind its apex, and every cone returns the movers in reach of the solid cone.
TEST(SpatialGridQueryTests, AConeFindsTheMoversThatTouchTheSolidCone)
{
    Grid behind(16.0);
    const std::vector<GridMover> one{{{0.0, 0.0, -10.0}, 1.0f, 1u, 1u}};
    behind.Build(one);
    std::vector<GridHit> out(1);
    EXPECT_EQ(behind.Cone({0.0, 0.0, 0.0}, Vector3{0.0f, 0.0f, 1.0f}, 20.0, 0.0873f, GridFilter{}, out), 0u);

    std::mt19937 random(23u);
    std::uniform_real_distribution<float64> unit(0.0, 1.0);
    std::uniform_real_distribution<float32> axis(-1.0f, 1.0f);
    constexpr uint32 kConeCount = 200;
    constexpr uint32 kMoversPerCone = 400;
    for (uint32 q = 0; q < kConeCount; ++q)
    {
        const WorldPosition apex{unit(random) * 64.0, unit(random) * 8.0, unit(random) * 64.0};
        const Vector3 direction =
            Vector3{axis(random), axis(random), axis(random)}.NormalizeOr(Vector3{1.0f, 0.0f, 0.0f});
        const float32 halfAngle = static_cast<float32>((1.0 + unit(random) * 88.0) * std::numbers::pi / 180.0);
        const float64 range = 1.0 + unit(random) * 40.0;
        const Vector3 other = std::abs(direction.y) < 0.9f ? Vector3{0.0f, 1.0f, 0.0f} : Vector3{1.0f, 0.0f, 0.0f};
        const Vector3 side = Vector3::Cross(direction, other).NormalizeOr(Vector3{1.0f, 0.0f, 0.0f});
        const Vector3 up = Vector3::Cross(direction, side);
        std::vector<GridMover> movers;
        std::set<uint32> expected;
        for (uint32 i = 0; i < kMoversPerCone; ++i)
        {
            // Four kinds in turn: in front of the apex, beside a side, behind the apex and at the range's cap.
            const float64 angle = (i % 4 == 0)   ? unit(random) * halfAngle
                                  : (i % 4 == 1) ? halfAngle + (unit(random) - 0.5) * 0.4
                                  : (i % 4 == 2) ? std::numbers::pi * (0.5 + unit(random) * 0.5)
                                                 : unit(random) * (halfAngle + 0.3);
            const float64 distance = (i % 4 == 3) ? range * (0.9 + unit(random) * 0.3) : unit(random) * range * 1.2;
            const float64 around = unit(random) * 2.0 * std::numbers::pi;
            const float64 a = distance * std::cos(angle);
            const float64 b = distance * std::sin(angle) * std::cos(around);
            const float64 c = distance * std::sin(angle) * std::sin(around);
            const WorldPosition p{apex.X + a * direction.x + b * side.x + c * up.x,
                                  apex.Y + a * direction.y + b * side.y + c * up.y,
                                  apex.Z + a * direction.z + b * side.z + c * up.z};
            const float32 radius = static_cast<float32>(0.05 + unit(random) * range * 0.3);
            const float64 gap = DistanceToSolidCone(apex, direction, range, halfAngle, p) - radius;
            // A mover within a micrometer of the surface has no exact answer in floating point.
            if (std::abs(gap) < 1.0e-6)
                continue;
            const uint32 id = static_cast<uint32>(movers.size());
            movers.push_back({p, radius, 1u, id});
            if (gap <= 0.0)
                expected.insert(id);
        }
        Grid grid(16.0);
        grid.Build(movers);
        out.resize(movers.size());
        const uint32 found = grid.Cone(apex, direction, range, halfAngle, GridFilter{}, out);
        SCOPED_TRACE(testing::Message() << "cone " << q << " half angle " << halfAngle << " range " << range);
        ASSERT_EQ(found, expected.size());
        ASSERT_TRUE(std::equal(expected.begin(), expected.end(), out.begin(),
                               [](uint32 id, const GridHit& hit) { return id == hit.Id; }));
    }
}

// (c) A mover that crosses a sector edge is found from both sides at its exact distance.
TEST(SpatialGridQueryTests, AMoverAcrossASectorEdgeIsFoundFromBothSides)
{
    for (const float64 edge : {512.0, 512.0 + 1024.0 * 293.0})
    {
        for (const float64 x : {edge - 0.01, edge + 0.01})
        {
            std::vector<GridMover> movers = MakeMovers(Layout::Uniform, edge - 128.0, 5u);
            movers[1] = {{x, 0.0, edge}, 0.5f, 1u, 1u};
            Grid grid(16.0);
            grid.Build(movers);
            for (const float64 side : {-6.0, 6.0})
            {
                const WorldPosition center{edge + side, 0.0, edge};
                std::vector<GridHit> out(kMoverCount);
                const uint32 matched = grid.Sphere(center, 7.0, GridFilter{}, out);
                const auto hit = std::find_if(out.begin(), out.begin() + matched,
                                              [](const GridHit& h) { return h.Id == 1u; });
                ASSERT_NE(hit, out.begin() + matched) << "x " << x << " side " << side;
                EXPECT_EQ(hit->DistanceSquared, (x - center.X) * (x - center.X));
            }
        }
    }
}

// (d) A query finds a large mover whose center is in a cell outside the query's own reach.
TEST(SpatialGridQueryTests, AQueryFindsMoversOfEveryLevel)
{
    std::vector<GridMover> movers;
    for (uint32 i = 0; i < 400; ++i)
    {
        movers.push_back({{5000.0 + i * 3.0, 0.0, 5000.0}, 0.5f, 1u, 100u + i});
        movers.push_back({{-5000.0 - i * 3.0, 0.0, -5000.0}, 20.0f, 1u, 1000u + i});
    }
    movers.push_back({{274.0, 0.0, 100.0}, 20.0f, 1u, 1u});
    movers.push_back({{244.6, 0.0, 100.0}, 0.5f, 1u, 2u});
    Grid grid(16.0);
    grid.Build(movers);
    std::vector<GridHit> out(8);
    ASSERT_EQ(grid.Sphere({250.0, 0.0, 100.0}, 5.0, GridFilter{}, out), 2u);
    EXPECT_EQ(out[0].Id, 1u);
    EXPECT_EQ(out[1].Id, 2u);
}

// (e) A span smaller than the matches receives the first hits in order, and the return value is the full count.
TEST(SpatialGridQueryTests, AShortSpanReceivesTheFirstHitsAndTheFullCount)
{
    const std::vector<GridMover> movers = MakeMovers(Layout::OneCell, 0.0, 3u);
    Grid grid(16.0);
    grid.Build(movers);
    const WorldPosition center{8.0, 8.0, 8.0};
    std::vector<GridHit> all(kMoverCount);
    const uint32 listed = grid.Sphere(center, 6.0, GridFilter{}, all);
    const uint32 nearest = grid.Nearest(center, 6.0, GridFilter{}, all);
    ASSERT_GT(listed, 8u);
    std::vector<GridHit> fullList(kMoverCount);
    grid.Sphere(center, 6.0, GridFilter{}, fullList);
    std::vector<GridHit> shortOut(8);
    EXPECT_EQ(grid.Sphere(center, 6.0, GridFilter{}, shortOut), listed);
    for (size_t i = 0; i < shortOut.size(); ++i)
        EXPECT_EQ(shortOut[i].Id, fullList[i].Id);
    EXPECT_EQ(grid.Nearest(center, 6.0, GridFilter{}, shortOut), nearest);
    for (size_t i = 0; i < shortOut.size(); ++i)
        EXPECT_EQ(shortOut[i].Id, all[i].Id);
}

// (f) A query makes no heap allocation once the thread's scratch has warmed.
TEST(SpatialGridQueryTests, AQueryMakesNoHeapAllocation)
{
    const std::vector<GridMover> movers = MakeMovers(Layout::Uniform, 0.0, 11u);
    Grid grid(16.0);
    grid.Build(movers);
    std::vector<GridHit> out(64);
    const WorldPosition center{128.0, 2.0, 128.0};
    const WorldPosition end{200.0, 2.0, 40.0};
    grid.Sphere(center, 300.0, GridFilter{}, out);
    Memory::AllocationCountScope scope(Memory::CountWindow::ThisThread);
    grid.Sphere(center, 10.0, GridFilter{}, out);
    grid.Sphere(center, 300.0, GridFilter{}, out);
    grid.Cylinder(center, 10.0, 2.0, GridFilter{}, out);
    grid.Box(center, Vector3{10.0f, 2.0f, 10.0f}, GridFilter{}, out);
    grid.Cone(center, Vector3{1.0f, 0.0f, 0.0f}, 30.0, 0.5f, GridFilter{}, out);
    grid.Segment(center, end, 1.0, GridFilter{}, out);
    grid.Nearest(center, 20.0, GridFilter{}, std::span<GridHit>(out).first(8));
    grid.Count(center, 20.0, GridFilter{});
    EXPECT_EQ(scope.Count(), 0u);
}

// A query that reports every mover of both levels fills its scratch to the mover count and loses no hit.
TEST(SpatialGridQueryTests, AQueryThatReportsEveryMoverIsComplete)
{
    const std::vector<GridMover> movers = MakeMovers(Layout::OneCell, 0.0, 5u);
    Grid grid(16.0);
    grid.Build(movers);
    const WorldPosition center{8.0, 8.0, 8.0};
    std::vector<GridHit> out(kMoverCount);
    const std::vector<GridHit> expected = Scan(movers, GridFilter{}, false, [&](const GridMover& m, float64& key)
                                               { key = DistanceSquared(center, m.Position); return OverlapsSphere(center, 40.0, m.Position, m.Radius); });
    ASSERT_EQ(expected.size(), kMoverCount);
    ExpectSame(expected, grid.Sphere(center, 40.0, GridFilter{}, out), out);
}

// Movers of radius 0.1 at x = id + 0.5 on one line: a box over x in [0, k] finds ids 0 to k - 1.
std::vector<GridMover> MakeLine(uint32 count)
{
    std::vector<GridMover> movers(count);
    for (uint32 i = 0; i < count; ++i)
        movers[i] = {{i + 0.5, 0.5, 0.5}, 0.1f, 1u, i};
    return movers;
}

uint32 BoxOverFirst(const Grid& grid, uint32 hitCount, std::span<GridHit> out)
{
    const float32 half = static_cast<float32>(hitCount) * 0.5f;
    return grid.Box({half, 0.5, 0.5}, Vector3{half, 0.5f, 0.5f}, GridFilter{}, out);
}

// The hit scratch starts at 64 hits and doubles: hit counts on both sides of each step are complete and in order.
TEST(SpatialGridQueryTests, AQueryWhoseHitsCrossEachScratchStepIsComplete)
{
    const std::vector<GridMover> movers = MakeLine(5000);
    Grid grid(16.0);
    grid.Build(movers);
    std::vector<GridHit> out(movers.size());
    for (const uint32 hitCount : {1u, 64u, 65u, 128u, 129u, 256u, 257u, 512u, 513u, 1024u, 1025u, 2048u, 2049u, 4096u,
                                  4097u, 5000u})
    {
        ASSERT_EQ(BoxOverFirst(grid, hitCount, out), hitCount);
        for (uint32 i = 0; i < hitCount; ++i)
            ASSERT_EQ(out[i].Id, i) << "hit " << i << " of " << hitCount;
    }
}

// A query inside a scratch scope of the caller loses no hit and leaves the caller's take unchanged.
TEST(SpatialGridQueryTests, AQueryInsideAnOpenScratchScopeIsComplete)
{
    const std::vector<GridMover> movers = MakeLine(1000);
    Grid grid(16.0);
    grid.Build(movers);
    Memory::ScratchScope outer;
    const std::span<GridHit> out = outer.AllocateArray<GridHit>(movers.size());
    const std::span<uint32> marks = outer.AllocateArray<uint32>(16);
    std::fill(marks.begin(), marks.end(), 7u);
    ASSERT_EQ(BoxOverFirst(grid, 300u, out), 300u);
    for (uint32 i = 0; i < 300u; ++i)
        ASSERT_EQ(out[i].Id, i) << "hit " << i;
    EXPECT_TRUE(std::all_of(marks.begin(), marks.end(), [](uint32 mark) { return mark == 7u; }));
}

// A query takes hit slots for its own hits, not for every mover of the grid, and doubles them when they are full.
TEST(SpatialGridQueryTests, AQueryTakesHitSlotsForItsOwnHits)
{
#if GE_DEBUG_INSTRUMENTATION
    const std::vector<GridMover> movers = MakeLine(20000);
    Grid grid(16.0);
    grid.Build(movers);
    std::vector<GridHit> out(8);
    for (const auto& [hitCount, slots] : {std::pair{3u, 64u}, std::pair{65u, 128u}, std::pair{129u, 256u}})
    {
        ASSERT_EQ(BoxOverFirst(grid, hitCount, out), hitCount);
        EXPECT_EQ(GridTestAccess::LastQueryHitCapacity(), slots) << hitCount << " hits";
    }
    ASSERT_EQ(grid.Sphere({-100.0, 0.5, 0.5}, 2.0, GridFilter{}, out), 0u);
    EXPECT_EQ(GridTestAccess::LastQueryHitCapacity(), 0u);
#else
    GTEST_SKIP() << "The hit capacity counter exists only when GE_DEBUG_INSTRUMENTATION is 1 (Debug, DebugFast).";
#endif
}

// Segment walks the cells along the segment: over random segments, of every direction and length, with movers
// of every level, it finds the same hits in the same order as a scan.
TEST(SpatialGridQueryTests, ASegmentFindsTheSameHitsAsAScan)
{
    std::mt19937 random(21u);
    std::uniform_real_distribution<float64> plane(0.0, 1024.0);
    std::uniform_real_distribution<float64> height(0.0, 64.0);
    std::uniform_real_distribution<float32> large(1.0f, 20.0f);
    std::vector<GridMover> movers(20000);
    for (uint32 i = 0; i < movers.size(); ++i)
        movers[i] = {{plane(random), height(random), plane(random)}, i % 10u == 0u ? large(random) : 0.5f, 1u, i};
    Grid grid(16.0);
    grid.Build(movers);
    std::uniform_real_distribution<float64> reach(-300.0, 300.0);
    std::uniform_real_distribution<float64> radius(0.0, 4.0);
    std::vector<GridHit> out(movers.size());
    for (uint32 query = 0; query < 200u; ++query)
    {
        const WorldPosition start{plane(random), height(random), plane(random)};
        const float64 scale = query % 4u == 0u ? 0.01 : 1.0;
        const WorldPosition end{start.X + reach(random) * scale, start.Y + reach(random) * 0.1 * scale,
                                start.Z + reach(random) * scale};
        const float64 r = radius(random);
        ExpectSame(Scan(movers, GridFilter{}, true, [&](const GridMover& m, float64& key)
                        { return OverlapsSegment(start, end, r, m.Position, m.Radius, key); }),
                   grid.Segment(start, end, r, GridFilter{}, out), out);
        if (testing::Test::HasFatalFailure())
            return;
    }
}

// Two cells 1,024 cells apart on one axis have the same tag. A query that spans 1,024 cells or more scans
// the level, so a mover whose cell shares a bucket with such a cell is found once.
TEST(SpatialGridQueryTests, AQueryOverAThousandAndTwentyFourCellsFindsEachMoverOnce)
{
    // 6,000 movers give a level 0 table of 16,384 buckets. The bucket rule is HashOf in Grid.cpp.
    constexpr uint64 kBucketMask = 16383u;
    const auto bucket = [](int64 x) { return Hashing::SplitMix64(static_cast<uint64>(x) & ((uint64{1} << 21) - 1)) & kBucketMask; };
    int64 first = 0;
    while (bucket(first) != bucket(first + 1024))
        ++first;
    std::vector<GridMover> movers;
    for (uint32 i = 0; i < 6000u; ++i)
        movers.push_back({{static_cast<float64>(first) + 0.125 + 0.25 * i, 0.5, 0.5}, 0.0f, 1u, i});
    Grid grid(1.0);
    grid.Build(movers);
    const WorldPosition center{static_cast<float64>(first) + 520.5, 0.5, 0.5};
    const Vector3 half{520.25f, 0.25f, 0.25f};
    std::vector<GridHit> out(movers.size());
    const uint32 matched = grid.Box(center, half, GridFilter{}, out);
    ExpectSame(Scan(movers, GridFilter{}, false, [&](const GridMover& m, float64& key)
                    { key = DistanceSquared(center, m.Position); return OverlapsBox(center, half, m.Position, m.Radius); }),
               matched, out);
    std::set<uint32> ids;
    for (uint32 i = 0; i < matched; ++i)
        EXPECT_TRUE(ids.insert(out[i].Id).second) << "mover " << out[i].Id << " found twice";
}

// Runs `query` on a grid with one mover at the origin. Debug and DebugFast stop at the domain assert;
// other configurations return no hit.
template <typename Query>
void ExpectRefused(Query&& query)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Grid grid(16.0);
    const std::vector<GridMover> one{{{0.0, 0.0, 0.0}, 1.0f, 1u, 1u}};
    grid.Build(one);
    std::vector<GridHit> out(1);
    [[maybe_unused]] uint32 found = 1u;
    EXPECT_DEBUG_DEATH(found = query(grid, out), "outside its domain");
#if defined(NDEBUG)
    EXPECT_EQ(found, 0u);
#endif
}

TEST(SpatialGridQueryDeathTest, AConeWithANonUnitDirectionIsRefused)
{
    ExpectRefused([](const Grid& grid, std::vector<GridHit>& out)
                  { return grid.Cone({0.0, 0.0, -1.0}, Vector3{0.0f, 0.0f, 2.0f}, 10.0, 0.5f, GridFilter{}, out); });
}

TEST(SpatialGridQueryDeathTest, AConeWithAHalfAngleOfZeroIsRefused)
{
    ExpectRefused([](const Grid& grid, std::vector<GridHit>& out)
                  { return grid.Cone({0.0, 0.0, -1.0}, Vector3{0.0f, 0.0f, 1.0f}, 10.0, 0.0f, GridFilter{}, out); });
}

TEST(SpatialGridQueryDeathTest, AConeWithAHalfAngleAboveARightAngleIsRefused)
{
    ExpectRefused([](const Grid& grid, std::vector<GridHit>& out)
                  { return grid.Cone({0.0, 0.0, -1.0}, Vector3{0.0f, 0.0f, 1.0f}, 10.0, 1.6f, GridFilter{}, out); });
}

TEST(SpatialGridQueryDeathTest, AQueryWithANaNRadiusIsRefused)
{
    ExpectRefused([](const Grid& grid, std::vector<GridHit>& out)
                  { return grid.Sphere({}, std::numeric_limits<float64>::quiet_NaN(), GridFilter{}, out); });
}

TEST(SpatialGridQueryDeathTest, AQueryWithANegativeRadiusIsRefused)
{
    ExpectRefused([](const Grid& grid, std::vector<GridHit>&) { return grid.Count({0.0, 0.0, 3.0}, -1.0, GridFilter{}); });
}

TEST(SpatialGridQueryDeathTest, ASphereWithANegativeRadiusIsRefused)
{
    ExpectRefused([](const Grid& grid, std::vector<GridHit>& out)
                  { return grid.Sphere({0.0, 0.0, 3.0}, -1.0, GridFilter{}, out); });
}

// A half angle of pi/2 in float32 is a half space: the cone finds a mover on the plane and none behind it.
TEST(SpatialGridQueryTests, AConeOfARightAngleIsAHalfSpace)
{
    Grid grid(16.0);
    const std::vector<GridMover> movers{{{5.0, 0.0, 0.0}, 0.5f, 1u, 1u}, {{5.0, 0.0, -0.6}, 0.5f, 1u, 2u}};
    grid.Build(movers);
    std::vector<GridHit> out(2);
    const float32 rightAngle = std::numbers::pi_v<float32> / 2.0f;
    ASSERT_EQ(grid.Cone({0.0, 0.0, 0.0}, Vector3{0.0f, 0.0f, 1.0f}, 10.0, rightAngle, GridFilter{}, out), 1u);
    EXPECT_EQ(out[0].Id, 1u);
}

} // namespace
} // namespace GameEngine::SpatialGrid
