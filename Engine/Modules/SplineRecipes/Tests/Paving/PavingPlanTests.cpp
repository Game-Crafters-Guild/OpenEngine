#include "SplineRecipes/Paving/PavingPlan.h"

#include "Mathematics/Geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace GameEngine::SplineRecipes
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector2;
using Mathematics::Vector3;

constexpr float32 kRoadHalfWidth = 2.0f;
constexpr float32 kSquareHalfSize = 10.0f;

// A straight road from `from` to `to` on the ground, sampled every meter.
PavingRoadCurve StraightRoad(uint32 id, const Vector3& from, const Vector3& to)
{
    PavingRoadCurve road;
    road.Road = ECS::EntityHandle{id, 1};
    road.Length = (to - from).Length();
    const uint32 intervals = static_cast<uint32>(std::ceil(road.Length));
    for (uint32 i = 0; i <= intervals; ++i)
    {
        const float32 t = static_cast<float32>(i) / static_cast<float32>(intervals);
        road.Points.push_back(from + (to - from) * t);
        road.ArcLengths.push_back(road.Length * t);
        road.HalfWidths.push_back(kRoadHalfWidth);
    }
    return road;
}

// A ring road around the origin, counter-clockwise from above, sampled every meter.
PavingRoadCurve RingRoad(uint32 id, float32 radius)
{
    PavingRoadCurve road;
    road.Road = ECS::EntityHandle{id, 1};
    road.Closed = true;
    road.Length = 2.0f * 3.14159265f * radius;
    const uint32 intervals = static_cast<uint32>(std::ceil(road.Length));
    for (uint32 i = 0; i <= intervals; ++i)
    {
        const float32 t = static_cast<float32>(i) / static_cast<float32>(intervals);
        const float32 angle = 2.0f * 3.14159265f * t;
        road.Points.push_back(Vector3(radius * std::cos(angle), 0.0f, radius * std::sin(angle)));
        road.ArcLengths.push_back(road.Length * t);
        road.HalfWidths.push_back(kRoadHalfWidth);
    }
    return road;
}

// A closed road through the corners `corners` on the ground, sampled every meter from the first corner.
PavingRoadCurve LoopRoad(uint32 id, std::span<const Vector2> corners)
{
    PavingRoadCurve road;
    road.Road = ECS::EntityHandle{id, 1};
    road.Closed = true;
    for (size_t c = 0; c < corners.size(); ++c)
    {
        const Vector2 from = corners[c];
        const Vector2 to = corners[(c + 1) % corners.size()];
        const float32 length = (to - from).Length();
        const uint32 intervals = static_cast<uint32>(std::ceil(length));
        for (uint32 i = c == 0 ? 0u : 1u; i <= intervals; ++i)
        {
            const Vector2 point = from + (to - from) * (static_cast<float32>(i) / static_cast<float32>(intervals));
            road.Points.push_back(Vector3(point.x, 0.0f, point.y));
            road.ArcLengths.push_back(road.Length + length * static_cast<float32>(i) / static_cast<float32>(intervals));
            road.HalfWidths.push_back(kRoadHalfWidth);
        }
        road.Length += length;
    }
    return road;
}

SG::AreaRing Square(float32 halfSize)
{
    return {Vector2(-halfSize, -halfSize), Vector2(halfSize, -halfSize), Vector2(halfSize, halfSize),
            Vector2(-halfSize, halfSize)};
}

// Both arms of `road` at the junction `nodeArc` along it.
void AddBothArms(PavingSource& source, const PavingRoadCurve& road, float32 nodeArc)
{
    const Vector3 node = road.Points.front() + (road.Points.back() - road.Points.front()) * (nodeArc / road.Length);
    source.Arms.push_back(BuildPavedArm(road, nodeArc, +1, Vector2(node.x, node.z)));
    source.Arms.push_back(BuildPavedArm(road, nodeArc, -1, Vector2(node.x, node.z)));
}

// A square with Mill Lane entering from the west and Ford Road from the north, each ending inside.
PavingSource MarketSquare(const PavingRoadCurve& millLane, const PavingRoadCurve& fordRoad)
{
    PavingSource square;
    square.Entity = ECS::EntityHandle{1, 1};
    square.Rank.LengthMeters = 8.0f * kSquareHalfSize;
    square.Rank.FirstPoint = Vector2(-kSquareHalfSize, -kSquareHalfSize);
    square.Outline = Square(kSquareHalfSize);
    AddBothArms(square, millLane, 30.0f);
    AddBothArms(square, fordRoad, 30.0f);
    return square;
}

