// A road's verge where the road leaves a paving: the paving's curb and the verge meet on the Open
// edge as one sweep, with no gap, no overlap and no end face, at a Flared and at a Straight mouth.
// The test prints the verge's vertices per meter and the vertices a mouth saves. The verge keeps its
// height where the road eases its crown.
#include "RoadVerge.h"

#include "Components/Spline/SplineExtrude.h"
#include "SplineGeometry/PavedBorder.h"
#include "SplineGeometry/PavedFootprint.h"
#include "SplineGeometry/SplineStripBuilder.h"
#include "SplineRecipes/Paving/PavingRoadTrim.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

namespace GameEngine::SplineRecipes
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector2;
using Mathematics::Vector3;

constexpr float32 kTolerance = 1.0e-4f;
// The road runs along +X from x = -40 to x = -4, its arc length x + 40, into a square of 10 m half
// size at x = -10. Its width channel is 2 m, so the default Bevel's top is 1.7 m to each side.
constexpr float32 kRoadStartX = -40.0f;
constexpr float32 kRoadEndX = -4.0f;
constexpr float32 kWidthChannel = 2.0f;
constexpr float32 kTopHalfWidth = 1.7f;
constexpr float32 kNodeX = -10.0f;
constexpr float32 kVergeWidth = 0.45f;
constexpr float32 kVergeHeight = 0.2f;

float32 Flat(const Vector2&)
{
    return 0.0f;
}

// The road's arm out of the square from the node, along -X; its arc length falls.
SG::PavedArm RoadArm()
{
    SG::PavedArm arm;
    arm.Node = Vector2(kNodeX, 0.0f);
    for (int i = 0; i <= 60; ++i)
    {
        const float32 along = 0.5f * static_cast<float32>(i);
        arm.Stations.push_back(SG::PavedArmStation{Vector2(kNodeX - along, 0.0f), kTopHalfWidth, kTopHalfWidth,
                                                   kNodeX - kRoadStartX - along});
    }
    return arm;
}

std::vector<SG::SplineStripStation> RoadStations()
{
    std::vector<SG::SplineStripStation> stations;
    for (int i = 0; kRoadStartX + 0.5f * static_cast<float32>(i) <= kRoadEndX; ++i)
    {
        const float32 x = kRoadStartX + 0.5f * static_cast<float32>(i);
        SG::SplineStripStation station;
        station.Position = Vector3(x, 0.0f, 0.0f);
        station.Forward = Vector3(1.0f, 0.0f, 0.0f);
        station.Right = Vector3(0.0f, 0.0f, -1.0f);
        station.Up = Vector3(0.0f, 1.0f, 0.0f);
        station.HalfWidthLeft = kWidthChannel;
        station.HalfWidthRight = kWidthChannel;
        station.Distance = x - kRoadStartX;
        stations.push_back(station);
    }
    return stations;
}

Components::SplineExtrude VergedRoad()
{
    Components::SplineExtrude road;
    road.Verge = true;
    road.VergeWidth = kVergeWidth;
    road.VergeHeight = kVergeHeight;
    return road;
}

// One mouth as the paving and the road build it: the square's curb, and the road's verge on the run
// that ends on the Open edge. With `verged` off, the road has no verge for the paving, and the verge
// is capped at the throat as it is where no curb reaches.
struct Mouth
{
    SG::SplineStripMesh Curb;
    SG::SplineStripMesh Verge;
    float32 ThroatX = 0.0f;
    float32 VergeLength = 0.0f;
};

