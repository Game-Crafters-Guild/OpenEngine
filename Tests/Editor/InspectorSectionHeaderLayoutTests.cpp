// An inspector sub-heading is a label with no box of its own, so the only thing that makes it a
// heading is that it has a height. That is easy to lose and impossible to see structurally: a
// label with a main-axis flex basis of 0 fills the row it was written for and collapses to
// nothing in a section body, while staying in the tree with the right text under the right
// parent. The two roles therefore keep two classes — `.inspector-section-header` for the
// component title inside the fixed-height `.inspector-section-header-row`, and
// `.inspector-section-subheader` for a heading over a group of rows — and this file measures both
// over the REAL editor stylesheets and the REAL production widgets: InspectorSection builds the
// header row, InspectorUI::AddTextBlock builds the sub-heading.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/Label.h"
#include "UI/InspectorSection.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using GameEngine::InspectorSection;
using GameEngine::UIElement;

namespace
{

// A body text row is a line of text plus its own padding, so a heading holding one line of its
// own is a good fraction of it. Well under that is a sliver — collapsed by something, even if not
// all the way to zero. Deliberately loose: the two carry different font sizes and boxes, and both
// are free to be restyled.
constexpr float kMinShareOfABodyRow = 0.5f;

// The header row's fixed-size children (dot, icon, options, chevron) come to well under half of
// any usable inspector width, so a title taking its row's slack clears this and a content-sized
// one does not.
constexpr float kMinShareOfItsRow = 0.5f;

// Layout rounds to a fraction of a pixel; extents are compared to this tolerance.
constexpr float kLayoutTolerance = 0.5f;

using InspectorLayoutTesting::FindByClass;
using InspectorLayoutTesting::InspectorLayoutFixture;

} // namespace

// The defect #1457 reported: every sub-heading in every component inspector laid out at height 0,
// so the user saw "Moon size / Moon exposure" with no "Moon" over them.
TEST(InspectorSectionHeaderLayout, ASubHeadingInASectionBodyHasAHeight)
{
    auto root = std::make_unique<UIElement>();
    auto section = std::make_unique<InspectorSection>("Sky Environment");
    InspectorSection* sectionRaw = section.get();
    root->AddChild(std::move(section));

    UIElement* body = sectionRaw->GetContentRoot();
    ASSERT_NE(body, nullptr);

    // The production call every affected inspector makes, and a body row beside it as the control.
    GameEngine::InspectorUI::AddTextBlock(body, "Moon", "inspector-section-subheader");
    GameEngine::InspectorUI::AddTextBlock(body, "Moon", "inspector-text");

    InspectorLayoutFixture fixture;
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;

    UIElement* subHeading = FindByClass(body, "inspector-section-subheader");
    UIElement* control = FindByClass(body, "inspector-text");
    ASSERT_NE(subHeading, nullptr) << "the sub-heading is not in the tree at all";
    ASSERT_NE(control, nullptr);

    // Positive control: text in this body measures to something. Without it a zero below would
    // read as the defect when it is really a fixture with no font.
    ASSERT_GT(control->GetLayoutHeight(), 0.0f)
        << "an ordinary inspector text row measured 0 high, so this fixture cannot tell a "
           "collapsed sub-heading from a fixture that measures no text; this test is vacuous";

    EXPECT_GT(subHeading->GetLayoutHeight(), 0.0f)
        << "the sub-heading laid out at height 0 — it is in the tree and draws nothing. A "
           "main-axis flex basis does this: it fills a header row and collapses the same label in "
           "a section body, which is a column.";
    EXPECT_GE(subHeading->GetLayoutHeight(), control->GetLayoutHeight() * kMinShareOfABodyRow)
        << "the sub-heading is a sliver rather than a line of text";
}

// The other role, and the reason the collapse above survived so long: the component title has to
// keep taking its row's slack, or the header bar's text bunches up on the left with the options
// affordance floating away from it.
TEST(InspectorSectionHeaderLayout, TheComponentTitleStillFillsItsHeaderRow)
{
    auto root = std::make_unique<UIElement>();
    auto section = std::make_unique<InspectorSection>("Post Process Volume");
    InspectorSection* sectionRaw = section.get();
    root->AddChild(std::move(section));

    InspectorLayoutFixture fixture;
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;

    UIElement* row = FindByClass(sectionRaw, "inspector-section-header-row");
    ASSERT_NE(row, nullptr);
    UIElement* title = FindByClass(row, "inspector-section-header");
    UIElement* options = FindByClass(row, "inspector-section-header-options");
    ASSERT_NE(title, nullptr);
    ASSERT_NE(options, nullptr);

    ASSERT_GT(row->GetLayoutWidth(), 0.0f) << "the header row has no width; this test is vacuous";
    EXPECT_GT(title->GetLayoutHeight(), 0.0f) << "the component title lost its height";

    // Content-sized, the title would be a few dozen px of a row hundreds wide. Half the row is
    // well clear of that and well clear of the dot/icon/options the row also has to fit.
    EXPECT_GT(title->GetLayoutWidth(), row->GetLayoutWidth() * kMinShareOfItsRow)
        << "the component title stopped taking its row's slack, so the header bar's title and its "
           "options affordance no longer sit at opposite ends";
    EXPECT_LE(title->GetLayoutX() + title->GetLayoutWidth(), options->GetLayoutX())
        << "the title overruns the options affordance beside it";
}

