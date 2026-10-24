#include <Types/ParseNumber.h>

#include <limits>

#include <gtest/gtest.h>

using GameEngine::ParseInt;
using GameEngine::ParseUnsigned;

TEST(ParseIntegerTests, ParseIntReadsAWholeDecimalInteger)
{
    EXPECT_EQ(ParseInt("0"), 0);
    EXPECT_EQ(ParseInt("42"), 42);
    EXPECT_EQ(ParseInt("-17"), -17);
    EXPECT_EQ(ParseInt("9223372036854775807"), std::numeric_limits<int64_t>::max());
    EXPECT_EQ(ParseInt("-9223372036854775808"), std::numeric_limits<int64_t>::min());
}

TEST(ParseIntegerTests, ParseIntRefusesAnythingButTheNumber)
{
    for (const char* text : {"", "-", "+1", " 1", "1 ", "12abc", "0x10", "1.5", "9223372036854775808"})
        EXPECT_FALSE(ParseInt(text).has_value()) << "'" << text << "'";
}

TEST(ParseIntegerTests, ParseUnsignedReadsDigitsOnly)
{
    EXPECT_EQ(ParseUnsigned("0"), 0u);
    EXPECT_EQ(ParseUnsigned("18446744073709551615"), std::numeric_limits<uint64_t>::max());
    for (const char* text : {"", "-1", "+1", " 7", "7 ", "18446744073709551616", "1e3"})
        EXPECT_FALSE(ParseUnsigned(text).has_value()) << "'" << text << "'";
}