// Square meters the region's pieces pave.
float32 PavedArea(const SG::PavedRegion& region)
{
    float32 area = 0.0f;
    for (const SG::AreaPiece& piece : region.Pieces)
    {
        area += 0.5f * std::fabs(Mathematics::PolygonDoubledSignedArea(piece.Outer));
        for (const SG::AreaRing& hole : piece.Holes)
            area -= 0.5f * std::fabs(Mathematics::PolygonDoubledSignedArea(hole));
    }
    return area;
}

// Whether a piece of the region paves `point`.
bool Paves(const SG::PavedRegion& region, const Vector2& point)
{
    for (const SG::AreaPiece& piece : region.Pieces)
    {
        if (!Mathematics::PointInPolygon(point, piece.Outer))
            continue;
        if (std::none_of(piece.Holes.begin(), piece.Holes.end(),
                         [&point](const SG::AreaRing& hole) { return Mathematics::PointInPolygon(point, hole); }))
            return true;
    }
    return false;
}

size_t OpenArmCount(const PavingPlan& plan)
{
    return static_cast<size_t>(std::count_if(plan.Footprint.Arms.begin(), plan.Footprint.Arms.end(),
                                             [](const SG::PavedArmStop& stop) { return stop.Open; }));
}

TEST(PavingPlanTests, ASquareWithTwoEnteringRoadsIsOneRegionTheSquareOwns)
{
    const PavingRoadCurve millLane = StraightRoad(2, Vector3(-40.0f, 0.0f, 0.0f), Vector3(-5.0f, 0.0f, 0.0f));
    const PavingRoadCurve fordRoad = StraightRoad(3, Vector3(0.0f, 0.0f, 40.0f), Vector3(0.0f, 0.0f, 5.0f));
    const PavingSource sources[] = {MarketSquare(millLane, fordRoad)};

    const PavingPlan plan = PlanPavings(sources);

    ASSERT_EQ(plan.Footprint.Regions.size(), 1u);
    EXPECT_EQ(plan.RegionOwners[0], 0u);
    EXPECT_EQ(plan.Roles[0], PavingRole::Owns);
    EXPECT_EQ(plan.Footprint.Regions[0].Outlines.size(), 1u);
    // The two approaches go on outside the square: each is cut by one Open edge across it.
    EXPECT_EQ(OpenArmCount(plan), 2u);
    for (const SG::PavedArmStop& stop : plan.Footprint.Arms)
    {
        if (!stop.Open)
            continue;
        EXPECT_EQ(stop.Region, 0u);
        EXPECT_NEAR((stop.OpenLeft - stop.OpenRight).Length(), 2.0f * kRoadHalfWidth, 0.05f);
        // The stop lies outside the rim, within a few widths of it.
        const float32 trimmed = std::fabs(stop.StopDistance - stop.NodeDistance);
        EXPECT_GT(trimmed, 0.0f);
        EXPECT_LT(trimmed, 4.0f * kRoadHalfWidth + 1.0f);
    }
}

TEST(PavingPlanTests, AnIgnoredRoadLeavesBothItsArmsUnpaved)
{
    const PavingRoadCurve millLane = StraightRoad(2, Vector3(-40.0f, 0.0f, 0.0f), Vector3(-5.0f, 0.0f, 0.0f));
    const PavingRoadCurve fordRoad = StraightRoad(3, Vector3(0.0f, 0.0f, 40.0f), Vector3(0.0f, 0.0f, 5.0f));
    PavingSource square = MarketSquare(millLane, fordRoad);
    square.UnpavedRoads.push_back(millLane.Road.id);
    const PavingSource sources[] = {square};

    const PavingPlan plan = PlanPavings(sources);

    EXPECT_EQ(std::count(plan.ArmRoads.begin(), plan.ArmRoads.end(), millLane.Road.id), 0);
    EXPECT_EQ(std::count(plan.ArmRoads.begin(), plan.ArmRoads.end(), fordRoad.Road.id), 2);
    EXPECT_EQ(OpenArmCount(plan), 1u);
}

