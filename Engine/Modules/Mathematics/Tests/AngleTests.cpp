#include "Mathematics/Angle.h"

#include <gtest/gtest.h>

#include <numbers>

using GameEngine::Mathematics::ToRadians;

TEST(Angle, ToRadiansConvertsDegrees)
{
    EXPECT_FLOAT_EQ(ToRadians(180.0f), std::numbers::pi_v<float>);
    EXPECT_FLOAT_EQ(ToRadians(-90.0f), -0.5f * std::numbers::pi_v<float>);
    EXPECT_EQ(ToRadians(0.0f), 0.0f);
}
