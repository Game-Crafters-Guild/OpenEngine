#include "MarkerScanner.h"

#include <gtest/gtest.h>

#include <sstream>

using namespace GameEngine::ComponentScanner;

namespace
{

std::vector<std::string> Lines(const std::string& s)
{
    std::vector<std::string> lines;
    std::string current;
    for (char c : s)
    {
        if (c == '\n')
        {
            lines.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(c);
        }
    }
    lines.push_back(current);
    return lines;
}

MarkerLines Scan(const std::string& text)
{
    std::ostringstream warnings;
    return MarkerScanner::Scan(Lines(text), warnings);
}

} // namespace

TEST(MarkerScanner, DetectsEachMarkerInLineComments)
{
    const MarkerLines m = Scan("a // @ge-no-add\nb // @ge-readonly\nc // @ge-hidden\nd");
    EXPECT_EQ(1u, m.NoAdd.count(1));
    EXPECT_EQ(1u, m.ReadOnly.count(2));
    EXPECT_EQ(1u, m.Hidden.count(3));
    EXPECT_EQ(0u, m.NoAdd.count(4));
}

TEST(MarkerScanner, DetectsEditorOnlyMarkerOnlyInALineComment)
{
    const MarkerLines m = Scan("// @ge-editor-only  stripped from exports\nstruct Measure {};\nconst char* s = \"@ge-editor-only\";");
    EXPECT_EQ(1u, m.EditorOnly.count(1));
    EXPECT_EQ(0u, m.EditorOnly.count(3));
    EXPECT_TRUE(m.NoAdd.empty());
}

TEST(MarkerScanner, DetectsMarkerInTrailingComment)
{
    const MarkerLines m = Scan("float Radius = 1.0f;  // @ge-readonly runtime handle");
    EXPECT_EQ(1u, m.ReadOnly.count(1));
}

TEST(MarkerScanner, IgnoresMarkerTextOutsideALineComment)
{
    // No "//" on the line -> not a marker (the token appears in code or a string).
    const MarkerLines m = Scan("const char* s = \"@ge-hidden\";\nint x; @ge-readonly");
    EXPECT_TRUE(m.Hidden.empty());
    EXPECT_TRUE(m.ReadOnly.empty());
}

TEST(MarkerScanner, RecordsMultipleDistinctMarkersOnOneLine)
{
    const MarkerLines m = Scan("int x;  // @ge-readonly @ge-hidden");
    EXPECT_EQ(1u, m.ReadOnly.count(1));
    EXPECT_EQ(1u, m.Hidden.count(1));
}

TEST(MarkerScanner, CapturesRangeBounds)
{
    struct Case
    {
        const char* Line;
        double Min;
        std::optional<double> Max;
    };
    const Case cases[] = {
        {"float Interval;  // @ge-range 0", 0.0, std::nullopt},
        {"float Interval;  // @ge-range 0 seconds, open maximum", 0.0, std::nullopt},
        {"float Interval;  // @ge-range: 0 60", 0.0, 60.0},
        {"float Bias = 0.5f;  // @ge-range -1.5 1.5 signed", -1.5, 1.5},
    };
    for (const Case& c : cases)
    {
        const MarkerLines m = Scan(c.Line);
        ASSERT_EQ(1u, m.Ranges.count(1)) << c.Line;
        EXPECT_EQ(c.Min, m.Ranges.at(1).Min) << c.Line;
        EXPECT_EQ(c.Max, m.Ranges.at(1).Max) << c.Line;
    }
}

TEST(MarkerScanner, RejectsMalformedRange)
{
    for (const char* line : {"float Interval;  // @ge-range", "float Interval;  // @ge-range abc",
                             "float Interval;  // @ge-range 1 0", "float Interval;  // @ge-range NaN 1"})
    {
        std::ostringstream warnings;
        const MarkerLines m = MarkerScanner::Scan(Lines(line), warnings);
        EXPECT_TRUE(m.Ranges.empty()) << line;
        EXPECT_NE(std::string::npos, warnings.str().find("warning: malformed @ge-range marker")) << line;
    }
}

TEST(MarkerScanner, CapturesTooltipText)
{
    struct Case
    {
        const char* Line;
        const char* Expected;
    };
    const Case cases[] = {
        {"float Strength;  // @ge-tooltip Strength of the reflection.", "Strength of the reflection."},
        {"float Strength;  // @ge-tooltip: Strength of the reflection.", "Strength of the reflection."},
        {"float Strength;  // @ge-tooltip = Strength of the reflection.", "Strength of the reflection."},
        {"float Strength;  // @ge-tooltip \"Strength of the \\\"reflection\\\".\"", "Strength of the \"reflection\"."},
    };
    for (const Case& c : cases)
    {
        const MarkerLines m = Scan(c.Line);
        ASSERT_EQ(1u, m.Tooltips.count(1)) << c.Line;
        EXPECT_EQ(c.Expected, m.Tooltips.at(1)) << c.Line;
    }
}

TEST(MarkerScanner, DetectsDoNotSerializeMarkerAndAliases)
{
    for (const char* line : {"// [DoNotSerialize]", "// [NonSerializable]", "// [DontSerialize]", "// @DoNotSerialize",
                             "// @NonSerializable", "struct Foo {  // [DoNotSerialize] runtime GPU handle"})
        EXPECT_EQ(1u, Scan(line).DoNotSerialize.count(1)) << line;
}

TEST(MarkerScanner, DoNotSerializeIsCaseInsensitive)
{
    for (const char* line : {"// [donotserialize]", "// [DONOTSERIALIZE]", "// [DoNotSerialize]"})
        EXPECT_EQ(1u, Scan(line).DoNotSerialize.count(1)) << line;
}

TEST(MarkerScanner, DoNotSerializeIgnoredOutsideALineComment)
{
    // The token in code or a string literal (no "//") is not a marker.
    const MarkerLines m = Scan("const char* s = \"[DoNotSerialize]\";\nint DoNotSerialize;");
    EXPECT_TRUE(m.DoNotSerialize.empty());
}