TEST(PavingPlanTests, AnIgnoredRoadIsCutOutOfTheSquareAlongItsStripAndTheBorder)
{
    constexpr float32 kBorderWidth = 0.3f;
    const PavingRoadCurve millLane = StraightRoad(2, Vector3(-40.0f, 0.0f, 0.0f), Vector3(-5.0f, 0.0f, 0.0f));
    const PavingRoadCurve fordRoad = StraightRoad(3, Vector3(0.0f, 0.0f, 40.0f), Vector3(0.0f, 0.0f, 5.0f));
    PavingSource square = MarketSquare(millLane, fordRoad);
    square.UnpavedRoads.push_back(millLane.Road.id);
    square.BorderWidth = kBorderWidth;
    PavingSource withoutMillLane = square;
    std::erase_if(withoutMillLane.Arms, [&millLane](const SG::PavedArm& arm) { return arm.Road == millLane.Road.id; });
    const PavingSource sources[] = {square};
    const PavingSource uncutSources[] = {withoutMillLane};

    const PavingPlan plan = PlanPavings(sources);
    const PavingPlan uncut = PlanPavings(uncutSources);

    ASSERT_EQ(plan.Footprint.Regions.size(), 1u);
    ASSERT_EQ(uncut.Footprint.Regions.size(), 1u);
    const SG::PavedRegion& region = plan.Footprint.Regions[0];
    // Mill Lane runs 5 m into the square: its strip and the border's width on each side and at its
    // end are not paved.
    const float32 cutWidth = 2.0f * (kRoadHalfWidth + kBorderWidth);
    const float32 cutLength = 5.0f + kBorderWidth;
    EXPECT_NEAR(PavedArea(region), PavedArea(uncut.Footprint.Regions[0]) - cutWidth * cutLength, 0.05f);
    for (const Vector2& point : {Vector2(-9.5f, 0.0f), Vector2(-7.0f, kRoadHalfWidth - 0.1f),
                                 Vector2(-5.1f, -kRoadHalfWidth + 0.1f)})
        EXPECT_FALSE(Paves(region, point)) << point.x << ", " << point.y;
    EXPECT_TRUE(Paves(region, Vector2(-7.0f, kRoadHalfWidth + kBorderWidth + 0.1f)));
    // The border runs along the cut.
    size_t cutEdges = 0;
    for (const SG::PavedBoundaryRun& run : region.Runs)
        cutEdges += static_cast<size_t>(std::count_if(run.Edges.begin(), run.Edges.end(), [](const SG::PavedEdge& edge) {
            return edge.Source == SG::PavedEdgeSource::Exclude;
        }));
    EXPECT_GE(cutEdges, 3u);
}

TEST(PavingPlanTests, AnUnpavedRoadAcrossTheRimAtAnAngleIsCutUntilItsWholeWidthIsOutside)
{
    constexpr float32 kBorderWidth = 0.3f;
    // The road crosses the west rim at (-10, 0) at 45 degrees and ends inside.
    const PavingRoadCurve road = StraightRoad(2, Vector3(-30.0f, 0.0f, -20.0f), Vector3(0.0f, 0.0f, 10.0f));
    PavingSource square;
    square.Entity = ECS::EntityHandle{1, 1};
    square.Outline = Square(kSquareHalfSize);
    AddBothArms(square, road, 20.0f * std::sqrt(2.0f));
    square.UnpavedRoads.push_back(road.Road.id);
    square.BorderWidth = kBorderWidth;
    const PavingSource sources[] = {square};

    const PavingPlan plan = PlanPavings(sources);

    ASSERT_EQ(plan.Footprint.Regions.size(), 1u);
    // On the strip, inside the rim, 1.5 m out along the road from where its centerline crosses the rim.
    EXPECT_FALSE(Paves(plan.Footprint.Regions[0], Vector2(-9.79f, -2.33f)));
}

