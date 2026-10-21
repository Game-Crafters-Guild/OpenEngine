#include "MarkerAttachment.h"

#include <gtest/gtest.h>

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

} // namespace

TEST(MarkerAttachment, FieldMarkerAttachesOnTheFieldsOwnLine)
{
    const auto lines = Lines("struct S {\n    int A;  // @ge-readonly\n    int B;\n};");
    EXPECT_TRUE(MarkerAttachment::FieldMarkerAttached(lines, 2, 2, std::set<int>{2}));
}

TEST(MarkerAttachment, FieldMarkerAttachesOnPrecedingCommentLine)
{
    const auto lines = Lines("struct S {\n    // @ge-hidden\n    int Secret;\n};");
    EXPECT_TRUE(MarkerAttachment::FieldMarkerAttached(lines, 3, 3, std::set<int>{2}));
}

TEST(MarkerAttachment, FieldValueMarkerUsesSameAttachmentRules)
{
    const auto lines = Lines("struct S {\n    // @ge-tooltip Visible strength.\n    float Strength;\n    float Other;\n};");
    const std::map<int, std::string> values{{2, "Visible strength."}};

    EXPECT_EQ("Visible strength.", MarkerAttachment::FieldValueMarkerAttached(lines, 3, 3, values).value_or(""));
    EXPECT_FALSE(MarkerAttachment::FieldValueMarkerAttached(lines, 4, 4, values).has_value());
}

TEST(MarkerAttachment, FieldMarkerDoesNotLeakToTheNextField)
{
    // A trailing "// @ge-readonly" on field A (line 2) must NOT attach to field B (line 3):
    // field A's line is code, so it ends the preceding-comment run for B.
    const auto lines = Lines("struct S {\n    int A;  // @ge-readonly\n    int B;\n};");
    EXPECT_FALSE(MarkerAttachment::FieldMarkerAttached(lines, 3, 3, std::set<int>{2}));
}

TEST(MarkerAttachment, FieldMarkerAttachesAnywhereInAMultiLineDeclarationRange)
{
    // The marker sits on the terminating ';' line of a multi-line initializer.
    const auto lines = Lines("struct S {\n    float Axis[3] = {0,\n                    1,\n                    0};  // @ge-hidden\n};");
    EXPECT_TRUE(MarkerAttachment::FieldMarkerAttached(lines, 2, 4, std::set<int>{4}));
}

TEST(MarkerAttachment, StructMarkerAttachesOnDeclarationAndPrecedingRun)
{
    const auto onDecl = Lines("struct Helper {  // @ge-no-add\n    int X;\n};");
    EXPECT_TRUE(MarkerAttachment::StructMarkerAttached(onDecl, 1, std::set<int>{1}));

    const auto onPreceding = Lines("// @ge-no-add\nstruct Helper {\n    int X;\n};");
    EXPECT_TRUE(MarkerAttachment::StructMarkerAttached(onPreceding, 2, std::set<int>{1}));
}

TEST(MarkerAttachment, StructMarkerDoesNotLeakPastAnInterveningCodeLine)
{
    // Marker on line 1, but an unrelated declaration on line 2 breaks the run before the
    // target struct on line 3.
    const auto lines = Lines("// @ge-no-add\nstruct Other {};\nstruct Target {\n    int X;\n};");
    EXPECT_FALSE(MarkerAttachment::StructMarkerAttached(lines, 3, std::set<int>{1}));
}

TEST(MarkerAttachment, NoMarkersMeansNotAttached)
{
    const auto lines = Lines("struct S {\n    int A;\n};");
    EXPECT_FALSE(MarkerAttachment::FieldMarkerAttached(lines, 2, 2, std::set<int>{}));
    EXPECT_FALSE(MarkerAttachment::StructMarkerAttached(lines, 1, std::set<int>{}));
}
