// The Terrain Grass Preset row (Terrain/TerrainGrassPresetRow): the list holds the three presets, each
// with a picture; the field reads Custom once a dial differs from the preset, without a
// Custom entry; a pick applies the preset as one undo step; and the pictures were taken with the
// presets' values.

#include "Terrain/TerrainGrassPresetRow.h"

#include "AssetCore/SharedFileRead.h"
#include "Components/Terrain/TerrainGrass.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "TerrainGrass/TerrainGrassPresets.h"
#include "Types/ParseNumber.h"
#include "Types/StringUtils.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

using namespace GameEngine;

namespace
{

Dropdown* FindDropdown(UIElement& element)
{
    if (auto* dropdown = dynamic_cast<Dropdown*>(&element))
        return dropdown;
    for (const auto& child : element.GetChildren())
    {
        if (Dropdown* found = FindDropdown(*child))
            return found;
    }
    return nullptr;
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

// One `key=value` of a picture's line in grass-pictures.txt, or empty when the line has no such key.
std::string_view RecordValue(std::string_view line, std::string_view key)
{
    for (std::string_view token : Split(line, ' '))
    {
        const size_t equals = token.find('=');
        if (equals != std::string_view::npos && token.substr(0, equals) == key)
            return token.substr(equals + 1);
    }
    return {};
}

void ExpectRecordedNumber(std::string_view line, std::string_view key, float expected)
{
    const std::optional<float> recorded = ParseFloat(RecordValue(line, key));
    ASSERT_TRUE(recorded.has_value()) << "no number for " << key << " in: " << line;
    EXPECT_NEAR(*recorded, expected, 1e-4f) << key << " in: " << line;
}

// A color as the record writes it: the stored sRGB word in hex, or "auto" for the automatic root color.
std::string RecordedColor(uint32 color)
{
    return color == Components::kTerrainGrassAutoRootColor ? std::string("auto") : std::format("{:08X}", color);
}

std::string ShownText(const Dropdown& presets)
{
    return presets.GetHeaderLabel() ? presets.GetHeaderLabel()->GetText() : std::string();
}

// A terrain with default grass (Meadow) and its Preset row.
struct GrassRow
{
    ECS::World World;
    Editor::EditorChangeNotifications Notifications;
    Editor::UndoRedoService Undo;
    UIElement Parent;
    ECS::EntityHandle Terrain{};
    Dropdown* Presets = nullptr;
    int Refreshes = 0;

    GrassRow()
    {
        Terrain = World.CreateEntity();
        World.AddComponentImmediate(Terrain, Components::TerrainGrass{});
        AddTerrainGrassPresetRow(&Parent, &World, Terrain, &Notifications, &Undo, [this]() { ++Refreshes; });
        Presets = FindDropdown(Parent);
    }

    const Components::TerrainGrass& Grass() { return *World.GetComponent<Components::TerrainGrass>(Terrain); }

    // A dial edited the way the inspector's rows edit one: written, reconciled and committed.
    void SetBladeHeight(float height)
    {
        Components::TerrainGrass updated = Grass();
        updated.BladeHeight = height;
        TerrainGrass::ReconcileTerrainGrassPreset(updated);
        World.AddComponentImmediate(Terrain, updated);
        Notifications.NotifyComponentCommit<Components::TerrainGrass>(&World, Terrain);
    }
};

} // namespace

// Meadow, Lawn and Steppe in that order, each with its picture; Custom is not an entry.
TEST(TerrainGrassPresetRow, TheListIsTheThreePresetsEachWithAPicture)
{
    GrassRow row;
    ASSERT_NE(row.Presets, nullptr);
    EXPECT_EQ(row.Presets->GetSelectedIndex(), 0) << "new grass is Meadow";
    EXPECT_EQ(ShownText(*row.Presets), "Meadow");
    const char* names[] = {"Meadow", "Lawn", "Steppe"};
    const char* pictures[] = {"option-picture--grass-meadow", "option-picture--grass-lawn",
                              "option-picture--grass-steppe"};
    UIElement* picture = ChildWithClass(*row.Presets->GetHeaderContainer(), "dropdown-header-picture");
    ASSERT_NE(picture, nullptr) << "the closed field shows the selected preset's picture";
    for (int i = 0; i < 3; ++i)
    {
        row.Presets->SetSelectedIndexWithoutNotify(i);
        EXPECT_EQ(row.Presets->GetSelectedLabel(), names[i]);
        EXPECT_TRUE(picture->HasClass(pictures[i])) << names[i];
    }
    row.Presets->SetSelectedIndexWithoutNotify(3);
    EXPECT_EQ(row.Presets->GetSelectedIndex(), -1) << "the list offers only the three presets";
}

// A dial moved away from the preset shows Custom as the field's state, with nothing selected in the list.
TEST(TerrainGrassPresetRow, AChangedDialReadsCustom)
{
    GrassRow row;
    ASSERT_NE(row.Presets, nullptr);
    row.SetBladeHeight(0.5f);
    EXPECT_EQ(row.Grass().Preset, Components::TerrainGrassPreset::Custom);
    EXPECT_EQ(row.Presets->GetSelectedIndex(), -1) << "Custom is a state, not an entry";
    EXPECT_EQ(ShownText(*row.Presets), "Custom");
}

// Picking a preset, from Custom, writes its dials and names it in one undo step, and asks for the dial
// rows to be rebuilt; one undo puts the hand-set dial back.
TEST(TerrainGrassPresetRow, PickingAPresetAppliesItAsOneUndoStep)
{
    GrassRow row;
    ASSERT_NE(row.Presets, nullptr);
    row.SetBladeHeight(0.5f);
    ASSERT_EQ(ShownText(*row.Presets), "Custom");

    row.Presets->SetSelectedIndex(1);
    Components::TerrainGrass lawn{};
    TerrainGrass::ApplyTerrainGrassPreset(lawn, Components::TerrainGrassPreset::Lawn);
    EXPECT_EQ(row.Grass().Preset, Components::TerrainGrassPreset::Lawn);
    EXPECT_FLOAT_EQ(row.Grass().BladeHeight, lawn.BladeHeight);
    EXPECT_FLOAT_EQ(row.Grass().BladesPerSquareMeter, lawn.BladesPerSquareMeter);
    EXPECT_EQ(row.Grass().TipColor, lawn.TipColor);
    EXPECT_EQ(ShownText(*row.Presets), "Lawn");
    EXPECT_EQ(row.Undo.GetUndoCount(), 1u) << "one pick, one undo step";
    EXPECT_EQ(row.Refreshes, 1) << "the dial rows are rebuilt to show the preset's values";

    row.Undo.Undo();
    EXPECT_FLOAT_EQ(row.Grass().BladeHeight, 0.5f);
    EXPECT_EQ(row.Grass().Preset, Components::TerrainGrassPreset::Custom);
    EXPECT_EQ(ShownText(*row.Presets), "Custom");
}

// Every preset's picture has a line in grass-pictures.txt with the dials it was taken with, and each
// line's dials are its preset's: a retuned preset fails here until its picture is retaken. The line
// also holds Ground Tint Height, which no preset sets but which changes how every preset looks.
TEST(TerrainGrassPresetRow, ThePicturesWereTakenWithThePresetsValues)
{
    const std::filesystem::path record =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets/Icons/OptionPictures/grass-pictures.txt";
    std::string text;
    ASSERT_TRUE(ReadFileTextShared(record, text)) << record.generic_string() << " did not read";
    struct Picture
    {
        Components::TerrainGrassPreset Preset;
        const char* Name;
        const char* File;
    };
    const Picture pictures[] = {{Components::TerrainGrassPreset::Meadow, "Meadow", "grass-meadow.png: "},
                                {Components::TerrainGrassPreset::Lawn, "Lawn", "grass-lawn.png: "},
                                {Components::TerrainGrassPreset::Steppe, "Steppe", "grass-steppe.png: "}};
    ASSERT_EQ(std::size(pictures), std::size(TerrainGrass::kTerrainGrassPresets)) << "one picture per preset";
    const Components::TerrainGrass defaults{};
    for (const Picture& picture : pictures)
    {
        const size_t start = text.find(std::string("\n") + picture.File);
        ASSERT_NE(start, std::string::npos) << "no values recorded for " << picture.File;
        const size_t end = text.find('\n', start + 1);
        std::string_view line = std::string_view(text).substr(start + 1, end - start - 1);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        Components::TerrainGrass preset{};
        TerrainGrass::ApplyTerrainGrassPreset(preset, picture.Preset);
        EXPECT_EQ(RecordValue(line, "preset"), picture.Name) << line;
        ExpectRecordedNumber(line, "gain", 1.0f);
        EXPECT_EQ(RecordValue(line, "tip"), RecordedColor(preset.TipColor)) << line;
        EXPECT_EQ(RecordValue(line, "root"), RecordedColor(preset.RootColor)) << line;
        EXPECT_EQ(RecordValue(line, "backlight"), RecordedColor(preset.BacklightColor)) << line;
        ExpectRecordedNumber(line, "density", preset.BladesPerSquareMeter);
        ExpectRecordedNumber(line, "height", preset.BladeHeight);
        ExpectRecordedNumber(line, "width", preset.BladeWidth);
        ExpectRecordedNumber(line, "tilt", preset.BladeTilt);
        ExpectRecordedNumber(line, "ground-tint-height", defaults.GroundTintHeight);
    }
}