TEST(PavingPlanTests, AnIgnoredRoadAcrossARoadsPavingIsCutOutAlongItsStripAndMeetsItFlush)
{
    constexpr float32 kBorderWidth = 0.3f;
    const PavingRoadCurve highStreet = StraightRoad(5, Vector3(-50.0f, 0.0f, 0.0f), Vector3(50.0f, 0.0f, 0.0f));
    const PavingRoadCurve fordRoad = StraightRoad(6, Vector3(0.0f, 0.0f, -30.0f), Vector3(0.0f, 0.0f, 30.0f));
    PavingSource onHighStreet;
    onHighStreet.Entity = highStreet.Road;
    onHighStreet.Rank.LengthMeters = highStreet.Length;
    onHighStreet.BorderWidth = kBorderWidth;
    AddBothArms(onHighStreet, highStreet, 50.0f);
    AddBothArms(onHighStreet, fordRoad, 30.0f);
    onHighStreet.UnpavedRoads.push_back(fordRoad.Road.id);
    const PavingSource sources[] = {onHighStreet};

    const PavingPlan plan = PlanPavings(sources);

    ASSERT_EQ(plan.Footprint.Regions.size(), 1u);
    const SG::PavedRegion& region = plan.Footprint.Regions[0];
    // No point of Ford Road's strip is paved, and High Street is paved up to the strip's edges.
    constexpr float32 kInset = 0.05f;
    for (float32 x = -kRoadHalfWidth + kInset; x <= kRoadHalfWidth - kInset; x += 0.25f)
        for (float32 z = -kRoadHalfWidth; z <= kRoadHalfWidth; z += 0.25f)
            EXPECT_FALSE(Paves(region, Vector2(x, z))) << x << ", " << z;
    for (float32 z = -kRoadHalfWidth + kInset; z <= kRoadHalfWidth - kInset; z += 0.25f)
    {
        EXPECT_TRUE(Paves(region, Vector2(kRoadHalfWidth + kInset, z))) << z;
        EXPECT_TRUE(Paves(region, Vector2(-kRoadHalfWidth - kInset, z))) << z;
    }
    // No border runs along the cut across High Street: the paving meets Ford Road flush there.
    ASSERT_FALSE(region.Runs.empty());
    for (const SG::PavedBoundaryRun& run : region.Runs)
    {
        for (size_t e = 0; e < run.Edges.size(); ++e)
        {
            const Vector2 middle = (run.Points[e] + run.Points[(e + 1) % run.Points.size()]) * 0.5f;
            EXPECT_FALSE(std::fabs(std::fabs(middle.x) - kRoadHalfWidth) < 0.4f && std::fabs(middle.y) < kRoadHalfWidth)
                << "a border edge on the cut at " << middle.x << ", " << middle.y;
        }
    }
}

TEST(PavingPlanTests, ASquareNoRoadEntersIsPavedWholeWithAClosedBorder)
{
    PavingSource square;
    square.Entity = ECS::EntityHandle{1, 1};
    square.Outline = Square(kSquareHalfSize);
    const PavingSource sources[] = {square};

    const PavingPlan plan = PlanPavings(sources);

    ASSERT_EQ(plan.Footprint.Regions.size(), 1u);
    EXPECT_EQ(plan.Roles[0], PavingRole::Owns);
    ASSERT_EQ(plan.Footprint.Regions[0].Runs.size(), 1u);
    EXPECT_TRUE(plan.Footprint.Regions[0].Runs[0].Closed);
}

TEST(PavingPlanTests, ARoadWithNoJunctionHasNothingToPave)
{
    PavingSource lane;
    lane.Entity = ECS::EntityHandle{4, 1};
    const PavingSource sources[] = {lane};

    const PavingPlan plan = PlanPavings(sources);

    EXPECT_TRUE(plan.Footprint.Regions.empty());
    EXPECT_EQ(plan.Roles[0], PavingRole::NothingToPave);
}

TEST(PavingPlanTests, TwoRoadsPavingTheCrossingTheyShareBuildItOnceUnderTheLongerRoad)
{
    const PavingRoadCurve highStreet = StraightRoad(5, Vector3(-50.0f, 0.0f, 0.0f), Vector3(50.0f, 0.0f, 0.0f));
    const PavingRoadCurve fordRoad = StraightRoad(6, Vector3(0.0f, 0.0f, -30.0f), Vector3(0.0f, 0.0f, 30.0f));
    PavingSource onHighStreet;
    onHighStreet.Entity = highStreet.Road;
    onHighStreet.Rank.LengthMeters = highStreet.Length;
    PavingSource onFordRoad;
    onFordRoad.Entity = fordRoad.Road;
    onFordRoad.Rank.LengthMeters = fordRoad.Length;
    for (PavingSource* source : {&onFordRoad, &onHighStreet})
    {
        AddBothArms(*source, highStreet, 50.0f);
        AddBothArms(*source, fordRoad, 30.0f);
    }
    const PavingSource sources[] = {onFordRoad, onHighStreet};

    const PavingPlan plan = PlanPavings(sources);

    ASSERT_EQ(plan.Footprint.Regions.size(), 1u);
    EXPECT_EQ(plan.Footprint.Arms.size(), 4u);
    EXPECT_EQ(plan.RegionOwners[0], 1u);
    EXPECT_EQ(plan.Roles[1], PavingRole::Owns);
    EXPECT_EQ(plan.Roles[0], PavingRole::PavedBy);
    EXPECT_EQ(plan.PavedBy[0], 1u);
    // Each arm is trimmed past the other road's ribbon and opens onto its own road.
    EXPECT_EQ(OpenArmCount(plan), 4u);
    for (const SG::PavedArmStop& stop : plan.Footprint.Arms)
        EXPECT_GE(std::fabs(stop.StopDistance - stop.NodeDistance), kRoadHalfWidth);
}

