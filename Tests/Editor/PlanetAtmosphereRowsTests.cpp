// The terrain inspector's atmosphere rows on a planet: the mode dropdown, the height in the mode's unit
// with the other unit as a caption, the read-only Auto height, and one undo step per edit.
//
// BOUNDARY: the inspector is built in process over a live World, as in TerrainPlanetRowsTests. The
// rebuild after a mode change is the editor's InspectorPanel and is not run here: the test counts it.

#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Terrain/TerrainInspector.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/EnumField.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

template<typename T>
T* FindFirst(UIElement& element)
{
    if (auto* found = dynamic_cast<T*>(&element))
        return found;
    for (const auto& child : element.GetChildren())
        if (T* found = FindFirst<T>(*child))
            return found;
    return nullptr;
}

// The inspector row whose label reads `text`: the row is the label's cell's parent.
// A dropdown's selected text is a Label too, so the search skips dropdowns.
UIElement* FindRow(UIElement& element, const std::string& text)
{
    if (dynamic_cast<Dropdown*>(&element))
        return nullptr;
    if (auto* label = dynamic_cast<Label*>(&element); label && label->GetText() == text)
        return label->GetParent() ? label->GetParent()->GetParent() : nullptr;
    for (const auto& child : element.GetChildren())
        if (UIElement* found = FindRow(*child, text))
            return found;
    return nullptr;
}

// The field's caption: the label inside it whose text starts with "= ".
std::string CaptionOf(UIElement& element)
{
    if (auto* label = dynamic_cast<Label*>(&element); label && label->GetText().rfind("= ", 0) == 0)
        return label->GetText();
    for (const auto& child : element.GetChildren())
        if (std::string caption = CaptionOf(*child); !caption.empty())
            return caption;
    return {};
}

class PlanetAtmosphereRowsTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    // P-2: a 2 km planet with 50 m of relief amplitude, its ridges up to 75 m.
    void Build(Components::TerrainAtmosphereMode mode, Components::TerrainDomain domain = Components::TerrainDomain::Spherical,
               float radius = 2000.0f, float share = 0.1f)
    {
        RegisterTerrainInspector();
        Components::Terrain terrain{};
        terrain.Domain = domain;
        terrain.PlanetRadius = radius;
        terrain.AtmosphereMode = mode;
        terrain.AtmosphereShare = share;
        terrain.AtmosphereHeight = 300.0f;
        entity = world.Create(terrain).GetHandle();
        Components::TerrainPlanetRelief relief{};
        relief.Amplitude = 50.0f;
        world.AddComponentImmediate(entity, relief);
        Rebuild();
    }

    void Rebuild()
    {
        parent.RemoveAllChildren();
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Components::Terrain>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext ctx;
        ctx.Parent = &parent;
        ctx.World = &world;
        ctx.Entity = entity;
        ctx.Undo = &undo;
        ctx.RequestInspectorRefresh = [this] { ++refreshes; };
        (*inspector)(ctx);
    }

    const Components::Terrain& Stored() { return *world.GetComponent<Components::Terrain>(entity); }

    EnumField<Components::TerrainAtmosphereMode>* ModeField()
    {
        UIElement* row = FindRow(parent, "Atmosphere Height");
        return row ? FindFirst<EnumField<Components::TerrainAtmosphereMode>>(*row) : nullptr;
    }

    ECS::World world{nullptr};
    ECS::EntityHandle entity{};
    Editor::UndoRedoService undo;
    UIElement parent{"div"};
    int refreshes = 0;
};

TEST_F(PlanetAtmosphereRowsTest, TheModeRowOffersAutoShareAndMetersInThatOrder)
{
    Build(Components::TerrainAtmosphereMode::Auto);
    EnumField<Components::TerrainAtmosphereMode>* modes = ModeField();
    ASSERT_NE(modes, nullptr) << "a planet shows no Atmosphere Height mode row";
    Dropdown* dropdown = modes->GetDropdown();
    ASSERT_NE(dropdown, nullptr);
    EXPECT_EQ(dropdown->GetSelectedLabel(), "Auto");
    const std::vector<std::string> expected = {"Auto", "Share of Radius", "Height in Meters"};
    for (int i = 0; i < static_cast<int>(expected.size()); ++i)
    {
        dropdown->SetSelectedIndexWithoutNotify(i);
        EXPECT_EQ(dropdown->GetSelectedLabel(), expected[static_cast<size_t>(i)]) << i;
    }
}

