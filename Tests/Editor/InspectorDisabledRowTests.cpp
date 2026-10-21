// A row disabled through its control: the control and every element inside it take the disabled
// state, so a composite control (an enum field around its dropdown) draws its own disabled look, and
// enabling the row again restores it.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "Inspectors/InspectorUIHelpers.h"

#include "UI/Controls/Dropdown.h"
#include "UI/Controls/EnumField.h"
#include "UI/UIElement.h"

#include <memory>

using GameEngine::DisplayMode;
using GameEngine::EnumEntry;
using GameEngine::UIElement;
using InspectorLayoutTesting::FindByClass;
using InspectorLayoutTesting::InspectorLayoutFixture;

namespace
{
enum class Mode
{
    First,
    Second,
};

constexpr EnumEntry<Mode> kModes[] = {
    {Mode::First, "First"},
    {Mode::Second, "Second"},
};
} // namespace

TEST(InspectorDisabledRow, AnEnumRowDisabledThroughItsControlDrawsTheDisabledDropdown)
{
    auto root = std::make_unique<UIElement>();
    auto* field = GameEngine::InspectorUI::AddEnumRow(root.get(), "Mode", kModes, Mode::First);
    ASSERT_NE(field, nullptr);
    GameEngine::InspectorUI::DisableRowOfControl(field, "Read-only here");

    InspectorLayoutFixture fixture;
    fixture.ExtraSheets = {"Assets/UI/controls/Dropdown.css"};
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;

    GameEngine::Dropdown* dropdown = field->GetDropdown();
    UIElement* chevron = FindByClass(dropdown, "dropdown-chevron");
    ASSERT_NE(chevron, nullptr);
    EXPECT_FALSE(dropdown->IsEnabled());
    EXPECT_EQ(chevron->GetResolvedStyle().Layout.DisplayMode, DisplayMode::None)
        << "a disabled dropdown drops its chevron (Dropdown.css)";

    GameEngine::InspectorUI::SetRowOfControlEnabled(field, true);
    fixture.Ui->Update(1.0f / 60.0f, true);
    EXPECT_TRUE(dropdown->IsEnabled());
    EXPECT_NE(chevron->GetResolvedStyle().Layout.DisplayMode, DisplayMode::None);
}
