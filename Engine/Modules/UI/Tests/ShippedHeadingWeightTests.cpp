// The weight the editor's small headings, titles and labels take, as the editor ships them. The
// editor ships Roboto Regular (400), Medium (500) and Bold (700) and nothing between, so text
// that asks for 600 draws in Bold: font matching takes the next heavier face for anything above
// 500. At 11 to 14 px these rules ask for 500 and draw in Medium, which keeps the step above the
// Regular labels around them and fills less of each glyph than Bold (fewer full-ink pixels). It
// does not sharpen the unhinted edges text has at 1x. Read from the staged sheets, so a panel
// sheet cannot be what makes the case pass.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"

#include "Core/Application.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using GameEngine::PathUtils;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

// The stylesheets the build stages next to the test exe, at the relative path the editor
// reads them from under its own exe.
std::string ReadShippedCss(const std::string& relative)
{
    std::ifstream in(PathUtils::GetExecutableDirectory() / "Assets" / "UI" / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

constexpr char kTestCss[] = R"(
#root { display: flex; flex-direction: column; width: 300px; height: 200px; }
#shown { display: flex; }
)";

// The project picker's modal is constructed hidden (display: none), so its case shows it through
// #shown, whose id outranks the sheet's class rule; an element that is never shown is never styled.

// The weight of Roboto Medium, the face between the Regular labels and Bold.
constexpr int kMediumWeight = 500;

// One shipped rule: the sheet that holds it and the markup that matches it. The element under
// test has the id "subject"; the markup carries the ancestors the rule's selector needs.
struct ShippedRuleCase
{
    const char* Sheet;
    const char* Markup;
};

constexpr ShippedRuleCase kMediumRules[] = {
    {"theme/inspector.css", R"(<label id="subject" class="inspector-guide-title">Guide</label>)"},
    {"theme/inspector.css", R"(<label id="subject" class="inspector-model-axis-name">Up</label>)"},
    {"theme/node-graph.css",
     R"(<uielement class="node-graph-palette"><label id="subject" class="node-graph-palette-section-title">Color</label></uielement>)"},
    {"theme/todos.css", R"(<label id="subject" class="todo-section-header">To Do</label>)"},
    {"theme/widgets.css", R"(<label id="subject" class="log-detail-title">Stack Trace</label>)"},
    {"panels/SettingsPanel.css", R"(<label id="subject" class="settings-subsection-header">Game name</label>)"},
    {"panels/SettingsPanel.css",
     R"(<uielement class="settings-tree-pane"><uielement class="project-settings-row"><label id="subject" class="tree-title">Project Settings</label></uielement></uielement>)"},
    {"panels/MovieRecorderPanel.css", R"(<label id="subject" class="movie-recorder-section-title">Output</label>)"},
    {"panels/MarkupsPanel.css", R"(<label id="subject" class="markup-member-heading">Members</label>)"},
    {"panels/AnimationWindowPanel.css", R"(<label id="subject" class="anim-options-title">Smooth Options</label>)"},
    {"panels/MissingAssetsPanel.css", R"(<label id="subject" class="missing-assets-row-title">Rock.fbx</label>)"},
    {"panels/DiffPanel.css", R"(<label id="subject" class="diff-scene-entity-key">Camera</label>)"},
    {"controls/InspectorNotice/InspectorNotice.css", R"(<label id="subject" class="inspector-notice-title">Waiting</label>)"},
    {"controls/CollapsibleInfoCard/CollapsibleInfoCard.css", R"(<label id="subject" class="editor-info-card-toggle">More</label>)"},
    {"controls/ProjectPicker.css",
     R"(<uielement id="shown" class="project-picker-modal"><label id="subject" class="project-card-title">Harbor Town</label></uielement>)"},
    {"controls/VideoPlayerModal.css", R"(<label id="subject" class="vpm-title">Intro.mp4</label>)"},
};

std::string ShippedCss(const char* sheet)
{
    std::string css = ReadShippedCss("theme/tokens.css");
    css += '\n';
    css += ReadShippedCss(sheet);
    css += '\n';
    css += kTestCss;
    return css;
}

} // namespace

TEST(ShippedHeadingWeightTests, InspectorSubheadersTakeTheMediumFace)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <label id="section-subheader" class="inspector-section-subheader">Bounds</label>
  <label id="body-subheader" class="inspector-body-subheader">Tonemapping</label>
  <label id="field-label" class="inspector-label">Size</label>
</uielement>)";
    ASSERT_FALSE(ReadShippedCss("theme/inspector.css").empty()) << "theme/inspector.css not staged next to the test exe";

    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kXml, ShippedCss("theme/inspector.css")))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }
    const GameEngine::ResolvedStyle* sectionSubheader = fx.Style("section-subheader");
    const GameEngine::ResolvedStyle* bodySubheader = fx.Style("body-subheader");
    const GameEngine::ResolvedStyle* fieldLabel = fx.Style("field-label");
    ASSERT_NE(sectionSubheader, nullptr);
    ASSERT_NE(bodySubheader, nullptr);
    ASSERT_NE(fieldLabel, nullptr);

    EXPECT_EQ(sectionSubheader->Visual.FontWeight, kMediumWeight);
    EXPECT_EQ(bodySubheader->Visual.FontWeight, kMediumWeight);
    // The heading still steps up from the labels it heads.
    EXPECT_LT(fieldLabel->Visual.FontWeight, kMediumWeight);
}

TEST(ShippedHeadingWeightTests, SmallTitlesAndLabelsInEverySheetTakeTheMediumFace)
{
    for (const ShippedRuleCase& rule : kMediumRules)
    {
        SCOPED_TRACE(std::string(rule.Sheet) + ": " + rule.Markup);
        ASSERT_FALSE(ReadShippedCss(rule.Sheet).empty()) << "shipped stylesheet not staged next to the test exe";

        const std::string xml = std::string(R"(<uielement id="root">)") + rule.Markup + "</uielement>";
        IsolatedUIFixture fx;
        if (!fx.Build(1.0f, xml.c_str(), ShippedCss(rule.Sheet)))
        {
            if (!fx.DeviceAvailable())
                GTEST_SKIP() << fx.Diagnostic();
            FAIL() << fx.Diagnostic();
        }
        const GameEngine::ResolvedStyle* subject = fx.Style("subject");
        ASSERT_NE(subject, nullptr);
        EXPECT_EQ(subject->Visual.FontWeight, kMediumWeight);
    }
}
