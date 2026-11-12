#include "Mathematics/Vector3F64.h"

#include <type_traits>

#include <gtest/gtest.h>

using GameEngine::Mathematics::Vector3;
using GameEngine::Mathematics::Vector3F64;

static_assert(!std::is_convertible_v<Vector3, Vector3F64>);
static_assert(!std::is_convertible_v<Vector3F64, Vector3>);

TEST(Vector3F64Tests, ConstructsInAConstantExpression)
{
    constexpr Vector3F64 origin;
    constexpr Vector3F64 v(1.0, -2.0, 3.0);
    EXPECT_EQ(origin.LengthSquared(), 0.0);
    EXPECT_EQ(v.X, 1.0);
    EXPECT_EQ(v.Y, -2.0);
    EXPECT_EQ(v.Z, 3.0);
}

TEST(Vector3F64Tests, LengthSquaredIsTheDotWithItself)
{
    const Vector3F64 v(1.0, -2.0, 3.0);
    EXPECT_EQ(v.LengthSquared(), 14.0);
    EXPECT_EQ(v.LengthSquared(), Vector3F64::Dot(v, v));
}

TEST(Vector3F64Tests, ArithmeticWorksPerComponent)
{
    const Vector3F64 a(1.0, 2.0, 3.0);
    const Vector3F64 b(4.0, -5.0, 6.0);
    const Vector3F64 sum = a + b;
    const Vector3F64 difference = a - b;
    const Vector3F64 scaled = 2.0 * a;
    const Vector3F64 divided = b / 2.0;
    EXPECT_EQ(sum.X, 5.0);
    EXPECT_EQ(sum.Y, -3.0);
    EXPECT_EQ(sum.Z, 9.0);
    EXPECT_EQ(difference.X, -3.0);
    EXPECT_EQ(difference.Y, 7.0);
    EXPECT_EQ(difference.Z, -3.0);
    EXPECT_EQ(scaled.X, (a * 2.0).X);
    EXPECT_EQ(scaled.Z, 6.0);
    EXPECT_EQ(divided.Y, -2.5);
}

TEST(Vector3F64Tests, NormalizeReturnsAUnitVector)
{
    const Vector3F64 unit = Vector3F64(0.0, 3.0, 4.0).Normalize();
    EXPECT_EQ(unit.X, 0.0);
    EXPECT_DOUBLE_EQ(unit.Y, 0.6);
    EXPECT_DOUBLE_EQ(unit.Z, 0.8);
}

TEST(Vector3F64Tests, CrossOfXAndYIsZ)
{
    const Vector3F64 z = Vector3F64::Cross(Vector3F64(1.0, 0.0, 0.0), Vector3F64(0.0, 1.0, 0.0));
    EXPECT_EQ(z.X, 0.0);
    EXPECT_EQ(z.Y, 0.0);
    EXPECT_EQ(z.Z, 1.0);
}

TEST(Vector3F64Tests, CrossOfTwoGeneralVectorsMatchesTheHandComputation)
{
    const Vector3F64 cross = Vector3F64::Cross(Vector3F64(1.0, 2.0, 3.0), Vector3F64(4.0, 5.0, 6.0));
    EXPECT_EQ(cross.X, -3.0);
    EXPECT_EQ(cross.Y, 6.0);
    EXPECT_EQ(cross.Z, -3.0);
}

// (1 + 2^-27)^2 - (1 + 2^-26) is 2^-54 exactly, and 0 if the product rounds first.
TEST(Vector3F64Tests, CrossKeepsTheFusedRoundingOfVector3)
{
    const Vector3F64 cross =
        Vector3F64::Cross(Vector3F64(0.0, 0x1.0000002p0, 0x1.0000004p0), Vector3F64(0.0, 1.0, 0x1.0000002p0));
    EXPECT_EQ(cross.X, 0x1p-54);
    EXPECT_EQ(cross.Y, 0.0);
    EXPECT_EQ(cross.Z, 0.0);
}

TEST(Vector3F64Tests, ConvertsToAndFromVector3)
{
    const Vector3 widened(0.1f, -2.0f, 3.0f);
    const Vector3F64 v(widened);
    EXPECT_EQ(v.X, static_cast<double>(0.1f));
    const Vector3 narrowed = static_cast<Vector3>(Vector3F64(0.1, -2.0, 3.0));
    EXPECT_EQ(narrowed.x, 0.1f);
    EXPECT_EQ(narrowed.y, -2.0f);
    EXPECT_EQ(narrowed.z, 3.0f);
}

// (1 + 2^-27)^2 - (1 + 2^-26) is 2^-54 exactly, and 0 if the product rounds first.
TEST(Vector3F64Tests, DotRoundsEachFusedProductOnce)
{
    EXPECT_EQ(Vector3F64::Dot(Vector3F64(0x1.0000002p0, -0x1.0000004p0, 0.0), Vector3F64(0x1.0000002p0, 1.0, 0.0)),
              0x1p-54);
    EXPECT_EQ(Vector3F64::Dot(Vector3F64(-0x1.0000004p0, 0.0, 0x1.0000002p0), Vector3F64(1.0, 0.0, 0x1.0000002p0)),
              0x1p-54);
}

// A 1 m step at Earth radius moves the up direction by 1.6e-7, near one float step: only doubles keep it.
TEST(Vector3F64Tests, UpDirectionAtPlanetRadiusKeepsAOneMeterStep)
{
    constexpr double kRadius = 6371000.0;
    const Vector3F64 up = Vector3F64(1.0, 2.0, 3.0).Normalize();
    const Vector3F64 tangent = Vector3F64::Cross(up, Vector3F64(0.0, 1.0, 0.0)).Normalize();
    const Vector3F64 nextUp = (up * kRadius + tangent).Normalize();
    EXPECT_NEAR(nextUp.Length(), 1.0, 1e-12);
    EXPECT_NEAR(Vector3F64::Dot((nextUp - up) * kRadius, tangent), 1.0, 1e-6);
}
