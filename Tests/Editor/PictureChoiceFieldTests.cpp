// The picture choice field (the sky's Seen by row): one tile per choice from its layout file, with the
// choice's picture class and texts; a left click or an arrow key selects, the callback fires once per
// change and never for the tile already selected, and a right-click selects nothing. In a narrow
// inspector the tiles stack, so each keeps its name and its line readable beside its picture.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "Input/KeyCodes.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/PictureChoiceField.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;
using GameEngine::EditorUI::PictureChoiceField;
using InspectorLayoutTesting::InspectorLayoutFixture;

namespace
{

std::vector<PictureChoiceField::Choice> EyeAndCamera()
{
    return {{"Eye", "Faint stars look white.", "test-picture-eye"},
            {"Camera", "Every star keeps its color.", "test-picture-camera"}};
}

UIElement* ChildWithClass(const UIElement& parent, const char* className)
{
    for (const auto& child : parent.GetChildren())
    {
        if (child->HasClass(className))
            return child.get();
    }
    return nullptr;
}

void Press(UIElement& tile, int button = Input::kMouseButton_Left)
{
    UIEvent e{};
    e.Id = kEventMouseDown;
    e.Button = button;
    e.Target = &tile;
    e.CurrentTarget = &tile;
    tile.DispatchEvent(e);
}

void Key(UIElement& field, int key)
{
    UIEvent e{};
    e.Id = kEventKeyDown;
    e.Key = key;
    e.Target = &field;
    e.CurrentTarget = &field;
    field.OnEvent(e);
}

// Layout rounds to a fraction of a pixel; heights and edges are compared to this tolerance.
constexpr float kLayoutTolerance = 0.5f;

// The checklist's narrow inspector width.
constexpr uint32_t kNarrowInspectorW = 359;

// The index of the tile that shows as selected, or the tile count when none does.
size_t SelectedTile(const PictureChoiceField& field)
{
    const auto& tiles = field.GetChildren();
    for (size_t i = 0; i < tiles.size(); ++i)
    {
        if (tiles[i]->HasClass("picture-choice-tile-selected"))
            return i;
    }
    return tiles.size();
}

} // namespace

TEST(PictureChoiceField, BuildsOneTilePerChoiceFromItsLayout)
{
    PictureChoiceField field(EyeAndCamera(), 1);
    const auto& tiles = field.GetChildren();
    ASSERT_EQ(tiles.size(), 2u) << "PictureChoiceField.uxml is staged beside the test executable under "
                                   "Assets/UI/controls/PictureChoiceField/";
    const char* pictures[] = {"test-picture-eye", "test-picture-camera"};
    const char* names[] = {"Eye", "Camera"};
    const char* lines[] = {"Faint stars look white.", "Every star keeps its color."};
    for (size_t i = 0; i < tiles.size(); ++i)
    {
        const UIElement& tile = *tiles[i];
        EXPECT_TRUE(tile.HasClass("picture-choice-tile"));
        const UIElement* picture = ChildWithClass(tile, "picture-choice-picture");
        const UIElement* text = ChildWithClass(tile, "picture-choice-text");
        ASSERT_NE(picture, nullptr);
        ASSERT_NE(text, nullptr);
        EXPECT_TRUE(picture->HasClass(pictures[i]));
        auto* name = dynamic_cast<const Label*>(ChildWithClass(*text, "picture-choice-name"));
        auto* line = dynamic_cast<const Label*>(ChildWithClass(*text, "picture-choice-description"));
        ASSERT_NE(name, nullptr);
        ASSERT_NE(line, nullptr);
        EXPECT_EQ(name->GetText(), names[i]);
        EXPECT_EQ(line->GetText(), lines[i]);
        EXPECT_EQ(tile.HasClass("picture-choice-tile-selected"), i == 1) << "tile " << i;
    }
}

