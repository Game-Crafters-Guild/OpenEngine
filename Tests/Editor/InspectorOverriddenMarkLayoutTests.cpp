// The override marks on the editor's own stylesheets: the teal edge of an overridden row and of a
// header sits at one x and takes no space from their content, and a removed component's header is
// faded and has no options.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Prefabs/PrefabRemovedComponents.h"
#include "UI/InspectorSection.h"

#include "UI/UIElement.h"

#include <memory>
#include <string>
#include <vector>

using GameEngine::InspectorSection;
using GameEngine::UIElement;
using InspectorLayoutTesting::FindByClass;
using InspectorLayoutTesting::InspectorLayoutFixture;

namespace
{

// A section holding one row labeled `label`; the row is marked when `overridden`.
InspectorSection* AddSectionWithRow(UIElement& root, const std::string& title, bool overridden)
{
    auto section = std::make_unique<InspectorSection>(title);
    if (overridden)
        section->SetOverridden(true);
    UIElement* row = GameEngine::InspectorUI::AddRow(section->GetContentRoot());
    GameEngine::InspectorUI::AddLabel(row, title);
    if (overridden)
        GameEngine::InspectorUI::SetRowOverridden(row, true);
    InspectorSection* raw = section.get();
    root.AddChild(std::move(section));
    return raw;
}

// The left edge of the first child of the first element of `section` with class `className`.
float ContentLeft(InspectorSection& section, const std::string& className)
{
    UIElement* element = FindByClass(&section, className);
    if (!element || element->GetChildren().empty())
        return -1.0f;
    return element->GetChildren().front()->GetLayoutX();
}

// Lays out a plain and an overridden section in an Inspector panel root with the extra class
// `rootClass`, and compares where the content of their header rows and their rows starts.
void ExpectTheMarksKeepTheContentInPlace(const std::string& rootClass)
{
    auto root = std::make_unique<UIElement>();
    root->AddClass("inspector-panel");
    if (!rootClass.empty())
        root->AddClass(rootClass);
    InspectorSection* plain = AddSectionWithRow(*root, "Plain", false);
    InspectorSection* marked = AddSectionWithRow(*root, "Marked", true);

    InspectorLayoutFixture fixture;
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;

    EXPECT_FLOAT_EQ(ContentLeft(*marked, "inspector-section-header-row"),
                    ContentLeft(*plain, "inspector-section-header-row"))
        << "the header's edge moves its content";
    EXPECT_FLOAT_EQ(ContentLeft(*marked, "inspector-row"), ContentLeft(*plain, "inspector-row"))
        << "the row's edge moves its content";
    const UIElement* headerRow = FindByClass(marked, "inspector-section-header-row");
    const UIElement* rowMark = FindByClass(FindByClass(marked, "inspector-row"), "inspector-override-mark");
    ASSERT_NE(rowMark, nullptr);
    EXPECT_FLOAT_EQ(headerRow->GetResolvedStyle().Layout.BorderWidth.Left, 3.0f) << "the header has no edge";
    EXPECT_FLOAT_EQ(rowMark->GetLayoutWidth(), 3.0f);
    EXPECT_FLOAT_EQ(rowMark->GetLayoutX(), headerRow->GetLayoutX()) << "the row's edge and the header's are apart";
}

} // namespace

TEST(InspectorOverriddenMarkLayout, TheEdgeOfAnOverriddenRowAndHeaderSitsAtOneXAndKeepsTheirContentInPlace)
{
    ExpectTheMarksKeepTheContentInPlace("");
}

TEST(InspectorOverriddenMarkLayout, TheEdgeKeepsTheContentInPlaceInTheFilledSectionsLook)
{
    ExpectTheMarksKeepTheContentInPlace("inspector-filled-sections");
}

TEST(InspectorOverriddenMarkLayout, ARemovedComponentHasAFadedTitleAMutedEdgeAndNoOptions)
{
    auto root = std::make_unique<UIElement>();
    InspectorSection* live = AddSectionWithRow(*root, "Light", false);
    auto removedHost = std::make_unique<GameEngine::Editor::PrefabRemovedComponents>(std::vector<std::string>{"MeshRenderer"});
    UIElement* removed = removedHost.get();
    root->AddChild(std::move(removedHost));

    InspectorLayoutFixture fixture;
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;

    const UIElement* liveTitle = FindByClass(live, "inspector-section-header");
    const UIElement* removedTitle = FindByClass(removed, "inspector-section-header");
    ASSERT_NE(liveTitle, nullptr);
    ASSERT_NE(removedTitle, nullptr);
    EXPECT_NE(removedTitle->GetResolvedStyle().Visual.Color, liveTitle->GetResolvedStyle().Visual.Color)
        << "the removed component's title reads as a live one";
    const UIElement* options = FindByClass(removed, "inspector-section-header-options");
    ASSERT_NE(options, nullptr);
    EXPECT_TRUE(options->HasClass("hidden")) << "the removed header offers options it has not got";
    EXPECT_NE(FindByClass(removed, "inspector-section-header-row-overridden"), nullptr);
    const UIElement* icon = FindByClass(removed, "inspector-section-icon");
    ASSERT_NE(icon, nullptr);
    EXPECT_FALSE(icon->HasClass("hidden")) << "the removed header has no icon";
    EXPECT_LT(icon->GetResolvedStyle().Visual.LocalOpacity, 1.0f);
}
