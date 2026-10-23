#include "SmartShape2D/ShapeAnchor.h"

#include "Mathematics/MatrixOps.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"

#include <gtest/gtest.h>

namespace GameEngine::SmartShape2D
{
TEST(SmartShapeAnchor, FollowsAnOffsetAndWrapsTheSegmentIndex)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.AddPoint({0.0f, 0.0f, 0.0f});
    spline.AddPoint({4.0f, 0.0f, 0.0f});
    Components::SmartShapeAnchor anchor;
    anchor.PointIndex = 7;
    anchor.PointOffset = 0.25f;
    const auto result = EvaluateAnchor(spline, anchor, {});
    EXPECT_FLOAT_EQ(result.Data()[12], 1.0f);
    EXPECT_FLOAT_EQ(result.Data()[13], 0.0f);
    EXPECT_FLOAT_EQ(result.Data()[0], 1.0f);
}

TEST(SmartShapeAnchor, UsesTheCurveAndItsForwardTangent)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::CubicBezier;
    spline.AddPoint({0.0f, 0.0f, 0.0f});
    spline.AddPoint({4.0f, 0.0f, 0.0f});
    spline.Points[0].TangentOut = {0.0f, 4.0f, 0.0f};
    spline.Points[1].TangentIn = {0.0f, 4.0f, 0.0f};
    Spline::RebuildSplineCache(spline);
    Components::SmartShapeAnchor anchor;
    anchor.PointOffset = 0.5f;
    const auto result = EvaluateAnchor(spline, anchor, {});
    EXPECT_NEAR(result.Data()[12], 2.0f, 0.0001f);
    EXPECT_NEAR(result.Data()[13], 3.0f, 0.0001f);
    EXPECT_NEAR(result.Data()[0], 1.0f, 0.0001f);
    EXPECT_NEAR(result.Data()[1], 0.0f, 0.0001f);
}

TEST(SmartShapeAnchor, CompensatesParentScaleUnlessEnabled)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.AddPoint({0.0f, 0.0f, 0.0f});
    spline.AddPoint({4.0f, 0.0f, 0.0f});
    const auto parent = Mathematics::MakeTranslation({5.0f, 7.0f, 0.0f}) * Mathematics::MakeScale({2.0f, 3.0f, 1.0f});
    Components::SmartShapeAnchor anchor;
    anchor.PointOffset = 0.5f;
    auto result = parent * EvaluateAnchor(spline, anchor, parent);
    EXPECT_FLOAT_EQ(result.Data()[12], 9.0f);
    EXPECT_FLOAT_EQ(result.Data()[13], 7.0f);
    EXPECT_FLOAT_EQ(result.Data()[0], 1.0f);
    EXPECT_FLOAT_EQ(result.Data()[5], 1.0f);
    anchor.UseShapeScale = true;
    result = parent * EvaluateAnchor(spline, anchor, parent);
    EXPECT_FLOAT_EQ(result.Data()[0], 2.0f);
    EXPECT_FLOAT_EQ(result.Data()[5], 3.0f);
}
}

namespace GameEngine::SmartShape2D
{
TEST(SmartShapeAnchor, FollowsTheShapePlaneWhenTheParentRotatesInThreeDimensions)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.AddPoint({0, 0, 0}, 1);
    spline.AddPoint({4, 0, 0}, 1);
    Components::SmartShapeAnchor anchor;
    anchor.PointOffset = 0.5f;
    const auto parent = Mathematics::MakeRotationY(1.0f);
    const auto world = parent * EvaluateAnchor(spline, anchor, parent);
    const auto expectedForward = parent.TransformPoint({1, 0, 0}) - parent.TransformPoint({});
    const auto forward = world.TransformPoint({1, 0, 0}) - world.TransformPoint({});
    EXPECT_NEAR(forward.x, expectedForward.x, 0.00001f);
    EXPECT_NEAR(forward.y, expectedForward.y, 0.00001f);
    EXPECT_NEAR(forward.z, expectedForward.z, 0.00001f);
    const auto position = world.TransformPoint({});
    const auto expectedPosition = parent.TransformPoint({2, 0, 0});
    EXPECT_NEAR(position.x, expectedPosition.x, 0.00001f);
    EXPECT_NEAR(position.z, expectedPosition.z, 0.00001f);
}
}
