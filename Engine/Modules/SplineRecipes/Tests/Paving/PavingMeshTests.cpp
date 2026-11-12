#include "Paving/PavingMesh.h"

#include "Paving/PavingRoadUV.h"
#include "Paving/PavingSeam.h"
#include "Rendering/Materials/MaterialDocument.h"

#include "SplineGeometry/AreaBoolean.h"
#include "SplineGeometry/AreaGround.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace GameEngine::SplineRecipes
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector2;
using Mathematics::Vector3;

// Ground that rises 1 m in 10 toward +X and 1 m in 20 toward +Z: a sloped square's rim.
constexpr float32 kSlopeX = 0.1f;
constexpr float32 kSlopeZ = 0.05f;

float32 SlopedGround(const Vector2& xz)
{
    return kSlopeX * xz.x + kSlopeZ * xz.y;
}

SG::AreaGround SlopedSquare()
{
    SG::AreaPiece square;
    square.Outer = {Vector2(-10.0f, -10.0f), Vector2(10.0f, -10.0f), Vector2(10.0f, 10.0f), Vector2(-10.0f, 10.0f)};
    const SG::AreaPiece pieces[] = {square};
    return SG::BuildAreaGround(pieces, SG::AreaGroundSettings{}, SlopedGround);
}

// A lid with no roads.
PavingLidMesh PlainLid(const SG::AreaGround& ground, const Mathematics::Matrix4x4& ownerWorld, const Vector2& phase)
{
    return BuildPavingLidMesh(ground, {}, {}, {}, ownerWorld, phase, PavingSeamMaterial{}).Lid;
}

TEST(PavingMeshTests, OnASlopedRimThePavingFollowsTheGroundWithNoCrease)
{
    const PavingLidMesh lid =
        PlainLid(SlopedSquare(), Mathematics::Matrix4x4::Identity(), Vector2(0.0f, 0.0f));

    ASSERT_GT(lid.Indices.size(), 6u);
    // Every point, on the rim and inside, stands on the ground: the rim's height at the rim.
    for (const SG::SplineVertex& vertex : lid.Vertices)
        EXPECT_NEAR(vertex.Position.y, SlopedGround(Vector2(vertex.Position.x, vertex.Position.z)), 1.0e-4f);
    // One plane: every triangle faces up with the ground's normal, so no edge folds.
    const Vector3 groundNormal = Vector3(-kSlopeX, 1.0f, -kSlopeZ).Normalize();
    for (size_t t = 0; t + 2 < lid.Indices.size(); t += 3)
    {
        const Vector3& a = lid.Vertices[lid.Indices[t]].Position;
        const Vector3& b = lid.Vertices[lid.Indices[t + 1]].Position;
        const Vector3& c = lid.Vertices[lid.Indices[t + 2]].Position;
        const Vector3 face = Vector3::Cross(b - a, c - a).Normalize();
        EXPECT_GT(Vector3::Dot(face, groundNormal), 0.99999f);
    }
    for (const SG::SplineVertex& vertex : lid.Vertices)
        EXPECT_GT(Vector3::Dot(vertex.Normal, groundNormal), 0.99999f);
}

TEST(PavingMeshTests, TheSeedMovesThePatternAndTheOwnerTurnsIt)
{
    const SG::AreaGround ground = SlopedSquare();
    const PavingLidMesh plain =
        PlainLid(ground, Mathematics::Matrix4x4::Identity(), PavingPatternPhase(0));
    const PavingLidMesh seeded =
        PlainLid(ground, Mathematics::Matrix4x4::Identity(), PavingPatternPhase(3));
    // An owner turned a quarter turn about +Y: its x axis lies along world -Z.
    Mathematics::Matrix4x4 turned = Mathematics::Matrix4x4::Identity();
    turned.Data()[0] = 0.0f;
    turned.Data()[2] = -1.0f;
    turned.Data()[8] = 1.0f;
    turned.Data()[10] = 0.0f;
    const PavingLidMesh rotated = PlainLid(ground, turned, PavingPatternPhase(0));

    ASSERT_EQ(plain.Vertices.size(), seeded.Vertices.size());
    EXPECT_GT((seeded.Vertices[0].UV - plain.Vertices[0].UV).Length(), 0.01f);
    const Vector3 world = plain.Vertices[0].Position;
    EXPECT_NEAR(rotated.Vertices[0].UV.x, -world.z, 1.0e-4f);
    EXPECT_NEAR(rotated.Vertices[0].UV.y, world.x, 1.0e-4f);
}


float32 FlatGround(const Vector2&)
{
    return 0.0f;
}

// A flat 24 m square.
SG::AreaGround FlatSquare()
{
    SG::AreaPiece square;
    square.Outer = {Vector2(-12.0f, -12.0f), Vector2(12.0f, -12.0f), Vector2(12.0f, 12.0f), Vector2(-12.0f, 12.0f)};
    const SG::AreaPiece pieces[] = {square};
    return SG::BuildAreaGround(pieces, SG::AreaGroundSettings{}, FlatGround);
}

