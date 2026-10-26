// An area's ground (BuildAreaGround): its rings sampled along the ground and the lid over its
// pieces with their holes, split as the settings ask.

#include "SplineGeometry/AreaBoolean.h"
#include "SplineGeometry/AreaGround.h"

#include "Mathematics/Angle.h"
#include "Mathematics/Geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;
using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

AreaRing Square(float minX, float minZ, float size)
{
    return {{minX, minZ}, {minX + size, minZ}, {minX + size, minZ + size}, {minX, minZ + size}};
}

float Slope(const Vector2& xz) { return 0.1f * xz.x + 0.05f * xz.y; }

// A 12 m square with a 4 m square hole, as CombineAreas gives it.
std::vector<AreaPiece> SquareWithHole()
{
    const std::vector<AreaRing> unions{Square(0.0f, 0.0f, 12.0f)};
    const std::vector<AreaRing> cuts{Square(4.0f, 4.0f, 4.0f)};
    return CombineAreas(unions, cuts);
}

// The band between radii `inner` and `outer` about the origin from `fromDegrees` to `toDegrees`,
// each arc sampled every `stepDegrees`, wound counter-clockwise.
AreaRing ArcBand(float inner, float outer, float fromDegrees, float toDegrees, float stepDegrees)
{
    const int steps = static_cast<int>(std::round((toDegrees - fromDegrees) / stepDegrees));
    AreaRing ring;
    for (int i = 0; i <= steps; ++i)
    {
        const float radians = Mathematics::ToRadians(fromDegrees + stepDegrees * static_cast<float>(i));
        ring.emplace_back(outer * std::cos(radians), outer * std::sin(radians));
    }
    for (int i = steps; i >= 0; --i)
    {
        const float radians = Mathematics::ToRadians(fromDegrees + stepDegrees * static_cast<float>(i));
        ring.emplace_back(inner * std::cos(radians), inner * std::sin(radians));
    }
    return ring;
}

float TriangleDoubledArea(const Vector3& a, const Vector3& b, const Vector3& c)
{
    return (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
}

} // namespace

// Under the default settings the lid covers the area once (its triangles' area is the area's,
// every one counter-clockwise from above), no lid edge is longer than 1 m, every ring sample is a
// lid point on the ground at distance 0 from the rings, and the rings are sampled 0.5 m apart.
TEST(AreaGround, TheDefaultLidCoversTheAreaOnceWithMeterEdgesOnTheGround)
{
    const AreaGround ground = BuildAreaGround(SquareWithHole(), AreaGroundSettings{}, Slope);
    ASSERT_EQ(ground.GetRingCount(), 2u);
    EXPECT_EQ(ground.GetRing(0).size(), 96u) << "48 m of outer ring at 0.5 m";
    EXPECT_EQ(ground.GetRing(1).size(), 32u) << "16 m of hole at 0.5 m";
    EXPECT_LT(Mathematics::PolygonDoubledSignedArea(ground.GetRingXZ(1)), 0.0f) << "the hole runs clockwise";

    float doubledArea = 0.0f;
    for (std::size_t t = 0; t + 2 < ground.LidTriangles.size(); t += 3)
    {
        const Vector3& a = ground.LidPoints[ground.LidTriangles[t]];
        const Vector3& b = ground.LidPoints[ground.LidTriangles[t + 1]];
        const Vector3& c = ground.LidPoints[ground.LidTriangles[t + 2]];
        const float triangle = TriangleDoubledArea(a, b, c);
        EXPECT_GT(triangle, 0.0f) << "triangle " << t / 3;
        doubledArea += triangle;
        for (const auto& [from, to] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}})
            EXPECT_LE(std::hypot(to.x - from.x, to.z - from.z), 1.0f + 1.0e-4f) << "triangle " << t / 3;
    }
    EXPECT_NEAR(doubledArea, 2.0f * (144.0f - 16.0f), 1.0e-2f);
    for (std::size_t i = 0; i < ground.LidPoints.size(); ++i)
    {
        const Vector3& point = ground.LidPoints[i];
        EXPECT_NEAR(point.y, Slope(Vector2(point.x, point.z)), 1.0e-4f) << "lid point " << i;
    }
    for (std::size_t i = 0; i < ground.Outline.size(); ++i)
    {
        EXPECT_EQ(ground.LidPoints[i].x, ground.Outline[i].x) << "ring sample " << i << " is not the lid's";
        EXPECT_EQ(ground.LidPoints[i].z, ground.Outline[i].z) << "ring sample " << i << " is not the lid's";
        EXPECT_EQ(ground.LidEdgeDistance[i], 0.0f);
    }
    EXPECT_TRUE(ground.Contains(Vector2(2.0f, 2.0f)));
    EXPECT_FALSE(ground.Contains(Vector2(6.0f, 6.0f))) << "inside the hole";
    EXPECT_FALSE(ground.Contains(Vector2(13.0f, 6.0f)));
}

// The settings bound the work: a ring takes at most MaxRingSamples samples, and the split stops
// at MaxLidTriangles (one split adds at most two), the ring samples put back on the edges adding
// at most one triangle each.
TEST(AreaGround, TheSettingsCapTheRingSamplesAndTheLidTriangles)
{
    AreaGroundSettings settings;
    settings.MaxRingSamples = 16;
    settings.MaxLidTriangles = 40;
    settings.MaxLidEdgeMeters = 0.25f;
    const std::vector<AreaRing> unions{Square(0.0f, 0.0f, 25.0f)};
    const AreaGround ground = BuildAreaGround(CombineAreas(unions, {}), settings, Slope);
    ASSERT_EQ(ground.GetRingCount(), 1u);
    EXPECT_LE(ground.GetRing(0).size(), 16u);
    EXPECT_LE(ground.LidTriangles.size() / 3, settings.MaxLidTriangles + 2u + ground.GetRing(0).size());
    EXPECT_GE(ground.LidTriangles.size() / 3, settings.MaxLidTriangles);
}