Mouth BuildMouth(float32 mouthRadius, bool verged)
{
    const std::vector<SG::AreaRing> outlines = {{{-10.0f, -10.0f}, {10.0f, -10.0f}, {10.0f, 10.0f}, {-10.0f, 10.0f}}};
    const std::vector<SG::PavedArm> arms = {RoadArm()};
    const std::vector<float32> radii = {mouthRadius};
    SG::PavedFootprintInput input;
    input.Outlines = outlines;
    input.Arms = arms;
    input.MouthRadii = radii;
    const SG::PavedFootprint footprint = SG::BuildPavedFootprint(input);
    Mouth mouth;
    EXPECT_EQ(footprint.Regions.size(), 1u);
    if (footprint.Regions.empty() || !footprint.Arms[0].Open)
    {
        ADD_FAILURE() << "the road's arm is not Open";
        return mouth;
    }

    const Components::SplineExtrude road = VergedRoad();
    const SG::PavedBorderSettings settings;
    const std::vector<SG::PavedVerge> verges = {SG::PavedVerge{verged, road.VergeWidth, road.VergeHeight}};
    for (const SG::PavedBorderRun& run : SG::BuildPavedBorder(footprint.Regions[0].Runs, settings, Flat, verges))
        AppendStripMesh(SG::BuildSplineStrip(SG::BuildPavedBorderProfile(settings), run.Stations, run.Strip), mouth.Curb);

    const SG::PavedArmStop& stop = footprint.Arms[0];
    PavingRoadTrim trim;
    trim.NodeDistance = stop.NodeDistance;
    trim.StopDistance = stop.StopDistance;
    trim.Open = true;
    trim.Curbed = true;
    trim.OpenLeft = Vector3(stop.OpenLeft.x, 0.0f, stop.OpenLeft.y);
    trim.OpenRight = Vector3(stop.OpenRight.x, 0.0f, stop.OpenRight.y);
    mouth.ThroatX = 0.5f * (stop.OpenLeft.x + stop.OpenRight.x);

    const std::vector<SG::SplineStripStation> stations = RoadStations();
    std::vector<float32> distances;
    for (const SG::SplineStripStation& station : stations)
        distances.push_back(station.Distance);
    const std::vector<PavingRoadTrim> trims = {trim};
    const TrimmedSweepStations trimmed = TrimSweepStations(stations, distances, 0.0f, trims, Mathematics::Matrix4x4());
    if (trimmed.Runs.empty() || !trimmed.RunEdges[0].second.Open)
    {
        ADD_FAILURE() << "the road's first run does not end on the Open edge";
        return mouth;
    }
    const auto [first, last] = trimmed.Runs[0];
    const RoadVergeEnd start{true, nullptr};
    const RoadVergeEnd end{!verged, &trimmed.RunEdges[0].second};
    mouth.Verge = BuildRoadVerge(road, std::span(trimmed.Stations).subspan(first, last - first + 1u), start, end, 0.0f);
    mouth.VergeLength = trimmed.Stations[last].Distance - trimmed.Stations[first].Distance;
    return mouth;
}

bool HasVertexAt(const SG::SplineStripMesh& mesh, const Vector3& position)
{
    return std::any_of(mesh.Vertices.begin(), mesh.Vertices.end(), [&](const SG::SplineVertex& vertex)
                       { return (vertex.Position - position).Length() <= kTolerance; });
}

// The curb's vertices on the throat that stand above the ground: its rings there, both sides.
std::vector<Vector3> CurbTopAtThroat(const Mouth& mouth)
{
    std::vector<Vector3> top;
    for (const SG::SplineVertex& vertex : mouth.Curb.Vertices)
    {
        if (std::abs(vertex.Position.x - mouth.ThroatX) <= kTolerance && vertex.Position.y > 0.0f)
            top.push_back(vertex.Position);
    }
    return top;
}