constexpr float32 kHalfWidth = 2.0f;

PavingRoadUV StraightRoad(const Vector2& from, const Vector2& to)
{
    PavingRoadUV road;
    road.Points = {from, to};
    const float32 length = (to - from).Length();
    road.Distance = {0.0f, length};
    road.HalfLeft = {kHalfWidth, kHalfWidth};
    road.HalfRight = {kHalfWidth, kHalfWidth};
    road.Spans = {Vector2(0.0f, length)};
    road.LeftV = 0.0f;
    return road;
}

// A road that runs through along +X, and a road that ends on it from -Z and yields to it.
std::vector<PavingRoadUV> TeeJunction()
{
    return {StraightRoad(Vector2(-12.0f, 0.0f), Vector2(12.0f, 0.0f)), StraightRoad(Vector2(0.0f, -12.0f), Vector2(0.0f, 0.0f))};
}

// Meters from `position` past the through road's ribbon edge, on the yielding road's side.
float32 PastThroughRoad(const PavingRoadUV& throughRoad, const Vector3& position)
{
    return -LocateOnRoad(throughRoad, Vector2(position.x, position.z)).Inside;
}

PavingLidParts TeeLid(const PavingRoadSeam& yielding, MaterialPatternSymmetry symmetry)
{
    const std::vector<PavingRoadUV> roads = TeeJunction();
    const PavingRoadSeam seams[] = {PavingRoadSeam{}, yielding};
    PavingSeamMaterial material;
    material.Symmetry = symmetry;
    return BuildPavingLidMesh(FlatSquare(), roads, seams, {}, Mathematics::Matrix4x4::Identity(), Vector2(0.0f, 0.0f),
                              material);
}

TEST(PavingMeshTests, ACrossfadeBandIsHalfTheBandDeepInTheYieldingLidAndNeverPastTheSeam)
{
    PavingRoadSeam blend;
    blend.BandTiles = 2.0f;
    const PavingLidParts parts = TeeLid(blend, MaterialPatternSymmetry::None);
    const PavingRoadUV throughRoad = TeeJunction()[0];

    ASSERT_FALSE(parts.Band.Indices.empty());
    ASSERT_EQ(parts.Band.Colors.size(), parts.Band.Vertices.size());
    ASSERT_EQ(parts.Band.SecondUV.size(), parts.Band.Vertices.size());
    for (size_t v = 0; v < parts.Band.Vertices.size(); ++v)
    {
        const Vector3& position = parts.Band.Vertices[v].Position;
        const float32 past = PastThroughRoad(throughRoad, position);
        // Half of two tiles at one tile per meter, from the seam into the yielding road only.
        EXPECT_GE(past, -2.0e-3f);
        EXPECT_LE(past, 1.0f + 1.0e-3f);
        EXPECT_LE(std::abs(position.x), 2.0f * kHalfWidth + 1.0e-3f);
        // The other field's weight is whole on the seam and none at the band's far edge.
        EXPECT_NEAR(parts.Band.Colors[v].w, 1.0f - std::clamp(past / 1.0f, 0.0f, 1.0f), 1.0e-3f);
    }
    EXPECT_TRUE(parts.Courses[1].Indices.empty());
}

TEST(PavingMeshTests, ASquarePatternAtEqualPitchesTakesTheProjectionAndNoCrossfade)
{
    const PavingLidParts parts = TeeLid(PavingRoadSeam{}, MaterialPatternSymmetry::Square);
    EXPECT_TRUE(parts.Band.Indices.empty());
    EXPECT_FALSE(parts.Lid.Indices.empty());
}

TEST(PavingMeshTests, ACourseLiesWithinItsWidthOfTheSeamAndItsOwnMaterialTilesOncePerSquareOfItsWidth)
{
    PavingRoadSeam stones;
    stones.Look = Components::SplinePavingSeam::StoneCourse;
    stones.CourseWidth = 0.5f;
    stones.HasCourseMaterial = true;
    const PavingLidParts parts = TeeLid(stones, MaterialPatternSymmetry::None);
    const PavingRoadUV throughRoad = TeeJunction()[0];
    const PavingLidMesh& course = parts.Courses[1];

    ASSERT_FALSE(course.Indices.empty());
    EXPECT_TRUE(parts.Band.Indices.empty());
    for (const SG::SplineVertex& vertex : course.Vertices)
    {
        const float32 past = PastThroughRoad(throughRoad, vertex.Position);
        EXPECT_GE(past, -2.0e-3f);
        EXPECT_LE(past, 0.5f + 1.0e-3f);
        // U is the through road's arc length over the course's width; V runs across the course.
        EXPECT_NEAR(vertex.UV.x, (vertex.Position.x + 12.0f) / 0.5f, 1.0e-2f);
        EXPECT_NEAR(vertex.UV.y, std::clamp(past, 0.0f, 0.5f) / 0.5f, 1.0e-2f);
    }
}