// Under Auto the height is the derived one, in meters, read-only, with its share as the caption.
TEST_F(PlanetAtmosphereRowsTest, AutoShowsTheDerivedHeightReadOnly)
{
    Build(Components::TerrainAtmosphereMode::Auto);
    UIElement* row = FindRow(parent, "Height (m)");
    ASSERT_NE(row, nullptr);
    FloatField* field = FindFirst<FloatField>(*row);
    ASSERT_NE(field, nullptr);
    EXPECT_FALSE(field->IsEnabled());
    EXPECT_NEAR(field->GetValue(), 75.0f / 0.08849f, 0.5f);
    TextInput* text = FindFirst<TextInput>(*field);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->GetValue(), "847.6");
    EXPECT_EQ(CaptionOf(*row), "= 0.424 of radius");
    Label* label = FindFirst<Label>(*row);
    ASSERT_NE(label, nullptr);
    EXPECT_NE(label->GetTooltip().find("about 11 times as tall as the highest peak"), std::string::npos);
    EXPECT_NE(label->GetTooltip().find("Pick Share of Radius or Height in Meters"), std::string::npos);
}

TEST_F(PlanetAtmosphereRowsTest, ShareModeEditsTheShareWithTheMetersCaptionAsOneUndoStep)
{
    Build(Components::TerrainAtmosphereMode::ShareOfRadius);
    UIElement* row = FindRow(parent, "Height (share of radius)");
    ASSERT_NE(row, nullptr);
    FloatField* field = FindFirst<FloatField>(*row);
    ASSERT_NE(field, nullptr);
    EXPECT_TRUE(field->IsEnabled());
    EXPECT_EQ(CaptionOf(*row), "= 200 m");
    field->SetValue(0.25f);
    field->NotifyValueChanged();
    EXPECT_FLOAT_EQ(Stored().AtmosphereShare, 0.25f);
    EXPECT_EQ(CaptionOf(*row), "= 500 m");
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_FLOAT_EQ(Stored().AtmosphereShare, 0.1f);
}

TEST_F(PlanetAtmosphereRowsTest, MetersModeEditsTheMetersWithTheShareCaptionAsOneUndoStep)
{
    Build(Components::TerrainAtmosphereMode::HeightMeters);
    UIElement* row = FindRow(parent, "Height (m)");
    ASSERT_NE(row, nullptr);
    FloatField* field = FindFirst<FloatField>(*row);
    ASSERT_NE(field, nullptr);
    EXPECT_TRUE(field->IsEnabled());
    EXPECT_EQ(CaptionOf(*row), "= 0.15 of radius");
    field->SetValue(60.0f);
    field->NotifyValueChanged();
    EXPECT_FLOAT_EQ(Stored().AtmosphereHeight, 60.0f);
    EXPECT_EQ(CaptionOf(*row), "= 0.03 of radius");
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_FLOAT_EQ(Stored().AtmosphereHeight, 300.0f);
}

// A mode change stores the current height in the new unit, so the sky keeps its look, and rebuilds the rows.
TEST_F(PlanetAtmosphereRowsTest, AModeChangeKeepsTheHeightAsOneUndoStep)
{
    Build(Components::TerrainAtmosphereMode::Auto);
    EnumField<Components::TerrainAtmosphereMode>* modes = ModeField();
    ASSERT_NE(modes, nullptr);
    modes->GetDropdown()->SetSelectedIndex(2);
    EXPECT_EQ(Stored().AtmosphereMode, Components::TerrainAtmosphereMode::HeightMeters);
    EXPECT_NEAR(Stored().AtmosphereHeight, 75.0f / 0.08849f, 0.5f);
    EXPECT_EQ(refreshes, 1);
    EXPECT_EQ(undo.GetUndoCount(), 1u);

    Rebuild();
    ModeField()->GetDropdown()->SetSelectedIndex(1);
    EXPECT_EQ(Stored().AtmosphereMode, Components::TerrainAtmosphereMode::ShareOfRadius);
    EXPECT_NEAR(Stored().AtmosphereShare, 75.0f / 0.08849f / 2000.0f, 1e-4f);

    undo.Undo();
    undo.Undo();
    EXPECT_EQ(Stored().AtmosphereMode, Components::TerrainAtmosphereMode::Auto);
}

// Captions use three significant digits in every mode, and whole meters from 100 up are grouped.
TEST_F(PlanetAtmosphereRowsTest, CaptionsUseThreeSignificantDigitsAndGroupWholeMeters)
{
    constexpr auto kShare = Components::TerrainAtmosphereMode::ShareOfRadius;
    Build(kShare, Components::TerrainDomain::Spherical, 600000.0f, 0.1130071f);
    UIElement* row = FindRow(parent, "Height (share of radius)");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(CaptionOf(*row), "= 67 804 m");

    parent.RemoveAllChildren();
    Build(kShare, Components::TerrainDomain::Spherical, 2000.0f, 0.0157f);
    row = FindRow(parent, "Height (share of radius)");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(CaptionOf(*row), "= 31.4 m");
}

TEST_F(PlanetAtmosphereRowsTest, APlanarTerrainHasNoAtmosphereRows)
{
    Build(Components::TerrainAtmosphereMode::Auto, Components::TerrainDomain::Planar);
    EXPECT_EQ(FindRow(parent, "Atmosphere Height"), nullptr);
    EXPECT_EQ(FindRow(parent, "Height (m)"), nullptr);
}

} // namespace