void ExpectOneSeam(float32 mouthRadius, const char* shape)
{
    SCOPED_TRACE(shape);
    const Mouth mouth = BuildMouth(mouthRadius, true);
    ASSERT_TRUE(mouth.Verge.IsValid());

    // No gap: every top vertex of the curb on the throat is a vertex of the verge, at the verge's size.
    const std::vector<Vector3> top = CurbTopAtThroat(mouth);
    ASSERT_FALSE(top.empty()) << "the curb does not reach the throat";
    for (const Vector3& position : top)
    {
        EXPECT_TRUE(HasVertexAt(mouth.Verge, position)) << position.x << " " << position.y << " " << position.z;
        EXPECT_NEAR(position.y, kVergeHeight, kTolerance);
    }
    for (const float32 side : {-1.0f, 1.0f})
    {
        const float32 outer = side * (kTopHalfWidth + kVergeWidth);
        EXPECT_TRUE(std::any_of(top.begin(), top.end(),
                                [&](const Vector3& position) { return std::abs(position.z - outer) <= kTolerance; }))
            << "no outer edge at z = " << outer;
    }
    // No overlap: the verge stays on the road's side of the throat and the curb on the paving's side.
    for (const SG::SplineVertex& vertex : mouth.Verge.Vertices)
        EXPECT_LE(vertex.Position.x, mouth.ThroatX + kTolerance);
    for (const SG::SplineVertex& vertex : mouth.Curb.Vertices)
        EXPECT_GE(vertex.Position.x, mouth.ThroatX - kTolerance);
    // No end face: no vertex on the throat faces along the road.
    for (const SG::SplineStripMesh* mesh : {&mouth.Curb, &mouth.Verge})
    {
        for (const SG::SplineVertex& vertex : mesh->Vertices)
        {
            if (std::abs(vertex.Position.x - mouth.ThroatX) <= kTolerance)
            {
                EXPECT_LT(std::abs(vertex.Normal.x), 0.99f) << "an end face on the throat";
            }
        }
    }

    // Two caps fewer on the curb and two on the verge: four points each.
    const Mouth capped = BuildMouth(mouthRadius, false);
    const size_t curbSaved = capped.Curb.Vertices.size() - mouth.Curb.Vertices.size();
    const size_t vergeSaved = capped.Verge.Vertices.size() - mouth.Verge.Vertices.size();
    EXPECT_EQ(curbSaved, 8u);
    EXPECT_EQ(vergeSaved, 8u);
    const size_t indicesSaved = (capped.Curb.Indices.size() - mouth.Curb.Indices.size()) +
                                (capped.Verge.Indices.size() - mouth.Verge.Indices.size());
    std::printf("[ verge cost ] %s mouth: the verge has %.1f vertices per meter of road (both sides); the mouth "
                "has %zu vertices and %zu indices fewer\n",
                shape, static_cast<double>(static_cast<float32>(mouth.Verge.Vertices.size()) / mouth.VergeLength),
                curbSaved + vergeSaved, indicesSaved);
}

TEST(RoadVerge, AtAFlaredMouthTheCurbRunsIntoTheVergeAsOneSweep)
{
    ExpectOneSeam(1.5f, "Flared");
}

TEST(RoadVerge, AtAStraightMouthTheCurbRunsIntoTheVergeAsOneSweep)
{
    ExpectOneSeam(0.0f, "Straight");
}

// Where the road eases its crown flat (RiseScale 0) the verge stands at its whole height, as it does
// beside the full crown.
TEST(RoadVerge, AVergeKeepsItsHeightWhereTheRoadEasesItsCrown)
{
    const std::vector<SG::SplineStripStation> crowned = RoadStations();
    std::vector<SG::SplineStripStation> flattened = crowned;
    for (SG::SplineStripStation& station : flattened)
        station.RiseScale = 0.0f;
    const RoadVergeEnd capped{true, nullptr};

    const SG::SplineStripMesh beside = BuildRoadVerge(VergedRoad(), crowned, capped, capped, 0.0f);
    const SG::SplineStripMesh besideFlat = BuildRoadVerge(VergedRoad(), flattened, capped, capped, 0.0f);

    ASSERT_TRUE(beside.IsValid());
    ASSERT_EQ(besideFlat.Vertices.size(), beside.Vertices.size());
    EXPECT_NEAR(besideFlat.MaxBounds.y, kVergeHeight, kTolerance);
    for (size_t i = 0; i < beside.Vertices.size(); ++i)
        EXPECT_NEAR(besideFlat.Vertices[i].Position.y, beside.Vertices[i].Position.y, kTolerance) << "vertex " << i;
}

} // namespace
} // namespace GameEngine::SplineRecipes
