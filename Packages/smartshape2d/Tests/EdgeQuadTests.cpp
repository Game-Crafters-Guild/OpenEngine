#include "SmartShape2D/EdgeQuad.h"

#include <gtest/gtest.h>

namespace GameEngine::SmartShape2D
{
TEST(SmartShapeEdgeQuad, OuterCornerUsesIncomingAndOutgoingAxes)
{
    const auto quad = MakeEdgeCorner({0.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 2.0f},
        2.0f, 2.0f, 0.0f, false, QuadCorner::Outer);
    EXPECT_FLOAT_EQ(quad.A.x, 1.0f);
    EXPECT_FLOAT_EQ(quad.A.y, -1.0f);
    EXPECT_FLOAT_EQ(quad.B.x, 1.0f);
    EXPECT_FLOAT_EQ(quad.B.y, 1.0f);
    EXPECT_FLOAT_EQ(quad.C.x, 3.0f);
    EXPECT_FLOAT_EQ(quad.C.y, 1.0f);
    EXPECT_FLOAT_EQ(quad.D.x, 3.0f);
    EXPECT_FLOAT_EQ(quad.D.y, -1.0f);
}

TEST(SmartShapeEdgeQuad, WeldingPreservesAverageHeightInsteadOfExtendingAMiter)
{
    EdgeQuad first;
    first.A = {0.0f, -1.0f}; first.B = {0.0f, 1.0f};
    first.C = {2.0f, 1.0f}; first.D = {2.0f, -1.0f};
    EdgeQuad second;
    second.A = {3.0f, 0.0f}; second.B = {1.0f, 0.0f};
    second.C = {1.0f, 2.0f}; second.D = {3.0f, 2.0f};
    WeldEdgeQuads(first, second);
    EXPECT_NEAR(first.D.x, 2.70710678f, 0.00001f);
    EXPECT_NEAR(first.D.y, -0.70710678f, 0.00001f);
    EXPECT_FLOAT_EQ(first.D.x, second.A.x);
    EXPECT_FLOAT_EQ(first.C.y, second.B.y);
    EXPECT_NEAR((first.C - first.D).Length(), 2.0f, 0.00001f);
}

TEST(SmartShapeEdgeQuad, TaperSplitsInsideAndRetexturesAShortSegment)
{
    EdgeQuad edge;
    edge.A = {0.0f, 1.0f}; edge.B = {0.0f, -1.0f};
    edge.C = {4.0f, -1.0f}; edge.D = {4.0f, 1.0f};
    EdgeQuad taper;
    ASSERT_TRUE(SplitEdgeTaper(edge, taper, {1.0f, 2.0f}, 7, false));
    EXPECT_FLOAT_EQ(taper.A.x, 0.0f);
    EXPECT_FLOAT_EQ(taper.D.x, 1.0f);
    EXPECT_FLOAT_EQ(edge.A.x, 1.0f);
    EXPECT_EQ(taper.Part, 7u);
    ASSERT_FALSE(SplitEdgeTaper(edge, taper, {5.0f, 2.0f}, 8, true));
    EXPECT_TRUE(edge.Taper);
    EXPECT_EQ(edge.Part, 8u);
    EXPECT_FLOAT_EQ(edge.D.x, 4.0f);
}

TEST(SmartShapeEdgeQuad, DiagonalTaperUsesBothTextureDimensions)
{
    EdgeQuad edge;
    edge.A = {0.0f, 0.0f}; edge.B = {-1.0f, 1.0f};
    edge.C = {3.0f, 5.0f}; edge.D = {4.0f, 4.0f};
    EdgeQuad taper;
    ASSERT_TRUE(SplitEdgeTaper(edge, taper, {2.0f, 1.0f}, 7, false));
    EXPECT_NEAR(taper.D.x, 1.41421356f, 0.00001f);
    EXPECT_NEAR(taper.D.y, 0.70710678f, 0.00001f);
}
}