// A header badge sits after the title's text, never over it, and a badged title keeps to one line of
// the fixed-height header. At 360 px a long title such as "Navigation Surface" runs past the 37% value
// column the badge starts on when the title is short; at 260 px it no longer fits beside its badge
// and ends in an ellipsis rather than wrapping onto a second line the header clips. The title's text
// width is measured on a content-sized label carrying the title's own class beside the section.
TEST(InspectorSectionHeaderLayout, AHeaderBadgeFollowsTheTitleOnOneLineAtNarrowInspectors)
{
    const std::string titleText = "Navigation Surface";
    for (const uint32_t viewportWidth : {360u, 260u})
    {
        auto root = std::make_unique<UIElement>();
        auto section = std::make_unique<InspectorSection>(titleText);
        InspectorSection* sectionRaw = section.get();
        sectionRaw->SetHeaderIconClass("inspector-section-icon-unloaded");
        sectionRaw->SetHeaderBadge("Failed", "", InspectorSection::BadgeKind::Error);
        root->AddChild(std::move(section));
        auto measureRow = std::make_unique<UIElement>();
        measureRow->AddClass("test-measure-row");
        auto measure = std::make_unique<GameEngine::Label>();
        measure->AddClass("inspector-section-header");
        measure->AddClass("test-title-measure");
        measure->SetText(titleText);
        GameEngine::Label* measureRaw = measure.get();
        measureRow->AddChild(std::move(measure));
        root->AddChild(std::move(measureRow));

        InspectorLayoutFixture fixture;
        fixture.ViewportW = viewportWidth;
        // The enable dot's own sheet gives it the footprint it has in the editor, which places the title.
        fixture.ExtraSheets.push_back("Assets/UI/controls/EnableDot/EnableDot.css");
        if (!fixture.Build(std::move(root)))
            GTEST_SKIP() << fixture.Diagnostic;
        // Content-sized and on one line; the font is the title rule's own.
        GameEngine::Stylesheet sheet{};
        ASSERT_TRUE(GameEngine::UIParsing::CSSParser::ParseStylesFromString(
            ".test-measure-row { display: flex; flex-direction: row; } "
            ".test-title-measure { flex: 0 0 auto; width: auto; max-width: none; white-space: nowrap; }",
            sheet));
        sheet.SourceName = "test-title-measure";
        fixture.Ui->AddStylesheet(std::make_shared<const GameEngine::Stylesheet>(std::move(sheet)));
        for (int i = 0; i < 3; ++i)
            fixture.Ui->Update(1.0f / 60.0f, /*interactive=*/true);

        UIElement* row = FindByClass(sectionRaw, "inspector-section-header-row");
        ASSERT_NE(row, nullptr);
        UIElement* title = FindByClass(row, "inspector-section-header");
        UIElement* badge = FindByClass(row, "inspector-section-header-badge");
        UIElement* options = FindByClass(row, "inspector-section-header-options");
        ASSERT_NE(title, nullptr);
        ASSERT_NE(badge, nullptr);
        ASSERT_NE(options, nullptr);
        const float textWidth = measureRaw->GetLayoutWidth();
        const float lineHeight = measureRaw->GetLayoutHeight();
        ASSERT_GT(textWidth, 0.0f) << "the title's text measured no width; this test is vacuous";
        ASSERT_GT(badge->GetLayoutWidth(), 0.0f) << "the badge has no width; this test is vacuous";

        // The title's text runs to the end of its content box, or to the end of the text when that is
        // shorter (an ellipsis ends a text the box cannot hold).
        const float contentRight = title->GetLayoutX() + title->GetLayoutWidth() - title->GetLayoutPadding().Right;
        const float textRight = std::min(title->GetLayoutX() + textWidth, contentRight);
        EXPECT_GE(badge->GetLayoutX(), textRight) << "the badge covers the title's text at " << viewportWidth << " px";
        EXPECT_LE(title->GetLayoutHeight(), lineHeight + kLayoutTolerance)
            << "the title wraps onto a second line the header clips at " << viewportWidth << " px";
        EXPECT_LE(badge->GetLayoutX() + badge->GetLayoutWidth(), options->GetLayoutX())
            << "the badge runs into the options affordance at " << viewportWidth << " px";
        EXPECT_LE(options->GetLayoutX() + options->GetLayoutWidth(), row->GetLayoutX() + row->GetLayoutWidth())
            << "the options affordance is pushed out of its row at " << viewportWidth << " px";
    }
}

