// The area boolean (CombineAreas): the union of one set of rings minus the union of another,
// whichever way each ring is wound; CombineTouchingAreas welds what touches.

#include "SplineGeometry/AreaBoolean.h"

#include "Mathematics/Geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;
using Mathematics::Vector2;

namespace
{

AreaRing Square(float minX, float minZ, float size)
{
    return {{minX, minZ}, {minX + size, minZ}, {minX + size, minZ + size}, {minX, minZ + size}};
}

} // namespace

// A cut inside the union is a hole: one piece, its outer ring wound counter-clockwise (given
// clockwise), its hole wound clockwise.
TEST(AreaBoolean, ACutInsideTheUnionIsAClockwiseHole)
{
    AreaRing outer = Square(0.0f, 0.0f, 40.0f);
    std::reverse(outer.begin(), outer.end());
    const std::vector<AreaRing> unions{outer};
    const std::vector<AreaRing> cuts{Square(10.0f, 10.0f, 10.0f)};
    const std::vector<AreaPiece> pieces = CombineAreas(unions, cuts);
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_NEAR(Mathematics::PolygonDoubledSignedArea(pieces[0].Outer), 2.0f * 1600.0f, 0.1f);
    ASSERT_EQ(pieces[0].Holes.size(), 1u);
    EXPECT_NEAR(Mathematics::PolygonDoubledSignedArea(pieces[0].Holes[0]), -2.0f * 100.0f, 0.1f);
}

// Overlapping rings join into one ring, counted once; one apart is a piece of its own.
TEST(AreaBoolean, OverlappingRingsJoinAndOneApartStandsAlone)
{
    const std::vector<AreaRing> unions{Square(0.0f, 0.0f, 40.0f), Square(30.0f, 10.0f, 20.0f),
                                       Square(100.0f, 0.0f, 10.0f)};
    const std::vector<AreaPiece> pieces = CombineAreas(unions, {});
    ASSERT_EQ(pieces.size(), 2u);
    float area = 0.0f;
    for (const AreaPiece& piece : pieces)
    {
        EXPECT_TRUE(piece.Holes.empty());
        area += 0.5f * Mathematics::PolygonDoubledSignedArea(piece.Outer);
    }
    EXPECT_NEAR(area, 1600.0f + 200.0f + 100.0f, 0.1f) << "the overlap counted twice";
}

// Four bars that close a frame leave a hole the union made, and a ring inside that hole is an
// island: a piece of its own.
TEST(AreaBoolean, AnIslandInsideAHoleTheUnionMadeIsAPieceOfItsOwn)
{
    const std::vector<AreaRing> unions{{{0.0f, 0.0f}, {30.0f, 0.0f}, {30.0f, 5.0f}, {0.0f, 5.0f}},
                                       {{25.0f, 0.0f}, {30.0f, 0.0f}, {30.0f, 30.0f}, {25.0f, 30.0f}},
                                       {{0.0f, 25.0f}, {30.0f, 25.0f}, {30.0f, 30.0f}, {0.0f, 30.0f}},
                                       {{0.0f, 0.0f}, {5.0f, 0.0f}, {5.0f, 30.0f}, {0.0f, 30.0f}},
                                       Square(12.0f, 12.0f, 6.0f)};
    const std::vector<AreaPiece> pieces = CombineAreas(unions, {});
    ASSERT_EQ(pieces.size(), 2u);
    const AreaPiece& frame = pieces[0].Holes.empty() ? pieces[1] : pieces[0];
    const AreaPiece& island = pieces[0].Holes.empty() ? pieces[0] : pieces[1];
    ASSERT_EQ(frame.Holes.size(), 1u);
    EXPECT_NEAR(Mathematics::PolygonDoubledSignedArea(frame.Holes[0]), -2.0f * 400.0f, 0.1f);
    EXPECT_TRUE(island.Holes.empty());
    EXPECT_NEAR(Mathematics::PolygonDoubledSignedArea(island.Outer), 2.0f * 36.0f, 0.1f);
}

// With a 1 mm touching tolerance, two squares 0.95 mm apart weld into one piece and 1.05 mm apart
// stay two; a strip 0.95 mm wide between a cut and the area's edge is removed and one 1.05 mm wide
// stays.
TEST(AreaBoolean, TouchingAreasWeldWithinTheToleranceAndNoFarther)
{
    constexpr float kTouchMeters = 1.0e-3f;
    constexpr float kSliverMeters = 1.0e-2f;
    for (const auto& [gap, pieceCount] : {std::pair{0.95e-3f, 1u}, std::pair{1.05e-3f, 2u}})
    {
        const std::vector<AreaRing> unions{Square(0.0f, 0.0f, 10.0f), Square(10.0f + gap, 0.0f, 10.0f)};
        const std::vector<AreaPiece> pieces = CombineTouchingAreas(unions, {}, kTouchMeters, kSliverMeters);
        EXPECT_EQ(pieces.size(), pieceCount) << "a gap of " << gap << " m";
    }
    for (const auto& [width, pieceCount] : {std::pair{0.95e-3f, 0u}, std::pair{1.05e-3f, 1u}})
    {
        const std::vector<AreaRing> unions{Square(0.0f, 0.0f, 10.0f)};
        const std::vector<AreaRing> cuts{{{-1.0f, -1.0f}, {10.0f - width, -1.0f}, {10.0f - width, 11.0f}, {-1.0f, 11.0f}}};
        const std::vector<AreaPiece> pieces = CombineTouchingAreas(unions, cuts, kTouchMeters, kSliverMeters);
        EXPECT_EQ(pieces.size(), pieceCount) << "a strip " << width << " m wide";
    }
}