// An outer ring wound clockwise and a hole wound counter-clockwise are turned: the rings come out
// counter-clockwise and clockwise, and the lid covers the area once.
TEST(AreaGround, ARingWoundTheOtherWayIsTurned)
{
    std::vector<AreaPiece> pieces = SquareWithHole();
    ASSERT_EQ(pieces.size(), 1u);
    std::reverse(pieces[0].Outer.begin(), pieces[0].Outer.end());
    std::reverse(pieces[0].Holes[0].begin(), pieces[0].Holes[0].end());
    const AreaGround ground = BuildAreaGround(pieces, AreaGroundSettings{}, Slope);
    ASSERT_EQ(ground.GetRingCount(), 2u);
    EXPECT_GT(Mathematics::PolygonDoubledSignedArea(ground.GetRingXZ(0)), 0.0f);
    EXPECT_LT(Mathematics::PolygonDoubledSignedArea(ground.GetRingXZ(1)), 0.0f);
    float doubledArea = 0.0f;
    for (std::size_t t = 0; t + 2 < ground.LidTriangles.size(); t += 3)
    {
        const float triangle = TriangleDoubledArea(ground.LidPoints[ground.LidTriangles[t]],
                                                   ground.LidPoints[ground.LidTriangles[t + 1]],
                                                   ground.LidPoints[ground.LidTriangles[t + 2]]);
        EXPECT_GT(triangle, 0.0f) << "triangle " << t / 3;
        doubledArea += triangle;
    }
    EXPECT_NEAR(doubledArea, 2.0f * (144.0f - 16.0f), 1.0e-2f);
}

// Roads' arc bands that meet along a radius, each sampled at its own step, join with lens-shaped
// holes between their chords, too thin to hold a lid corner: the lid over them covers the area
// once and meets the edge limit, with the triangle count well under the cap.
TEST(AreaGround, TheLidOverLensHolesBetweenArcsCoversTheAreaWithMeterEdges)
{
    const std::vector<std::vector<AreaRing>> inputs{
        {ArcBand(18.0f, 22.0f, 7.5f, 45.0f, 1.5f), ArcBand(18.0f, 22.0f, 45.0f, 82.5f, 1.5f),
         ArcBand(22.0f, 27.0f, 21.0f, 45.0f, 2.0f), ArcBand(22.0f, 27.0f, 45.0f, 69.0f, 2.0f)},
        {ArcBand(18.0f, 22.0f, 7.5f, 45.0f, 1.5f), ArcBand(18.0f, 22.0f, 45.0f, 82.5f, 1.5f),
         ArcBand(22.0f, 27.0f, 21.0f, 45.0f, 2.0f), ArcBand(22.0f, 27.0f, 45.0f, 69.0f, 2.0f),
         ArcBand(27.0f, 31.0f, 30.0f, 60.0f, 2.5f)},
        {ArcBand(14.0f, 18.0f, 5.0f, 85.0f, 1.25f), ArcBand(18.0f, 22.0f, 7.5f, 45.0f, 1.5f),
         ArcBand(18.0f, 22.0f, 45.0f, 82.5f, 1.5f), ArcBand(22.0f, 27.0f, 21.0f, 45.0f, 2.0f),
         ArcBand(22.0f, 27.0f, 45.0f, 69.0f, 2.0f), ArcBand(27.0f, 31.0f, 9.0f, 81.0f, 3.0f)}};
    const AreaGroundSettings settings;
    for (std::size_t input = 0; input < inputs.size(); ++input)
    {
        SCOPED_TRACE(::testing::Message() << inputs[input].size() << " bands");
        const std::vector<AreaPiece> pieces = CombineAreas(inputs[input], {});
        ASSERT_EQ(pieces.size(), 1u);
        ASSERT_FALSE(pieces[0].Holes.empty()) << "the input has no lens hole";
        float pieceDoubledArea = Mathematics::PolygonDoubledSignedArea(pieces[0].Outer);
        for (const AreaRing& hole : pieces[0].Holes)
            pieceDoubledArea += Mathematics::PolygonDoubledSignedArea(hole);
        const AreaGround ground = BuildAreaGround(pieces, settings, Slope);
        float doubledArea = 0.0f;
        float longestEdge = 0.0f;
        for (std::size_t t = 0; t + 2 < ground.LidTriangles.size(); t += 3)
        {
            const Vector3& a = ground.LidPoints[ground.LidTriangles[t]];
            const Vector3& b = ground.LidPoints[ground.LidTriangles[t + 1]];
            const Vector3& c = ground.LidPoints[ground.LidTriangles[t + 2]];
            doubledArea += TriangleDoubledArea(a, b, c);
            for (const auto& [from, to] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}})
                longestEdge = std::max(longestEdge, std::hypot(to.x - from.x, to.z - from.z));
        }
        EXPECT_NEAR(doubledArea, pieceDoubledArea, 0.5f);
        EXPECT_LE(longestEdge, settings.MaxLidEdgeMeters * 1.05f);
        EXPECT_LT(ground.LidTriangles.size() / 3, settings.MaxLidTriangles * 3 / 4);
    }
}