// A component with no on/off state keeps the dot's footprint as an invisible placeholder. A click
// there has to reach the header, which collapses the section, rather than land on a dot that does
// nothing.
TEST(InspectorSectionHeaderLayout, APlaceholderDotLetsAClickThroughToTheHeader)
{
    auto root = std::make_unique<UIElement>();
    auto section = std::make_unique<InspectorSection>("Transform");
    InspectorSection* sectionRaw = section.get();
    sectionRaw->SetEnabledTogglePlaceholder(true);
    GameEngine::InspectorUI::AddTextBlock(sectionRaw->GetContentRoot(), "Local Position", "inspector-text");
    root->AddChild(std::move(section));

    InspectorLayoutFixture fixture;
    fixture.ExtraSheets.push_back("Assets/UI/controls/EnableDot/EnableDot.css");
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;

    UIElement* dot = FindByClass(sectionRaw, "inspector-section-dot");
    ASSERT_NE(dot, nullptr);
    ASSERT_GT(dot->GetLayoutWidth(), 0.0f) << "the placeholder keeps no footprint; this test is vacuous";

    UIElement* title = FindByClass(sectionRaw, "inspector-section-header");
    ASSERT_NE(title, nullptr);
    const auto clickAt = [&fixture](const UIElement& target)
    {
        fixture.Ui->OnMouseMove(target.GetLayoutX() + target.GetLayoutWidth() * 0.5f,
                                target.GetLayoutY() + target.GetLayoutHeight() * 0.5f);
        fixture.Ui->Update(1.0f / 60.0f, /*interactive=*/true);
        fixture.Ui->OnMouseButton(0, true);
        fixture.Ui->Update(1.0f / 60.0f, /*interactive=*/true);
        fixture.Ui->OnMouseButton(0, false);
        fixture.Ui->Update(1.0f / 60.0f, /*interactive=*/true);
    };

    // Positive control: a click on the title collapses the section, and a second expands it.
    clickAt(*title);
    ASSERT_TRUE(sectionRaw->IsCollapsed()) << "a title click does not collapse here; this test is vacuous";
    clickAt(*title);
    ASSERT_FALSE(sectionRaw->IsCollapsed());

    clickAt(*dot);
    EXPECT_TRUE(sectionRaw->IsCollapsed()) << "the placeholder takes the click, so the header never sees it";
}

// The title class belongs to the section widget that builds the bar. An inspector that reaches for
// it by hand gets a heading styled as a bar title in a place with no bar — which is how every
// sub-heading in the editor ended up sharing the row's layout in the first place. Source-level,
// because the point is that no inspector writes the string, not that some particular panel lays
// out correctly.
TEST(InspectorSectionHeaderLayout, NoInspectorReachesForTheSectionTitleClassByHand)
{
    // Both trees: the inspector helpers that emit these headings are header-only
    // (InspectorUIHelpers.h), so scanning only .cpp under Source/ would leave the shape this test
    // exists to catch reachable from a header.
    const std::filesystem::path editor = std::filesystem::path(GE_EDITOR_SOURCE_DIR);
    ASSERT_TRUE(std::filesystem::is_directory(editor / "Source"))
        << "the editor sources did not read; this test is vacuous";
    ASSERT_TRUE(std::filesystem::is_directory(editor / "Include"))
        << "the editor headers did not read; this test is vacuous";

    std::vector<std::string> offenders;
    size_t scanned = 0;
    for (const char* subtree : {"Source", "Include"})
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(editor / subtree))
        {
            if (!entry.is_regular_file())
                continue;
            const std::filesystem::path extension = entry.path().extension();
            if (extension != ".cpp" && extension != ".h")
                continue;
            // The section widget owns the class and is the one place allowed to name it.
            if (entry.path().filename() == "InspectorSection.cpp")
                continue;
            ++scanned;
            std::ifstream in(entry.path(), std::ios::binary);
            if (!in)
                continue;
            std::ostringstream ss;
            ss << in.rdbuf();
            if (ss.str().find("\"inspector-section-header\"") != std::string::npos)
                offenders.push_back(entry.path().filename().string());
        }
    }

    ASSERT_GT(scanned, 0u) << "no editor sources were scanned; this test is vacuous";
    EXPECT_TRUE(offenders.empty())
        << "these add the section title class by hand instead of the sub-heading class, so their "
           "headings are styled for a bar they are not in: "
        << [&offenders] {
               std::string joined;
               for (const std::string& name : offenders)
                   joined += (joined.empty() ? "" : ", ") + name;
               return joined;
           }();
}
