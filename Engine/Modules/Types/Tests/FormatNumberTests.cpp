// The display number formats, all locale-independent: FormatFixed trims trailing zeros, FormatMeters
// adds the unit, FormatGroupedWholeNumber rounds to a whole number and groups the digits, and
// AppendHexBytes writes two lowercase digits for each byte.

#include "Types/FormatNumber.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

using GameEngine::AppendHexBytes;
using GameEngine::FormatFixed;
using GameEngine::FormatGroupedWholeNumber;
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

TEST(FormatNumberTests, FormatGroupedWholeNumberRoundsToWholeAndGroupsTheDigits)
{
    EXPECT_EQ(FormatGroupedWholeNumber(1048576.0), "1,048,576");
    EXPECT_EQ(FormatGroupedWholeNumber(2000000.4), "2,000,000");
    EXPECT_EQ(FormatGroupedWholeNumber(250880.5), "250,881");
    EXPECT_EQ(FormatGroupedWholeNumber(999.6), "1,000");
    EXPECT_EQ(FormatGroupedWholeNumber(100.0), "100");
    EXPECT_EQ(FormatGroupedWholeNumber(-2500.6), "-2,501");
    EXPECT_EQ(FormatGroupedWholeNumber(-0.4), "0");
    EXPECT_EQ(FormatGroupedWholeNumber(1e15), "1e+15");
    EXPECT_EQ(FormatGroupedWholeNumber(std::numeric_limits<double>::infinity()), "inf");
    EXPECT_EQ(FormatGroupedWholeNumber(std::numeric_limits<double>::quiet_NaN()), "nan");
}

TEST(FormatNumberTests, AppendHexBytesAppendsTwoLowercaseDigitsPerByte)
{
    const std::uint8_t bytes[] = {0x00, 0x0a, 0x7f, 0xff};
    std::string text = "id:";
    AppendHexBytes(text, bytes);
    EXPECT_EQ(text, "id:000a7fff");
}
