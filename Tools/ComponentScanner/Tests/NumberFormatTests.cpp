// The number layouts the three emitters share. The expected strings are what the previous
// generator wrote for the same values, so the generated files keep their bytes.
#include "NumberFormat.h"

#include <gtest/gtest.h>

using namespace GameEngine::ComponentScanner;

TEST(NumberFormat, ShortestRoundTripLayout)
{
    struct Case
    {
        double Value;
        const char* Expected;
    };
    const Case cases[] = {
        {0.0, "0"},
        {-0.0, "-0"},
        {5.0, "5"},
        {0.1, "0.1"},
        {0.5, "0.5"},
        {-1.5, "-1.5"},
        {100.0, "100"},
        {100000.0, "100000"},
        {0.001, "0.001"},
        {0.00012, "0.00012"},
        {0.000123456789, "0.000123456789"},
        {12345.678901234567, "12345.678901234567"},
        {100000000000000.5, "100000000000000.5"},
        {999999999999999.0, "999999999999999"},
        {1e14, "100000000000000"},
        {1e15, "1000000000000000"},
        {1e16, "10000000000000000"},
        {1e17, "1E+17"},
        {1.2345678901234568e17, "1.2345678901234568E+17"},
        {1.2345678901234569e23, "1.2345678901234569E+23"},
        {1e300, "1E+300"},
        {1e-5, "1E-05"},
        {1.25e-5, "1.25E-05"},
        {7e-5, "7E-05"},
        {-1e-7, "-1E-07"},
        {1.5e-7, "1.5E-07"},
    };
    for (const Case& c : cases)
        EXPECT_EQ(c.Expected, NumberFormat::ShortestRoundTrip(c.Value)) << c.Value;
}

TEST(NumberFormat, ParseDoubleAcceptsMarkerNumbers)
{
    struct Case
    {
        const char* Text;
        double Expected;
    };
    const Case cases[] = {
        {"0", 0.0}, {"0.5", 0.5}, {".5", 0.5}, {"5.", 5.0}, {"+5", 5.0}, {"-1.5e-7", -1.5e-7},
        {" 3 ", 3.0}, {"1E5", 1e5}, {".5e1", 5.0}, {"-0", -0.0},
    };
    for (const Case& c : cases)
    {
        const std::optional<double> parsed = NumberFormat::ParseDouble(c.Text);
        ASSERT_TRUE(parsed.has_value()) << c.Text;
        EXPECT_EQ(c.Expected, *parsed) << c.Text;
    }
}

TEST(NumberFormat, ParseDoubleRejectsWhatIsNotADecimalNumber)
{
    for (const char* text : {"", "abc", "0.0f", "0x10", "1,5", "1e", "e5", ".", "--5", "+-5", "NaN", "Infinity", "inf"})
        EXPECT_FALSE(NumberFormat::ParseDouble(text).has_value()) << text;
}

TEST(NumberFormat, CanonicalNumbersPreserveGrammarAndRange)
{
    using NumberFormat::LiteralStatus;
    for (const char* text : {"", "+1", "- 1.5", "1.2tail", "1e", "1,5", "inf", "nan", "0x1p2"})
    {
        EXPECT_EQ(LiteralStatus::NotALiteral, NumberFormat::CanonicalFloat(text).Status) << text;
        EXPECT_EQ(LiteralStatus::NotALiteral, NumberFormat::CanonicalDouble(text).Status) << text;
    }
    for (const char* text : {"1e400", "-1e400", "1e-400", "-1e-400"})
    {
        EXPECT_EQ(LiteralStatus::OutOfRange, NumberFormat::CanonicalFloat(text).Status) << text;
        EXPECT_EQ(LiteralStatus::OutOfRange, NumberFormat::CanonicalDouble(text).Status) << text;
    }
    EXPECT_EQ(LiteralStatus::OutOfRange, NumberFormat::CanonicalFloat("1e39").Status);
    EXPECT_EQ(LiteralStatus::OutOfRange, NumberFormat::CanonicalFloat("1e-46").Status);
    EXPECT_EQ(LiteralStatus::NotALiteral, NumberFormat::CanonicalDouble("1.5f").Status);
}

TEST(NumberFormat, CanonicalNumbersRoundTripIncludingSubnormals)
{
    using NumberFormat::LiteralStatus;
    for (const char* text : {"0", "1.25", "-1.25", "0x10", "1e-45", "-1e-45", "3.4028235e38"})
    {
        const auto number = NumberFormat::CanonicalFloat(text);
        ASSERT_EQ(LiteralStatus::Ok, number.Status) << text;
        const auto repeated = NumberFormat::CanonicalFloat(number.Text);
        EXPECT_EQ(LiteralStatus::Ok, repeated.Status) << text;
        EXPECT_EQ(number.Text, repeated.Text) << text;
    }
    for (const char* text : {"0", "1.25", "-1.25", "0x10", "5e-324", "-5e-324", "1.7976931348623157e308"})
    {
        const auto number = NumberFormat::CanonicalDouble(text);
        ASSERT_EQ(LiteralStatus::Ok, number.Status) << text;
        const auto repeated = NumberFormat::CanonicalDouble(number.Text);
        EXPECT_EQ(LiteralStatus::Ok, repeated.Status) << text;
        EXPECT_EQ(number.Text, repeated.Text) << text;
    }
    EXPECT_EQ("-0", NumberFormat::CanonicalFloat("-0.0").Text);
    EXPECT_EQ("-0", NumberFormat::CanonicalDouble("-0.0").Text);
    EXPECT_EQ("1.25", NumberFormat::CanonicalFloat("1.25f").Text);
    EXPECT_EQ("2.5", NumberFormat::CanonicalDouble("2.50").Text);
}
