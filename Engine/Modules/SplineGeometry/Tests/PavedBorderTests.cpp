// A paving region's border (BuildPavedBorder): its boundary runs as strip stations, ready for
// BuildSplineStrip with the border's Rectangle profile.

#include "SplineGeometry/AreaGround.h"
#include "SplineGeometry/PavedBorder.h"
#include "SplineGeometry/PavedFootprint.h"

#include "Mathematics/Geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;
using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

constexpr float kStationSpacing = 0.5f;

AreaRing Rectangle(float minX, float minZ, float maxX, float maxZ)
{
    return {{minX, minZ}, {maxX, minZ}, {maxX, maxZ}, {minX, maxZ}};
}

PavedArm StraightArm(uint32 road, const Vector2& node, const Vector2& direction, float length, float halfWidth)
{
    PavedArm arm;
    arm.Road = road;
    arm.Node = node;
    const int steps = static_cast<int>(std::ceil(length / kStationSpacing));
    for (int i = 0; i <= steps; ++i)
    {
        const float along = length * static_cast<float>(i) / static_cast<float>(steps);
        arm.Stations.push_back(PavedArmStation{node + direction * along, halfWidth, halfWidth, along});
    }
    return arm;
}

float Flat(const Vector2&) { return 0.0f; }

// The runs of the one region `outlines`, `arms` and `excludes` make.
std::vector<PavedBoundaryRun> RunsOf(const std::vector<AreaRing>& outlines, const std::vector<PavedArm>& arms,
                                     const std::vector<AreaRing>& excludes)
{
    const PavedFootprint footprint = BuildPavedFootprint(PavedFootprintInput{outlines, arms, excludes});
    EXPECT_EQ(footprint.Regions.size(), 1u);
    return footprint.Regions.empty() ? std::vector<PavedBoundaryRun>{} : footprint.Regions[0].Runs;
}

// An L-shaped plaza no road enters, with a planter cut out of it.
std::vector<PavedBoundaryRun> PlazaWithPlanter()
{
    return RunsOf({{{0.0f, 0.0f}, {30.0f, 0.0f}, {30.0f, 12.0f}, {12.0f, 12.0f}, {12.0f, 30.0f}, {0.0f, 30.0f}}}, {},
                  {Rectangle(3.0f, 3.0f, 8.0f, 8.0f)});
}

// The cosine between a station's forward and the edge from `from` to `to`, on the ground plane.
float ForwardAlong(const SplineStripStation& station, const Vector2& from, const Vector2& to)
{
    const Vector2 edge = (to - from).Normalize();
    return station.Forward.x * edge.x + station.Forward.z * edge.y;
}

// Distance never falls along a run, and rises at every station that advances.
void ExpectDistanceContinuous(const PavedBorderRun& border)
{
    for (std::size_t i = 1; i < border.Stations.size(); ++i)
    {
        const SplineStripStation& station = border.Stations[i];
        if (station.Join == SplineStationJoin::Advance)
            EXPECT_GT(station.Distance, border.Stations[i - 1].Distance) << "station " << i;
        else
            EXPECT_EQ(station.Distance, border.Stations[i - 1].Distance) << "station " << i;
    }
}

} // namespace

