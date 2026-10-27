// FormatFixed and FormatMeters: fixed notation for display, trailing zeros trimmed, locale-independent.

#include "Types/FormatNumber.h"

#include <gtest/gtest.h>

#include <limits>

using GameEngine::FormatFixed;
using GameEngine::FormatMeters;

TEST(FormatNumberTests, FormatFixedTrimsTrailingZerosAndNeverReadsMinusZero)
{
    EXPECT_EQ(FormatFixed(12.5, 3), "12.5");
    EXPECT_EQ(FormatFixed(3.0, 2), "3");
    EXPECT_EQ(FormatFixed(1.23456, 2), "1.23");
    EXPECT_EQ(FormatFixed(-4.25, 3), "-4.25");
    EXPECT_EQ(FormatFixed(1500.0, 0), "1500");
    EXPECT_EQ(FormatFixed(-0.0001, 3), "0");
    EXPECT_EQ(FormatFixed(std::numeric_limits<double>::infinity(), 3), "inf");
}

TEST(FormatNumberTests, FormatMetersGivesTheLengthToItsDecimalsAndTheUnit)
{
    EXPECT_EQ(FormatMeters(12.5, 1), "12.5 m");
    EXPECT_EQ(FormatMeters(4.0414, 2), "4.04 m");
    EXPECT_EQ(FormatMeters(2.5, 2), "2.5 m");
    EXPECT_EQ(FormatMeters(10.0, 1), "10 m");
    EXPECT_EQ(FormatMeters(61.97, 1), "62 m");
    EXPECT_EQ(FormatMeters(-0.01, 1), "0 m");
}
