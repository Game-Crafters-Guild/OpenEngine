#include "Mathematics/Vector3.h"

#include <gtest/gtest.h>

using GameEngine::Mathematics::Vector3;

TEST(Vector3Tests, LengthSquaredIsTheDotWithItself)
{
    const Vector3 v(1.0f, -2.0f, 3.0f);
    EXPECT_FLOAT_EQ(v.LengthSquared(), 14.0f);
    EXPECT_FLOAT_EQ(v.LengthSquared(), Vector3::Dot(v, v));
}

TEST(Vector3Tests, ConstructsInAConstantExpression)
{
    constexpr Vector3 origin;
    constexpr Vector3 v(1.0f, -2.0f, 3.0f);
    EXPECT_EQ(origin.LengthSquared(), 0.0f);
    EXPECT_EQ(v.x, 1.0f);
    EXPECT_EQ(v.y, -2.0f);
    EXPECT_EQ(v.z, 3.0f);
}

TEST(Vector3Tests, NormalizeOrZeroReturnsAUnitVector)
{
    const Vector3 n = Vector3(0.0f, 3.0f, 4.0f).NormalizeOrZero();
    EXPECT_FLOAT_EQ(n.x, 0.0f);
    EXPECT_FLOAT_EQ(n.y, 0.6f);
    EXPECT_FLOAT_EQ(n.z, 0.8f);
}

TEST(Vector3Tests, NormalizeOrZeroKeepsTheZeroVector)
{
    const Vector3 n = Vector3().NormalizeOrZero();
    EXPECT_EQ(n.x, 0.0f);
    EXPECT_EQ(n.y, 0.0f);
    EXPECT_EQ(n.z, 0.0f);
}

TEST(Vector3Tests, NormalizeOrReturnsTheFallbackForADegenerateVector)
{
    const Vector3 fallback(0.0f, 1.0f, 0.0f);
    // The squared length of 1e-30 underflows to zero in float.
    for (const Vector3& degenerate : {Vector3(), Vector3(1e-30f, 0.0f, -1e-30f)})
    {
        const Vector3 n = degenerate.NormalizeOr(fallback);
        EXPECT_EQ(n.x, fallback.x);
        EXPECT_EQ(n.y, fallback.y);
        EXPECT_EQ(n.z, fallback.z);
    }
}

TEST(Vector3Tests, NormalizeOrReturnsAUnitVector)
{
    const Vector3 n = Vector3(0.0f, -3.0f, 4.0f).NormalizeOr(Vector3(1.0f, 0.0f, 0.0f));
    EXPECT_FLOAT_EQ(n.x, 0.0f);
    EXPECT_FLOAT_EQ(n.y, -0.6f);
    EXPECT_FLOAT_EQ(n.z, 0.8f);
}

TEST(Vector3Tests, NormalizeOrZeroIsNormalizeOrWithTheZeroFallback)
{
    for (const Vector3& v : {Vector3(), Vector3(1e-30f, 0.0f, 0.0f), Vector3(2.0f, -1.0f, 5.0f)})
    {
        const Vector3 a = v.NormalizeOrZero();
        const Vector3 b = v.NormalizeOr(Vector3());
        EXPECT_EQ(a.x, b.x);
        EXPECT_EQ(a.y, b.y);
        EXPECT_EQ(a.z, b.z);
    }
}

TEST(Vector3Tests, NormalizeOrGivesAUnitVectorOrTheFallbackForTinyLengths)
{
    // A squared length under the smallest normal float keeps too few bits to give a unit result.
    const Vector3 fallback(0.0f, 1.0f, 0.0f);
    for (const float length : {3e-23f, 1e-21f, 5e-20f, 2e-19f, 1e-12f})
    {
        const Vector3 n = Vector3(length, 0.0f, 0.0f).NormalizeOr(fallback);
        const bool isFallback = n.x == fallback.x && n.y == fallback.y && n.z == fallback.z;
        EXPECT_TRUE(isFallback || std::abs(n.Length() - 1.0f) < 1e-6f) << "length " << length;
    }
}

// (1 + 2^-12)^2 - (1 + 2^-11) is 2^-24 exactly, and 0 if the product rounds first. Dot fuses the
// x product with the y product as its addend, then the z product on top: the same on every platform.
TEST(Vector3Tests, DotRoundsEachFusedProductOnce)
{
    EXPECT_EQ(Vector3::Dot(Vector3(0x1.001p0f, -0x1.002p0f, 0.0f), Vector3(0x1.001p0f, 1.0f, 0.0f)), 0x1p-24f);
    EXPECT_EQ(Vector3::Dot(Vector3(-0x1.002p0f, 0.0f, 0x1.001p0f), Vector3(1.0f, 0.0f, 0x1.001p0f)), 0x1p-24f);
}
