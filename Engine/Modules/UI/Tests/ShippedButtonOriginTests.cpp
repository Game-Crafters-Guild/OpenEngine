// A Button resolved from the shipped sheets: the editor's sheets have the Author origin and the UI
// module's Button.css has the Control origin, as the UIStyle asset parser gives them. A theme rule
// of equal specificity beats Button.css, so these cases fail when the theme copies a Button rule.

#include "UI/Parsers/CSSParser.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"
#include "UI/UIStyle.h"

#include "Core/ApplicationPaths.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace GameEngine;
using GameEngine::UIParsing::CSSParser;
using GameEngine::UIParsing::ElementState;

namespace
{

std::string ReadShippedCss(const std::string& relative)
{
    std::ifstream in(ApplicationPaths::GetExecutableDirectory() / "Assets" / "UI" / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Reference elements read the theme tokens the Button rules must resolve to.
constexpr char kReferenceCss[] = R"(
.ref-error { color: var(--ui_color_error_text); }
.ref-disabled { background-color: var(--ui_color_bg_hover); }
)";

class ShippedButtonOriginTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        for (const char* path : m_AuthorSheets)
            ASSERT_NO_FATAL_FAILURE(AddShippedSheet(path, StyleOrigin::Author));
        ASSERT_NO_FATAL_FAILURE(AddShippedSheet("controls/Button.css", StyleOrigin::Control));

        Stylesheet reference{};
        ASSERT_TRUE(CSSParser::ParseStylesFromString(kReferenceCss, reference));
        m_Sheets.push_back(std::make_unique<Stylesheet>(std::move(reference)));

        for (const auto& sheet : m_Sheets)
            m_SheetPointers.push_back(sheet.get());
        m_RootStyle = CSSParser::ComputeStyleFor(m_Root, m_SheetPointers, ElementState{},
                                                 static_cast<const ResolvedStyle*>(nullptr));
    }

    // The style of a child of the :root element with these classes in this state.
    ResolvedStyle Resolve(const std::vector<std::string>& classes, const ElementState& state)
    {
        auto child = std::make_unique<UIElement>();
        for (const std::string& name : classes)
            child->AddClass(name);
        UIElement* element = child.get();
        m_Root.AddChild(std::move(child));
        return CSSParser::ComputeStyleFor(*element, m_SheetPointers, state, &m_RootStyle);
    }

    void AddShippedSheet(const char* path, StyleOrigin origin)
    {
        const std::string text = ReadShippedCss(path);
        ASSERT_FALSE(text.empty()) << "shipped stylesheet not staged next to the test exe: " << path;
        Stylesheet sheet{};
        ASSERT_TRUE(CSSParser::ParseStylesFromString(text, sheet)) << path;
        sheet.Origin = origin;
        m_Sheets.push_back(std::make_unique<Stylesheet>(std::move(sheet)));
    }

    // The Author sheets, added before Button.css.
    std::vector<const char*> m_AuthorSheets{"theme/tokens.css", "theme/core.css", "theme/widgets.css",
                                            "theme/inspector.css", "panels/LogPanel/LogPanel.css"};
    UIElement m_Root;
    std::vector<std::unique_ptr<Stylesheet>> m_Sheets;
    std::vector<const Stylesheet*> m_SheetPointers;
    ResolvedStyle m_RootStyle;
};

} // namespace

TEST_F(ShippedButtonOriginTests, LogErrorFilterHasTheErrorColorOnItsLabelAndBorder)
{
    const uint32_t errorColor = Resolve({"ref-error"}, ElementState{}).Visual.Color;
    ASSERT_NE(errorColor, Resolve({}, ElementState{}).Visual.Color) << "the theme defines no --ui_color_error_text";

    const ResolvedStyle filter =
        Resolve({"button", "small", "secondary", "log-filter", "log-filter-error", "active"}, ElementState{});
    EXPECT_EQ(filter.Visual.Color, errorColor) << "the shown Errors filter lost its red label";
    EXPECT_EQ(filter.Visual.BorderColor.Top, errorColor) << "the shown Errors filter lost its red border";
}