TEST(PictureChoiceField, APressSelectsAndNotifiesOncePerChange)
{
    PictureChoiceField field(EyeAndCamera(), 0);
    std::vector<size_t> picked;
    field.SetOnSelected([&picked](size_t index) { picked.push_back(index); });
    ASSERT_EQ(field.GetChildren().size(), 2u);
    UIElement& eye = *field.GetChildren()[0];
    UIElement& camera = *field.GetChildren()[1];

    Press(camera, Input::kMouseButton_Right);
    EXPECT_TRUE(picked.empty()) << "a right-click selects nothing";
    EXPECT_TRUE(eye.HasClass("picture-choice-tile-selected"));
    Press(camera);
    EXPECT_TRUE(camera.HasClass("picture-choice-tile-selected"));
    EXPECT_FALSE(eye.HasClass("picture-choice-tile-selected"));
    Press(camera);
    EXPECT_EQ(picked, (std::vector<size_t>{1})) << "the selected tile again is no change and no undo step";
}

// Up and Left go back, Down and Right go on, and both wrap at the ends, as in a group of radio buttons.
TEST(PictureChoiceField, TheArrowKeysMoveTheSelectionAndWrap)
{
    std::vector<PictureChoiceField::Choice> choices = EyeAndCamera();
    choices.push_back({"Third", "A third choice.", "test-picture-third"});
    PictureChoiceField field(std::move(choices), 0);
    ASSERT_EQ(field.GetChildren().size(), 3u);
    std::vector<size_t> picked;
    field.SetOnSelected([&picked](size_t index) { picked.push_back(index); });

    Key(field, Input::kKeyCode_Left);
    EXPECT_EQ(SelectedTile(field), 2u) << "Left on the first tile wraps to the last";
    Key(field, Input::kKeyCode_Down);
    EXPECT_EQ(SelectedTile(field), 0u) << "Down on the last tile wraps to the first";
    Key(field, Input::kKeyCode_Right);
    Key(field, Input::kKeyCode_Up);
    EXPECT_EQ(picked, (std::vector<size_t>{2, 0, 1, 0}));
}

// At the narrow inspector width a tile is not squeezed beside its sibling: its name stays on one
// line and its line wraps to two lines at most, inside the field.
TEST(PictureChoiceFieldLayout, InANarrowInspectorEachTileKeepsItsNameAndLine)
{
    auto root = std::make_unique<UIElement>();
    PictureChoiceField* field =
        InspectorUI::AddFieldRow(root.get(), "Seen by", std::make_unique<PictureChoiceField>(EyeAndCamera(), 0));
    ASSERT_NE(field, nullptr);

    InspectorLayoutFixture fixture;
    fixture.ViewportW = kNarrowInspectorW;
    fixture.ExtraSheets = {"Assets/UI/theme/core.css", "Assets/UI/controls/PictureChoiceField/PictureChoiceField.css"};
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;

    const float fieldRight = field->GetLayoutX() + field->GetLayoutWidth();
    for (const auto& tile : field->GetChildren())
    {
        const UIElement* text = ChildWithClass(*tile, "picture-choice-text");
        ASSERT_NE(text, nullptr);
        const UIElement* name = ChildWithClass(*text, "picture-choice-name");
        const UIElement* line = ChildWithClass(*text, "picture-choice-description");
        ASSERT_NE(name, nullptr);
        ASSERT_NE(line, nullptr);
        const float lineHeight = name->GetLayoutHeight();
        ASSERT_GT(lineHeight, 0.0f) << "no text measured, so a wrap cannot be told from a fixture without a font";

        EXPECT_GT(text->GetLayoutWidth(), 0.0f) << "the tile shows its picture and no text";
        EXPECT_LE(line->GetLayoutHeight(), 2.0f * lineHeight + kLayoutTolerance)
            << "the line wraps into a column of words beside the picture";
        EXPECT_LE(tile->GetLayoutX() + tile->GetLayoutWidth(), fieldRight + kLayoutTolerance)
            << "the tile runs out of the field";
    }
}