// A plaza no road enters has a closed border on its rim and one round its planter: each a loop
// whose last station is its first, its U running on from station to station and every face
// meeting itself at the seam on a whole tile, its right of travel out of the paving (into the
// planter on the hole's), and its top the border's height above the lid.
TEST(PavedBorder, AClosedRunIsOneLoopOnWholeTilesWithTheHolesBorderInsideTheHole)
{
    const PavedBorderSettings settings;
    const std::vector<PavedBoundaryRun> runs = PlazaWithPlanter();
    ASSERT_EQ(runs.size(), 2u);
    const std::vector<PavedBorderRun> borders = BuildPavedBorder(runs, settings, Flat);
    ASSERT_EQ(borders.size(), 2u);
    const SplineProfile profile = BuildPavedBorderProfile(settings);
    for (std::size_t r = 0; r < borders.size(); ++r)
    {
        const PavedBorderRun& border = borders[r];
        ASSERT_TRUE(border.Closed);
        ASSERT_GE(border.Stations.size(), 4u);
        EXPECT_EQ(border.Stations.front().Position.x, border.Stations.back().Position.x);
        EXPECT_EQ(border.Stations.front().Position.z, border.Stations.back().Position.z);
        ExpectDistanceContinuous(border);
        EXPECT_NEAR(border.Strip.LoopLengthMetres, border.Stations.back().Distance - border.Stations.front().Distance,
                    1.0e-4f);
        EXPECT_FALSE(border.Strip.GenerateCaps);

        for (const SplineStripStation& station : border.Stations)
        {
            // At a corner the right of one leg runs along the other.
            const bool atCorner = std::any_of(runs[r].Points.begin(), runs[r].Points.end(), [&](const Vector2& point) {
                return std::hypot(point.x - station.Position.x, point.y - station.Position.z) < 1.0e-3f;
            });
            if (atCorner)
                continue;
            const Vector2 outward(station.Position.x + station.Right.x * 0.05f, station.Position.z + station.Right.z * 0.05f);
            const bool inPlanter = Mathematics::PointInPolygon(outward, Rectangle(3.0f, 3.0f, 8.0f, 8.0f));
            EXPECT_EQ(inPlanter, runs[r].Hole) << "run " << r << ": the border's right is not out of the paving";
        }

        const SplineStripMesh mesh = BuildSplineStrip(profile, border.Stations, border.Strip);
        ASSERT_TRUE(mesh.IsValid());
        EXPECT_EQ(mesh.DroppedStations, 0u);
        const uint32 last = (mesh.RingCount - 1) * mesh.RingVertexCount;
        float top = -1.0f;
        for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
        {
            const float tiles = mesh.Vertices[last + k].UV.x - mesh.Vertices[k].UV.x;
            EXPECT_NEAR(tiles, std::round(tiles), 1.0e-3f) << "run " << r << ", ring vertex " << k << " at the seam";
            top = std::max(top, mesh.Vertices[k].Position.y);
        }
        EXPECT_NEAR(top, settings.Height, 1.0e-4f) << "the border's top above the lid";
    }
}

// Where a road leaves the region the runs stop: each open run is capped at both ends, its first
// and last stations facing along the run's first and last edges, so the border ends square to the
// Open edge beside it.
TEST(PavedBorder, AnOpenRunIsCappedAndEndsSquareToTheOpenEdge)
{
    const std::vector<PavedBoundaryRun> runs =
        RunsOf({Rectangle(-10.0f, -10.0f, 10.0f, 10.0f)},
               {StraightArm(0, {0.0f, 10.0f}, {0.0f, 1.0f}, 20.0f, 2.0f),
                StraightArm(1, {10.0f, 0.0f}, {1.0f, 0.0f}, 20.0f, 2.0f)},
               {});
    ASSERT_EQ(runs.size(), 2u);
    const std::vector<PavedBorderRun> borders = BuildPavedBorder(runs, PavedBorderSettings{}, Flat);
    for (std::size_t r = 0; r < borders.size(); ++r)
    {
        const PavedBorderRun& border = borders[r];
        const PavedBoundaryRun& run = runs[r];
        ASSERT_FALSE(border.Closed);
        EXPECT_TRUE(border.Strip.GenerateCaps);
        ExpectDistanceContinuous(border);
        EXPECT_NEAR(ForwardAlong(border.Stations.front(), run.Points[0], run.Points[1]), 1.0f, 1.0e-4f);
        EXPECT_NEAR(ForwardAlong(border.Stations.back(), run.Points[run.Points.size() - 2], run.Points.back()), 1.0f,
                    1.0e-4f);
        EXPECT_TRUE(BuildSplineStrip(BuildPavedBorderProfile(PavedBorderSettings{}), border.Stations, border.Strip).IsValid());
    }
}

