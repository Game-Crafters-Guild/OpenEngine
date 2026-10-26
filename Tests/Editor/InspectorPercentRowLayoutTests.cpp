// The percent row a modifier effect's rows share (AddComponentMultiplierPercentRow): its value box
// holds its widest value, "100 %", at the inspector's default width, and the "%" follows the number. Lays the real row out over the
// real editor stylesheets and measures the value in the field's own text face.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "Inspectors/InspectorComponentRowHelpers.h"

#include "Components/Terrain/TerrainModifierEffects.h"
#include "UI/Controls/FloatField.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <memory>
#include <string>

using namespace GameEngine;
using InspectorLayoutTesting::FindByClass;
using InspectorLayoutTesting::InspectorLayoutFixture;

namespace
{

// The inspector's default width in logical px (PendingAssetHeaderLayoutTests uses the same).
constexpr uint32_t kDefaultInspectorWidth = 383;
// The suffix slot's left margin (UI/controls/FieldSuffix/FieldSuffix.css), plus a pixel of rounding.
constexpr float kMaxValueToSuffixGap = 5.0f;

// Lays out one percent row showing `initial` over the real editor stylesheets; returns the row's
// value field, or null with the fixture's diagnostic when the fixture cannot build.
FloatField* LayOutPercentRow(InspectorLayoutFixture& fixture, float initial)
{
    auto panel = std::make_unique<UIElement>();
    panel->AddClass("inspector-panel");
    // Nothing edits the row, so it needs no world.
    const InspectorDrag::SliderWithFloatValueRow row =
        InspectorDrag::AddComponentMultiplierPercentRow<Components::TerrainGrassEffect>(
            panel.get(), initial, nullptr, ECS::EntityHandle{}, nullptr, [](Components::TerrainGrassEffect&, float) {},
            [](float) { return std::string("Height"); }, nullptr);
    fixture.ViewportW = kDefaultInspectorWidth;
    // The global theme sheets that style a field with a suffix, then the suffix label's own sheet.
    fixture.ExtraSheets = {"Assets/UI/theme/core.css", "Assets/UI/theme/widgets.css",
                           "Assets/UI/controls/FieldSuffix/FieldSuffix.css"};
    return fixture.Build(std::move(panel)) ? row.ValueField : nullptr;
}

} // namespace

TEST(InspectorPercentRowLayout, TheValueBoxHoldsOneHundredPercent)
{
    InspectorLayoutFixture fixture;
    FloatField* field = LayOutPercentRow(fixture, 1.0f);
    if (!field)
        GTEST_SKIP() << fixture.Diagnostic;

    UIElement* editor = FindByClass(field, "float-field-input");
    ASSERT_NE(editor, nullptr) << "the field has no text input under the class its rules select";
    const float valueWidth = fixture.Ui->MeasureTextWidth(*editor, "100");
    ASSERT_GT(valueWidth, 0.0f) << "the value measured to nothing; the fixture has no font";
    const ResolvedStyle& style = editor->GetResolvedStyle();
    // The field with its suffix is the padded box; its text input adds no padding of its own.
    EXPECT_EQ(style.Layout.Padding.Left, 0.0f) << "the text input pads inside the padded box";
    EXPECT_EQ(style.Layout.Padding.Right, 0.0f) << "the text input pads inside the padded box";
    const float textWidth = editor->GetLayoutWidth() - style.Layout.Padding.Left - style.Layout.Padding.Right;
    EXPECT_GE(textWidth, valueWidth) << "the value box clips \"100\": " << textWidth << " px for " << valueWidth
                                     << " px of text (field " << field->GetLayoutWidth() << " px)";
}

// The "%" follows the number, as the inspector's other unit fields place theirs (the texture
// inspector's inset fields), not at the far edge of the box; a short value shows it best.
TEST(InspectorPercentRowLayout, ThePercentFollowsTheNumber)
{
    InspectorLayoutFixture fixture;
    FloatField* field = LayOutPercentRow(fixture, 0.5f);
    if (!field)
        GTEST_SKIP() << fixture.Diagnostic;

    UIElement* editor = FindByClass(field, "float-field-input");
    ASSERT_NE(editor, nullptr) << "the field has no text input under the class its rules select";
    UIElement* suffix = FindByClass(field, "field-suffix-slot");
    ASSERT_NE(suffix, nullptr) << "the field shows no suffix";
    const float valueWidth = fixture.Ui->MeasureTextWidth(*editor, "50");
    ASSERT_GT(valueWidth, 0.0f) << "the value measured to nothing; the fixture has no font";
    const float valueEnd = editor->GetLayoutX() + editor->GetResolvedStyle().Layout.Padding.Left + valueWidth;
    const float gap = suffix->GetLayoutX() - valueEnd;
    EXPECT_LE(gap, kMaxValueToSuffixGap) << "the \"%\" sits " << gap << " px after \"50\"";
}