TEST(PavingPlanTests, ABedInsideTheSquareIsAHoleInThePaving)
{
    PavingSource square;
    square.Entity = ECS::EntityHandle{1, 1};
    square.Outline = Square(kSquareHalfSize);
    square.Excludes.push_back(Square(2.0f));
    const PavingSource sources[] = {square};

    const PavingPlan plan = PlanPavings(sources);

    ASSERT_EQ(plan.Footprint.Regions.size(), 1u);
    ASSERT_EQ(plan.Footprint.Regions[0].Pieces.size(), 1u);
    EXPECT_EQ(plan.Footprint.Regions[0].Pieces[0].Holes.size(), 1u);
}

// A ring road leaves the square east at (10, 5) and turns north at (16, 5), where one width past the rim
// falls inside the turn. Both arms that run east stop 2 m up the north leg, so the strip resumes square
// on a straight run and the paving covers the whole turn.
TEST(PavingPlanTests, ARingRoadThatTurnsJustPastTheSquareResumesItsStripPastTheTurn)
{
    const std::vector<Vector2> corners = {Vector2(5.0f, 5.0f), Vector2(16.0f, 5.0f), Vector2(16.0f, 20.0f),
                                          Vector2(5.0f, 20.0f)};
    const PavingRoadCurve ring = LoopRoad(7, corners);
    PavingSource square;
    square.Entity = ECS::EntityHandle{1, 1};
    square.Outline = Square(kSquareHalfSize);
    for (const int8 direction : {int8{+1}, int8{-1}})
    {
        square.Arms.push_back(BuildPavedArm(ring, 5.0f, direction, Vector2(10.0f, 5.0f)));
        square.Arms.push_back(BuildPavedArm(ring, 47.0f, direction, Vector2(5.0f, 10.0f)));
    }
    const std::vector<PavingSource> sources = {square};

    const PavingPlan plan = PlanPavings(sources);

    ASSERT_EQ(plan.Footprint.Regions.size(), 1u);
    // Arc length 13 is (16, 7): the north leg, level with the turn's inner corner (14, 7).
    for (const uint32 arm : {0u, 1u})
    {
        const SG::PavedArmStop& stop = plan.Footprint.Arms[plan.SourceArms[0][arm]];
        EXPECT_TRUE(stop.Open);
        EXPECT_NEAR(stop.StopDistance, 13.0f, 1.0e-3f) << "arm " << arm;
        EXPECT_NEAR(stop.OpenLeft.y, 7.0f, 1.0e-3f) << "arm " << arm;
        EXPECT_NEAR(stop.OpenRight.y, 7.0f, 1.0e-3f) << "arm " << arm;
    }
    EXPECT_TRUE(Paves(plan.Footprint.Regions[0], Vector2(15.5f, 4.0f))) << "the turn's outer corner is not paved";
    EXPECT_TRUE(Paves(plan.Footprint.Regions[0], Vector2(14.5f, 6.5f))) << "the turn's inner corner is not paved";
    EXPECT_FALSE(Paves(plan.Footprint.Regions[0], Vector2(16.0f, 7.5f)));
}

TEST(PavingPlanTests, AnArmOfARingRoadCrossesItsSeam)
{
    const PavingRoadCurve ring = RingRoad(7, 20.0f);

    const SG::PavedArm arm = BuildPavedArm(ring, 1.0f, -1, Vector2(20.0f, 1.0f));

    ASSERT_GE(arm.Stations.size(), 4u);
    EXPECT_FALSE(arm.EndsRoad);
    EXPECT_FLOAT_EQ(arm.RoadLoopMeters, ring.Length);
    EXPECT_NEAR(arm.Stations[0].Distance, 1.0f, 1.0e-3f);
    // Past the seam the distance wraps to the far end of the loop.
    EXPECT_GT(arm.Stations[3].Distance, ring.Length - 2.0f);
    EXPECT_NEAR(arm.Stations.back().Distance, 1.0f + 0.5f * ring.Length, 0.01f);
}

} // namespace
} // namespace GameEngine::SplineRecipes
