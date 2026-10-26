// Pins the component color row (InspectorComponentColorRow.h): the member's color, alpha and undo step,
// the intensity range, the chip's encoding and the label reset.
//
// BOUNDARY: the row is built in process over a live World on the Post Process Volume's night tint
// color and the Sky environment's colors, the picker is a stub that keeps the callbacks and the
// opening color it is handed, and the click goes through
// UIElement::DispatchEvent, as in TerrainGrassRootColorRowTests. Hit-testing, UIManager routing and
// the native picker window are not covered.

#include <gtest/gtest.h>

#include "Inspectors/InspectorComponentColorRow.h"

#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Panels/ColorPicker.h"
#include "Types/ColorUtils.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

using namespace GameEngine;
using Components::PostProcessVolume;
using Components::SkyEnvironment;

namespace
{

constexpr ColorLinear kAuthored{0.25f, 0.5f, 1.0f, 0.5f};
constexpr std::uint32_t kPickedArgb = 0xFF336699u;

// One sRGB byte step around linear 0.5, about 0.006, scaled by an intensity of 2: what an sRGB byte
// encode and decode may move a channel of a color twice as bright as white.
constexpr float kBrightByteTolerance = 0.012f;

// The row's value text: the label with the inspector-text class.
Label* FindValue(UIElement& element, const std::string& text)
{
    if (auto* label = dynamic_cast<Label*>(&element);
        label && label->HasClass("inspector-text") && label->GetText() == text)
        return label;
    for (const auto& child : element.GetChildren())
        if (Label* found = FindValue(*child, text))
            return found;
    return nullptr;
}

// The row's name: the label without the inspector-text class.
Label* FindName(UIElement& element, const std::string& text)
{
    if (auto* label = dynamic_cast<Label*>(&element);
        label && !label->HasClass("inspector-text") && label->GetText() == text)
        return label;
    for (const auto& child : element.GetChildren())
        if (Label* found = FindName(*child, text))
            return found;
    return nullptr;
}

void Press(Label& label)
{
    UIEvent press{};
    press.Id = kEventMouseDown;
    press.Button = 0;
    press.Target = &label;
    press.CurrentTarget = &label;
    label.DispatchEvent(press);
}

// What the stub picker was opened with, and the callbacks it keeps.
struct PickerStub
{
    std::optional<ColorPickerCallbacks> Callbacks;
    std::uint32_t OpenedArgb = 0;
    float OpenedIntensity = 0.0f;
};

// A color row on one member of a component, with the given intensity range.
template<typename TComponent>
struct ColorRowFixture
{
    ECS::World World{nullptr};
    ECS::EntityHandle Entity{};
    Editor::UndoRedoService Undo;
    UIElement Parent{"div"};
    PickerStub Picker;

    ColorRowFixture(ColorLinear TComponent::*member, const ColorLinear& authored, float maxIntensity)
    {
        TComponent component{};
        component.*member = authored;
        Entity = World.Create(component).GetHandle();
        InspectorUI::ComponentColorBinding<TComponent> binding;
        binding.World = &World;
        binding.Entity = Entity;
        binding.Undo = &Undo;
        binding.OpenPicker = [this](std::uint32_t argb, float intensity, ColorPickerCallbacks callbacks)
        {
            Picker.OpenedArgb = argb;
            Picker.OpenedIntensity = intensity;
            Picker.Callbacks = std::move(callbacks);
        };
        binding.Color = member;
        binding.ChangeLabel = "Change Color";
        binding.MaxIntensity = maxIntensity;
        InspectorUI::AddComponentColorRow<TComponent>(&Parent, "Color", "", std::move(binding));
    }