TEST(PavingMeshTests, AStoneCourseOverAnEmptyMaterialTurnsThePavingsStonesAcrossTheSeam)
{
    // The through road's pattern: one UV unit per 4 m along and per 2 m across.
    std::vector<PavingRoadUV> roads = TeeJunction();
    roads[0].TilesPerMeterU = 0.25f;
    roads[0].TilesPerMeterV = 0.5f;
    PavingRoadSeam stones;
    stones.Look = Components::SplinePavingSeam::StoneCourse;
    stones.CourseWidth = 0.5f;
    const PavingRoadSeam seams[] = {PavingRoadSeam{}, stones};
    const PavingLidParts parts = BuildPavingLidMesh(FlatSquare(), roads, seams, {}, Mathematics::Matrix4x4::Identity(),
                                                    Vector2(0.0f, 0.0f), PavingSeamMaterial{});
    const PavingLidMesh& course = parts.Courses[1];

    ASSERT_FALSE(course.Indices.empty());
    for (const SG::SplineVertex& vertex : course.Vertices)
    {
        const float32 past = PastThroughRoad(roads[0], vertex.Position);
        // A quarter turn of the through road's mapping: U runs across the seam, V back along it.
        EXPECT_NEAR(vertex.UV.x, std::clamp(past, 0.0f, 0.5f) * 0.25f, 1.0e-2f);
        EXPECT_NEAR(vertex.UV.y, -(vertex.Position.x + 12.0f) * 0.5f, 1.0e-2f);
    }
    // The paving's own material draws the course, lighter by a quarter.
    ASSERT_EQ(course.Colors.size(), course.Vertices.size());
    EXPECT_NEAR(course.Colors.front().x, 1.25f, 1.0e-3f);
}

TEST(PavingMeshTests, AHardCutAddsNoBandAndNoCourse)
{
    PavingRoadSeam hardCut;
    hardCut.Look = Components::SplinePavingSeam::HardCut;
    const PavingLidParts parts = TeeLid(hardCut, MaterialPatternSymmetry::None);
    EXPECT_TRUE(parts.Band.Indices.empty());
    EXPECT_TRUE(parts.Courses[1].Indices.empty());
}

// The yielding road of a T runs on to a square, and its strip stops 2 cm short of the square's rim. That
// seam has no room for a band and draws a hard cut; the seam at the T keeps its band.
TEST(PavingMeshTests, EachSeamOfARoadTakesTheBandDepthOfTheStripCapNearestIt)
{
    constexpr uint32 kThroughRoad = 7u;
    constexpr uint32 kYieldingRoad = 9u;
    constexpr float32 kRimZ = -8.0f;
    constexpr float32 kCapPastRimMeters = 0.02f;
    std::vector<PavingRoadUV> roads = {StraightRoad(Vector2(-12.0f, 0.0f), Vector2(12.0f, 0.0f)),
                                       StraightRoad(Vector2(0.0f, kRimZ + kCapPastRimMeters), Vector2(0.0f, 0.0f))};
    roads[0].Entity = kThroughRoad;
    roads[1].Entity = kYieldingRoad;
    const SG::AreaRing square = {Vector2(-12.0f, -12.0f), Vector2(12.0f, -12.0f), Vector2(12.0f, kRimZ),
                                 Vector2(-12.0f, kRimZ)};
    const SG::AreaRing outlines[] = {square};
    const PavingRoadSeam seams[] = {PavingRoadSeam{}, PavingRoadSeam{}};

    const PavingLidParts parts = BuildPavingLidMesh(FlatSquare(), roads, seams, outlines,
                                                    Mathematics::Matrix4x4::Identity(), Vector2(0.0f, 0.0f),
                                                    PavingSeamMaterial{});

    ASSERT_EQ(parts.HardCutSeams.size(), 1u) << "only the seam on the square's rim is a hard cut";
    EXPECT_EQ(parts.HardCutSeams[0].Road, kYieldingRoad);
    EXPECT_FALSE(parts.HardCutSeams[0].PartnerRoad.has_value()) << "the hard cut lies on the square's rim";
    ASSERT_FALSE(parts.Band.Indices.empty()) << "the T keeps its crossfade";
    for (const SG::SplineVertex& vertex : parts.Band.Vertices)
    {
        // Half of two tiles at one tile per meter, from the through road's seam.
        const float32 past = PastThroughRoad(roads[0], vertex.Position);
        EXPECT_GE(past, -2.0e-3f);
        EXPECT_LE(past, 1.0f + 1.0e-3f);
    }
}

} // namespace

TEST(PavingMeshTests, SeedsPastTheFloatsWholeNumbersKeepTheirOwnPhases)
{
    constexpr uint32 kLargeSeed = 1u << 30;
    const Mathematics::Vector2 first = PavingPatternPhase(kLargeSeed);
    const Mathematics::Vector2 next = PavingPatternPhase(kLargeSeed + 1u);
    EXPECT_GT((first - next).Length(), 0.01f);
    EXPECT_GE(first.x, 0.0f);
    EXPECT_LT(first.x, 64.0f);
}

} // namespace GameEngine::SplineRecipes