// The points the boolean made take the border's corner style: a T junction's two notches are
// mitered (one crease ring each) or rounded (a fan); two roads crossing at 30 degrees turn their
// acute notches past the miter limit, which builds them round and says so.
TEST(PavedBorder, NewPointsTakeTheCornerStyleWithTheMitreLimit)
{
    std::vector<PavedArm> arms{StraightArm(0, {0.0f, 0.0f}, {1.0f, 0.0f}, 30.0f, 2.75f),
                               StraightArm(0, {0.0f, 0.0f}, {-1.0f, 0.0f}, 30.0f, 2.75f),
                               StraightArm(1, {0.0f, 0.0f}, {0.0f, 1.0f}, 30.0f, 1.8f)};
    // The westward arm's arc length falls from the node.
    for (PavedArmStation& station : arms[1].Stations)
        station.Distance = -station.Distance;
    const std::vector<PavedBoundaryRun> runs = RunsOf({}, arms, {});
    int newPoints = 0;
    for (const PavedBoundaryRun& run : runs)
        newPoints += static_cast<int>(std::count(run.NewPoints.begin(), run.NewPoints.end(), uint8{1}));
    ASSERT_EQ(newPoints, 2);

    PavedBorderSettings settings;
    int creases = 0;
    for (const PavedBorderRun& border : BuildPavedBorder(runs, settings, Flat))
    {
        EXPECT_TRUE(border.CornerIssues.empty());
        creases += static_cast<int>(std::count_if(border.Stations.begin(), border.Stations.end(),
                                                  [](const SplineStripStation& s) { return s.Join == SplineStationJoin::Crease; }));
    }
    EXPECT_EQ(creases, 2) << "a miter is one ring emitted twice";

    settings.Corner = SplineCornerStyle::Round;
    int pivots = 0;
    for (const PavedBorderRun& border : BuildPavedBorder(runs, settings, Flat))
        pivots += static_cast<int>(std::count_if(border.Stations.begin(), border.Stations.end(),
                                                 [](const SplineStripStation& s) { return s.Join == SplineStationJoin::Pivot; }));
    EXPECT_GE(pivots, 2 * 8) << "a 90 degree round corner is a fan of 9 rings";

    const float cos30 = std::sqrt(3.0f) * 0.5f;
    std::vector<PavedArm> acute{StraightArm(0, {0.0f, 0.0f}, {1.0f, 0.0f}, 40.0f, 2.0f),
                                StraightArm(0, {0.0f, 0.0f}, {-1.0f, 0.0f}, 40.0f, 2.0f),
                                StraightArm(1, {0.0f, 0.0f}, {cos30, 0.5f}, 40.0f, 2.0f),
                                StraightArm(1, {0.0f, 0.0f}, {-cos30, -0.5f}, 40.0f, 2.0f)};
    for (const std::size_t back : {std::size_t{1}, std::size_t{3}})
        for (PavedArmStation& station : acute[back].Stations)
            station.Distance = -station.Distance;
    int pastLimit = 0;
    for (const PavedBorderRun& border : BuildPavedBorder(RunsOf({}, acute, {}), PavedBorderSettings{}, Flat))
        pastLimit += static_cast<int>(std::count_if(border.CornerIssues.begin(), border.CornerIssues.end(),
                                                    [](const SplineCornerIssue& issue) {
                                                        return issue.Kind == SplineCornerIssueKind::PastMitreLimit;
                                                    }));
    EXPECT_EQ(pastLimit, 2) << "the two acute notches turn 150 degrees";
}

