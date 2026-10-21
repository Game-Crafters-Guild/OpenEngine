#include "Mathematics/MatrixOps.h"

#include <gtest/gtest.h>

#include <numbers>

using namespace GameEngine::Mathematics;

namespace
{
constexpr float kTolerance = 1e-5f;

void ExpectPoint(const Vector2& p, float x, float y)
{
    EXPECT_NEAR(p.x, x, kTolerance);
    EXPECT_NEAR(p.y, y, kTolerance);
}
} // namespace

TEST(Matrix3x3AffineTests, TheProductAppliesTheRightOperandFirst)
{
    const Matrix3x3 scaleThenMove = MakeTranslation2D(Vector2(10.0f, 0.0f)) * MakeScale2D(Vector2(2.0f, 3.0f));
    ExpectPoint(scaleThenMove.TransformPoint(Vector2(1.0f, 1.0f)), 12.0f, 3.0f);

    const Matrix3x3 moveThenScale = MakeScale2D(Vector2(2.0f, 3.0f)) * MakeTranslation2D(Vector2(10.0f, 0.0f));
    ExpectPoint(moveThenScale.TransformPoint(Vector2(1.0f, 1.0f)), 22.0f, 3.0f);
}

// On the UI's y-down screen a positive angle turns +X towards +Y: clockwise.
TEST(Matrix3x3AffineTests, APositiveRotationTurnsXTowardsY)
{
    const Matrix3x3 quarter = MakeRotation2D(std::numbers::pi_v<float> * 0.5f);
    ExpectPoint(quarter.TransformPoint(Vector2(1.0f, 0.0f)), 0.0f, 1.0f);
    ExpectPoint(quarter.TransformPoint(Vector2(0.0f, 1.0f)), -1.0f, 0.0f);
}

TEST(Matrix3x3AffineTests, SkewAndMatrixUseTheCssArgumentOrder)
{
    const float quarterTurn = std::numbers::pi_v<float> * 0.25f;
    ExpectPoint(MakeSkew2D(quarterTurn, 0.0f).TransformPoint(Vector2(0.0f, 2.0f)), 2.0f, 2.0f);
    ExpectPoint(MakeSkew2D(0.0f, quarterTurn).TransformPoint(Vector2(2.0f, 0.0f)), 2.0f, 2.0f);

    // matrix(a, b, c, d, e, f): x' = a x + c y + e, y' = b x + d y + f.
    const Matrix3x3 m = MakeAffine2D(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f);
    ExpectPoint(m.TransformPoint(Vector2(1.0f, 1.0f)), 9.0f, 12.0f);
}

TEST(Matrix3x3AffineTests, TheInverseUndoesTheMap)
{
    const Matrix3x3 m = MakeTranslation2D(Vector2(40.0f, -7.0f)) * MakeRotation2D(0.7f) *
                        MakeSkew2D(0.2f, -0.1f) * MakeScale2D(Vector2(1.5f, 0.5f));
    Matrix3x3 inverse;
    ASSERT_TRUE(TryInverseAffine2D(m, inverse));
    const Vector2 p(13.0f, 29.0f);
    ExpectPoint(inverse.TransformPoint(m.TransformPoint(p)), p.x, p.y);
    ExpectPoint(m.TransformPoint(inverse.TransformPoint(p)), p.x, p.y);
}

TEST(Matrix3x3AffineTests, ASingularMapHasNoInverse)
{
    Matrix3x3 out = MakeTranslation2D(Vector2(1.0f, 2.0f));
    EXPECT_FALSE(TryInverseAffine2D(MakeScale2D(Vector2(0.0f, 1.0f)), out));
    ExpectPoint(out.TransformPoint(Vector2(0.0f, 0.0f)), 1.0f, 2.0f);
}

TEST(Matrix3x3AffineTests, RectBoundsHoldEveryMappedCorner)
{
    const Rect square{0.0f, 0.0f, 2.0f, 2.0f};
    const Rect turned =
        TransformRectBounds2D(MakeTranslation2D(Vector2(1.0f, 1.0f)) * MakeRotation2D(std::numbers::pi_v<float> * 0.25f) *
                                  MakeTranslation2D(Vector2(-1.0f, -1.0f)),
                              square);
    const float halfDiagonal = std::numbers::sqrt2_v<float>;
    EXPECT_NEAR(turned.X, 1.0f - halfDiagonal, kTolerance);
    EXPECT_NEAR(turned.Y, 1.0f - halfDiagonal, kTolerance);
    EXPECT_NEAR(turned.Width, 2.0f * halfDiagonal, kTolerance);
    EXPECT_NEAR(turned.Height, 2.0f * halfDiagonal, kTolerance);
}
