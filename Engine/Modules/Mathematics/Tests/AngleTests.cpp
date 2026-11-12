#include "Mathematics/Angle.h"

#include <gtest/gtest.h>

#include <numbers>

using GameEngine::Mathematics::SignedAngleDifference;
using GameEngine::Mathematics::ToDegrees;
using GameEngine::Mathematics::ToRadians;
using GameEngine::Mathematics::WrapAngle;

namespace
{
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kAngleTolerance = 1e-6f;
} // namespace

TEST(Angle, ToRadiansConvertsDegrees)
{
    EXPECT_FLOAT_EQ(ToRadians(180.0f), std::numbers::pi_v<float>);
    EXPECT_FLOAT_EQ(ToRadians(-90.0f), -0.5f * std::numbers::pi_v<float>);
    EXPECT_EQ(ToRadians(0.0f), 0.0f);
}

TEST(Angle, ToDegreesConvertsRadians)
{
    static_assert(ToDegrees(0.0f) == 0.0f);
    EXPECT_FLOAT_EQ(ToDegrees(kPi), 180.0f);
    EXPECT_FLOAT_EQ(ToDegrees(-0.5f * kPi), -90.0f);
    EXPECT_FLOAT_EQ(ToDegrees(ToRadians(37.5f)), 37.5f);
}

TEST(Angle, WrapAngleWrapsIntoHalfOpenTurnAroundZero)
{
    EXPECT_EQ(WrapAngle(kPi), -kPi);
    EXPECT_EQ(WrapAngle(-kPi), -kPi);
    EXPECT_NEAR(WrapAngle(ToRadians(370.0f)), ToRadians(10.0f), kAngleTolerance);
    EXPECT_NEAR(WrapAngle(ToRadians(-190.0f)), ToRadians(170.0f), kAngleTolerance);
    EXPECT_NEAR(WrapAngle(ToRadians(-1070.0f)), ToRadians(10.0f), 4.0f * kAngleTolerance);
    EXPECT_EQ(WrapAngle(1e-8f), 1e-8f);
}

TEST(Angle, SignedAngleDifferenceTakesShortestTurnAcrossSeam)
{
    EXPECT_NEAR(SignedAngleDifference(ToRadians(170.0f), ToRadians(-170.0f)), ToRadians(20.0f), kAngleTolerance);
    EXPECT_NEAR(SignedAngleDifference(ToRadians(-170.0f), ToRadians(170.0f)), ToRadians(-20.0f), kAngleTolerance);
    EXPECT_NEAR(SignedAngleDifference(ToRadians(10.0f), ToRadians(30.0f)), ToRadians(20.0f), kAngleTolerance);
    EXPECT_NEAR(SignedAngleDifference(ToRadians(30.0f), ToRadians(390.0f)), 0.0f, kAngleTolerance);
}