// On ground that falls away from the paving the border's base sinks by the fall across its width,
// so it never floats; where the ground rises away from the paving it does not sink.
TEST(PavedBorder, TheBaseSinksWhereTheGroundFallsAwayUnderIt)
{
    const PavedBorderSettings settings;
    const auto slope = [](const Vector2& xz) { return 0.2f * xz.x; };
    const std::vector<PavedBoundaryRun> runs = RunsOf({Rectangle(0.0f, 0.0f, 10.0f, 10.0f)}, {}, {});
    ASSERT_EQ(runs.size(), 1u);
    const std::vector<PavedBorderRun> borders = BuildPavedBorder(runs, settings, slope);
    int west = 0;
    int east = 0;
    for (const SplineStripStation& station : borders[0].Stations)
    {
        EXPECT_NEAR(station.Position.y, slope(Vector2(station.Position.x, station.Position.z)), 1.0e-5f);
        if (station.Position.x < 1.0e-4f && station.Position.z > 1.0f && station.Position.z < 9.0f)
        {
            EXPECT_NEAR(station.GroundOffset, -0.2f * settings.Width, 1.0e-4f) << "the west side falls outward";
            ++west;
        }
        if (station.Position.x > 10.0f - 1.0e-4f && station.Position.z > 1.0f && station.Position.z < 9.0f)
        {
            EXPECT_EQ(station.GroundOffset, 0.0f) << "the east side rises outward";
            ++east;
        }
    }
    EXPECT_GT(west, 10);
    EXPECT_GT(east, 10);
}

// On a round plaza the lid's outline cuts inside the ring by up to kAreaLidInsetMeters between the
// corners it keeps; the border reaches that far in over the lid, so no gap opens at its foot.
TEST(PavedBorder, TheBorderReachesOverTheLidsEdgeWhereTheLidCutsInsideTheRing)
{
    // A 30 m circle with points 0.5 m apart: the lid's corners skip about three of them at a time,
    // and its split stands on the chord between two corners, inside the ring.
    AreaRing circle;
    constexpr int kPoints = 377;
    for (int i = 0; i < kPoints; ++i)
    {
        const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(kPoints);
        circle.emplace_back(30.0f * std::cos(angle), 30.0f * std::sin(angle));
    }
    const PavedFootprint footprint = BuildPavedFootprint(PavedFootprintInput{std::vector<AreaRing>{circle}, {}, {}});
    ASSERT_EQ(footprint.Regions.size(), 1u);
    const PavedRegion& region = footprint.Regions[0];
    const AreaGround lid = BuildAreaGround(region.Pieces, AreaGroundSettings{}, Flat);
    // The lid's boundary edges are the edges of one triangle each; how far inside the ring their
    // ends and middles lie.
    std::vector<std::pair<uint32, uint32>> edges;
    for (std::size_t t = 0; t + 2 < lid.LidTriangles.size(); t += 3)
        for (std::size_t k = 0; k < 3; ++k)
            edges.emplace_back(std::minmax(lid.LidTriangles[t + k], lid.LidTriangles[t + (k + 1) % 3]));
    std::sort(edges.begin(), edges.end());
    float deepest = 0.0f;
    for (std::size_t i = 0; i < edges.size(); ++i)
    {
        const bool shared = (i > 0 && edges[i - 1] == edges[i]) || (i + 1 < edges.size() && edges[i + 1] == edges[i]);
        if (shared)
            continue;
        const Vector3& a = lid.LidPoints[edges[i].first];
        const Vector3& b = lid.LidPoints[edges[i].second];
        for (const Vector2& point : {Vector2(a.x, a.z), Vector2((a.x + b.x) * 0.5f, (a.z + b.z) * 0.5f)})
        {
            float nearest = std::numeric_limits<float>::max();
            const std::vector<Vector2>& ring = region.Runs[0].Points;
            for (std::size_t j = 0; j < ring.size(); ++j)
                nearest = std::min(nearest, Mathematics::DistanceToSegment(point, ring[j], ring[(j + 1) % ring.size()]));
            deepest = std::max(deepest, nearest);
        }
    }
    EXPECT_GT(deepest, 1.0e-3f) << "the fixture's lid cuts inside the ring";
    const std::vector<PavedBorderRun> borders = BuildPavedBorder(region.Runs, PavedBorderSettings{}, Flat);
    ASSERT_EQ(borders.size(), 1u);
    for (const SplineStripStation& station : borders[0].Stations)
        EXPECT_GE(station.HalfWidthLeft, deepest) << "a gap between the lid and the border's foot";
}