    const TComponent& Component() { return *World.GetComponent<TComponent>(Entity); }
};

using SkyRowFixture = ColorRowFixture<SkyEnvironment>;

TEST(InspectorComponentColorRow, AnAppliedColorLandsInTheMemberAndUndoRestoresIt)
{
    ECS::World world{nullptr};
    PostProcessVolume volume{};
    volume.NightTintColor = kAuthored;
    const ECS::EntityHandle entity = world.Create(volume).GetHandle();
    Editor::UndoRedoService undo;
    UIElement parent{"div"};
    std::optional<ColorPickerCallbacks> picker;

    InspectorUI::ComponentColorBinding<PostProcessVolume> binding;
    binding.World = &world;
    binding.Entity = entity;
    binding.Undo = &undo;
    binding.OpenPicker = [&picker](std::uint32_t, float, ColorPickerCallbacks callbacks) { picker = std::move(callbacks); };
    binding.Color = &PostProcessVolume::NightTintColor;
    binding.ChangeLabel = "Change Night Tint Color";
    InspectorUI::AddComponentColorRow<PostProcessVolume>(&parent, "Color", "", std::move(binding));

    Label* value = FindValue(parent, InspectorUI::FormatColorRgb(kAuthored.r, kAuthored.g, kAuthored.b));
    ASSERT_NE(value, nullptr) << "the row does not show the member's color";
    Press(*value);
    ASSERT_TRUE(picker.has_value()) << "a click on the value did not open the picker";

    picker->onApply(kPickedArgb, 1.0f);
    const ColorLinear picked = ColorUtils::ArgbToLinear(kPickedArgb);
    const ColorLinear& applied = world.GetComponent<PostProcessVolume>(entity)->NightTintColor;
    EXPECT_FLOAT_EQ(applied.r, picked.r);
    EXPECT_FLOAT_EQ(applied.g, picked.g);
    EXPECT_FLOAT_EQ(applied.b, picked.b);
    EXPECT_FLOAT_EQ(applied.a, kAuthored.a) << "the row wrote the member's alpha";
    EXPECT_EQ(value->GetText(), InspectorUI::FormatColorRgb(picked.r, picked.g, picked.b));

    ASSERT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_EQ(world.GetComponent<PostProcessVolume>(entity)->NightTintColor, kAuthored)
        << "undo did not restore the color from before the picker";
}

// A tint twice as bright as white opens the picker on the unit color with an intensity of 2, and the
// picker's result comes back as the same bright color.
TEST(InspectorComponentColorRow, TheIntensityCarriesAColorBrighterThanWhiteThroughThePicker)
{
    constexpr ColorLinear kBright{2.0f, 1.0f, 0.5f};
    SkyRowFixture row(&SkyEnvironment::AmbientTintSky, kBright, kColorPickerMaxIntensity);
    Label* value = FindValue(row.Parent, InspectorUI::FormatColorRgb(kBright.r, kBright.g, kBright.b));
    ASSERT_NE(value, nullptr) << "the row does not show the member's color";
    Press(*value);
    ASSERT_TRUE(row.Picker.Callbacks.has_value()) << "a click on the value did not open the picker";

    EXPECT_FLOAT_EQ(row.Picker.OpenedIntensity, 2.0f) << "the brightness above white is not the picker's intensity";
    EXPECT_EQ(row.Picker.OpenedArgb, ColorUtils::LinearToArgb(ColorLinear(1.0f, 0.5f, 0.25f)))
        << "the picker does not open on the unit color";

    row.Picker.Callbacks->onApply(row.Picker.OpenedArgb, row.Picker.OpenedIntensity);
    const ColorLinear& applied = row.Component().AmbientTintSky;
    EXPECT_FLOAT_EQ(applied.r, kBright.r);
    EXPECT_NEAR(applied.g, kBright.g, kBrightByteTolerance);
    EXPECT_NEAR(applied.b, kBright.b, kBrightByteTolerance);
}

// The picker opens on the color the chip shows: the sRGB encoding of the linear color.
TEST(InspectorComponentColorRow, ThePickerOpensOnTheChipsEncoding)
{
    constexpr ColorLinear kAuthoredTop{0.25f, 0.5f, 1.0f};
    SkyRowFixture row(&SkyEnvironment::GradientSkyTopColor, kAuthoredTop, kColorPickerMaxIntensity);
    Label* value = FindValue(row.Parent, InspectorUI::FormatColorRgb(kAuthoredTop.r, kAuthoredTop.g, kAuthoredTop.b));
    ASSERT_NE(value, nullptr) << "the row does not show the member's color";
    Press(*value);
    ASSERT_TRUE(row.Picker.Callbacks.has_value()) << "a click on the value did not open the picker";

    EXPECT_EQ(row.Picker.OpenedArgb, ColorUtils::LinearToArgb(kAuthoredTop))
        << "the picker does not open on the sRGB encoding of the color";
    EXPECT_FLOAT_EQ(row.Picker.OpenedIntensity, 1.0f);
    const UIElement* chip = value->GetParent()->GetChildren().front().get();
    EXPECT_EQ(chip->Overrides().Get(Style::BackgroundColor), row.Picker.OpenedArgb)
        << "the picker opens on a different color than the chip";
}

// A row with the default range of 1 ignores the picker's intensity: the night tint stays within 0 to 1.
TEST(InspectorComponentColorRow, TheDefaultRangeKeepsThePickedColorWithinOne)
{
    ColorRowFixture<PostProcessVolume> row(&PostProcessVolume::NightTintColor, kAuthored, 1.0f);
    Label* value = FindValue(row.Parent, InspectorUI::FormatColorRgb(kAuthored.r, kAuthored.g, kAuthored.b));
    ASSERT_NE(value, nullptr) << "the row does not show the member's color";
    Press(*value);
    ASSERT_TRUE(row.Picker.Callbacks.has_value()) << "a click on the value did not open the picker";

    row.Picker.Callbacks->onApply(kPickedArgb, 4.0f);
    const ColorLinear picked = ColorUtils::ArgbToLinear(kPickedArgb);
    const ColorLinear& applied = row.Component().NightTintColor;
    EXPECT_FLOAT_EQ(applied.r, picked.r);
    EXPECT_FLOAT_EQ(applied.g, picked.g);
    EXPECT_FLOAT_EQ(applied.b, picked.b);
    EXPECT_LE(std::max({applied.r, applied.g, applied.b}), 1.0f) << "the picker's intensity brightened a unit color";
}

// A double-click on the label puts the member's default back as one undo step; a single press does not.
TEST(InspectorComponentColorRow, ADoubleClickOnTheLabelResetsTheDefaultAsOneUndoStep)
{
    constexpr ColorLinear kAuthoredTop{0.9f, 0.1f, 0.1f};
    SkyRowFixture row(&SkyEnvironment::GradientSkyTopColor, kAuthoredTop, kColorPickerMaxIntensity);
    Label* name = FindName(row.Parent, "Color");
    ASSERT_NE(name, nullptr) << "the row has no label";

    Press(*name);
    EXPECT_EQ(row.Component().GradientSkyTopColor, kAuthoredTop) << "a single press reset the color";
    EXPECT_EQ(row.Undo.GetUndoCount(), 0u);

    Press(*name);
    EXPECT_EQ(row.Component().GradientSkyTopColor, SkyEnvironment{}.GradientSkyTopColor)
        << "a double-click did not reset the color to its default";
    ASSERT_EQ(row.Undo.GetUndoCount(), 1u);
    row.Undo.Undo();
    EXPECT_EQ(row.Component().GradientSkyTopColor, kAuthoredTop) << "undo did not restore the color from before the reset";
}

} // namespace