TEST_F(ShippedButtonOriginTests, DisabledSecondaryButtonKeepsTheDisabledFillUnderThePointer)
{
    const uint32_t disabledFill = Resolve({"ref-disabled"}, ElementState{}).Visual.BackgroundColor;
    ASSERT_NE(disabledFill >> 24, 0u) << "the theme defines no --ui_color_bg_hover";

    ElementState disabledHovered{};
    disabledHovered.Disabled = true;
    disabledHovered.Enabled = false;
    disabledHovered.Hover = true;
    EXPECT_EQ(Resolve({"button", "secondary"}, disabledHovered).Visual.BackgroundColor, disabledFill)
        << "a disabled secondary button shows the hover fill";
}

TEST_F(ShippedButtonOriginTests, InspectorButtonHasTheButtonFillAndBorderAtRestAndHovered)
{
    ElementState hovered{};
    hovered.Hover = true;
    for (const ElementState& state : {ElementState{}, hovered})
    {
        const ResolvedStyle button = Resolve({"button"}, state);
        const ResolvedStyle inspectorButton = Resolve({"button", "inspector-button"}, state);
        EXPECT_EQ(inspectorButton.Visual.BackgroundColor, button.Visual.BackgroundColor)
            << "theme/inspector.css sets its own fill on .inspector-button (hover " << state.Hover << ")";
        EXPECT_EQ(inspectorButton.Visual.BorderColor.Top, button.Visual.BorderColor.Top)
            << "theme/inspector.css sets its own border on .inspector-button (hover " << state.Hover << ")";
        EXPECT_EQ(inspectorButton.Visual.Color, button.Visual.Color);
    }
}

namespace
{

// The sheets that style the shortcut remove, bookmark remove and todo delete buttons.
class ShippedRowButtonTests : public ShippedButtonOriginTests
{
  protected:
    ShippedRowButtonTests()
    {
        m_AuthorSheets = {"theme/tokens.css", "theme/core.css", "theme/widgets.css", "panels/SettingsPanel.css",
                          "theme/bookmarks.css", "theme/todos.css"};
    }
};

} // namespace

// The icon button variant draws its states: no fill, the glyph dimmed at rest and full under the pointer.
TEST_F(ShippedRowButtonTests, IconButtonGlyphIsFullOnlyUnderThePointer)
{
    ElementState hovered{};
    hovered.Hover = true;
    const ResolvedStyle rest = Resolve({"button", "icon-button"}, ElementState{});
    const ResolvedStyle hover = Resolve({"button", "icon-button"}, hovered);
    EXPECT_EQ(rest.Visual.BackgroundColor >> 24, 0u) << "the icon button has a fill at rest";
    EXPECT_EQ(hover.Visual.BackgroundColor >> 24, 0u) << "the icon button has a fill under the pointer";
    EXPECT_LT(rest.Visual.Opacity, 1.0f) << "the icon button glyph has no rest state";
    EXPECT_EQ(hover.Visual.Opacity, 1.0f) << "the icon button glyph does not brighten under the pointer";
}

// A host rule of equal specificity beats Button.css, so a host fill, border, color or opacity on
// these buttons hides the icon button's states. The host sets only their size.
TEST_F(ShippedRowButtonTests, RowRemoveButtonsHaveTheIconButtonLookAtRestAndHovered)
{
    ElementState hovered{};
    hovered.Hover = true;
    for (const char* hostClass : {"shortcut-remove-button", "bookmark-remove", "todo-delete"})
    {
        for (const ElementState& state : {ElementState{}, hovered})
        {
            const ResolvedStyle iconButton = Resolve({"button", "icon-button"}, state);
            const ResolvedStyle rowButton = Resolve({"button", "icon-button", hostClass}, state);
            EXPECT_EQ(rowButton.Visual.BackgroundColor, iconButton.Visual.BackgroundColor)
                << hostClass << " has a fill of its own (hover " << state.Hover << ")";
            EXPECT_EQ(rowButton.Visual.BorderColor.Top, iconButton.Visual.BorderColor.Top)
                << hostClass << " has a border of its own (hover " << state.Hover << ")";
            EXPECT_EQ(rowButton.Visual.Color, iconButton.Visual.Color)
                << hostClass << " has a text color of its own (hover " << state.Hover << ")";
            EXPECT_EQ(rowButton.Visual.Opacity, iconButton.Visual.Opacity)
                << hostClass << " has an opacity of its own (hover " << state.Hover << ")";
        }
    }
}
