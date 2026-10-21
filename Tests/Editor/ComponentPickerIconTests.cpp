// The Add Component picker draws a component's icon by its inspector icon class, the one its
// registered traits name, so a component an editor module registers shows its own icon in the
// picker without the picker knowing it; a component with no icon of its own shows the generic
// glyph. A selected row draws that icon in its text color.

#include <gtest/gtest.h>

#include "Editor/Entities/EditorComponentTraits.h"
#include "InspectorLayoutFixture.h"
#include "Inspectors/ComponentIconClass.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/UIElement.h"

#include <memory>
#include <span>
#include <vector>

namespace
{
using GameEngine::ECS::ComponentTypeId;
namespace ed = GameEngine::Editor;

// Type ids no engine component has.
constexpr ComponentTypeId kWithTraits = 0xC1C0;
constexpr ComponentTypeId kWithoutTraits = 0xC1C1;
} // namespace

TEST(ComponentPickerIcon, IsTheRegisteredIconClassOrTheGenericGlyph)
{
    ed::EditorComponentTraits traits;
    traits.InspectorIconClass = "inspector-section-icon-navigation-surface";
    ed::EditorComponentTraitsRegistry::Get().Register(kWithTraits, std::move(traits));

    const GameEngine::SearchIcon registered =
        ed::ComponentPickerIcon(std::span<const ComponentTypeId>(&kWithTraits, 1), "Navigation");
    EXPECT_EQ(registered.CssClass, "inspector-section-icon-navigation-surface");
    EXPECT_FALSE(registered.HasImage()) << "an image path would draw over the class's icon";

    const GameEngine::SearchIcon generic =
        ed::ComponentPickerIcon(std::span<const ComponentTypeId>(&kWithoutTraits, 1), "Navigation");
    EXPECT_EQ(generic.CssClass, "inspector-section-icon-default");
    EXPECT_FALSE(generic.HasImage());
}

// On the selected row of a search dialog a component icon is drawn in the row's text color: the
// resting icon tint does not read on the selection's accent background. Unselected rows keep the
// resting tint. The rows are built as SearchDialog builds them, over the editor's sheets.
TEST(ComponentPickerIcon, TheSelectedRowDrawsItsIconInTheTextColor)
{
    using GameEngine::Label;
    using GameEngine::UIElement;
    auto root = std::make_unique<UIElement>();
    auto dialog = std::make_unique<UIElement>();
    dialog->AddClass("search-dialog");
    dialog->AddClass("open");
    std::vector<UIElement*> icons;
    std::vector<Label*> labels;
    for (const bool selected : {true, false})
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("search-result-row");
        if (selected)
            row->AddClass("selected");
        auto icon = std::make_unique<UIElement>();
        icon->AddClass("search-result-icon");
        icon->AddClass("inspector-section-icon");
        icon->AddClass("inspector-section-icon-navigation-surface");
        icons.push_back(icon.get());
        auto label = std::make_unique<Label>();
        label->AddClass("search-result-label");
        label->SetText("Navigation Surface");
        labels.push_back(label.get());
        row->AddChild(std::move(icon));
        row->AddChild(std::move(label));
        dialog->AddChild(std::move(row));
    }
    root->AddChild(std::move(dialog));

    InspectorLayoutTesting::InspectorLayoutFixture fixture;
    fixture.ExtraSheets = {"Assets/UI/controls/SearchDialog.css"};
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;

    const uint32_t selectedText = labels[0]->GetResolvedStyle().Visual.Color;
    const uint32_t restingTint = icons[1]->GetResolvedStyle().Visual.BackgroundImage.Tint;
    ASSERT_NE(restingTint, selectedText) << "the resting tint is the text color; this test is vacuous";
    EXPECT_EQ(icons[0]->GetResolvedStyle().Visual.BackgroundImage.Tint, selectedText)
        << "the selected row's icon keeps the resting tint on the selection color";
}
