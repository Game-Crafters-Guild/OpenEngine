// The Grass Area region's Darken Ground row (TerrainGrassRegionRows.h): the label carries the share
// of the terrain's Darken Ground it lands on, a notice says when the terrain's amount is 0, and a drag
// holds the terrain's interactive-edit signal until the value is committed.
//
// BOUNDARY: the row is built in process over a live World and driven through its float field, as in
// TerrainGrassSoilRowsTests. Hit-testing and UIManager routing are not covered.

#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Terrain/TerrainGrassRegionRows.h"
#include "Terrain/TerrainInteractiveModifierEdit.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/UIElement.h"

#include <string>

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

Label* FindLabelStartingWith(UIElement& element, const std::string& prefix)
{
    if (auto* label = dynamic_cast<Label*>(&element); label && label->GetText().rfind(prefix, 0) == 0)
        return label;
    for (const auto& child : element.GetChildren())
        if (Label* found = FindLabelStartingWith(*child, prefix))
            return found;
    return nullptr;
}

class TerrainGrassRegionRowsTest : public ::testing::Test
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

    // One grass terrain at `terrainAmount` and one region keeping `regionScale` of it.
    void Build(float terrainAmount, float regionScale)
    {
        Components::TerrainGrass grass{};
        grass.GroundDarkening = terrainAmount;
        const ECS::EntityHandle terrain = world.Create(grass).GetHandle();
        world.AddComponentImmediate<Components::Terrain>(terrain, Components::Terrain{});
        Components::TerrainGrassEffect effect{};
        effect.GroundDarkeningScale = regionScale;
        region = world.Create(effect).GetHandle();
        InspectorContext ctx;
        ctx.Parent = &parent;
        ctx.World = &world;
        ctx.Entity = region;
        AddGrassRegionDarkenGroundRow(ctx, effect, ResolveTerrainGrassBase(world));
    }

    ECS::World world{nullptr};
    ECS::EntityHandle region{};
    UIElement parent{"div"};
};

TEST_F(TerrainGrassRegionRowsTest, TheLabelShowsTheDarkenGroundTheRegionLandsOn)
{
    Build(0.55f, 0.5f);
    Label* label = FindLabelStartingWith(parent, "Darken Ground");
    ASSERT_NE(label, nullptr) << "the region shows no Darken Ground row";
    EXPECT_EQ(label->GetText(), "Darken Ground (27.5 %)");
    EXPECT_EQ(FindFirst<EditorUI::InspectorNotice>(parent), nullptr) << "the terrain darkens the ground; no notice";
    FloatField* field = FindFirst<FloatField>(parent);
    ASSERT_NE(field, nullptr);
    EXPECT_FLOAT_EQ(field->GetValue(), 50.0f);
    field->SetValue(20.0f);
    field->NotifyValueChanged();
    EXPECT_EQ(label->GetText(), "Darken Ground (11 %)");
    EXPECT_FLOAT_EQ(world.GetComponent<Components::TerrainGrassEffect>(region)->GroundDarkeningScale, 0.2f);
}

TEST_F(TerrainGrassRegionRowsTest, TheNoticeShowsOnlyWhenTheTerrainsAmountIsZero)
{
    Build(0.0f, 0.5f);
    EXPECT_NE(FindFirst<EditorUI::InspectorNotice>(parent), nullptr)
        << "a terrain at Darken Ground 0% leaves the row without effect, and nothing says so";
    EXPECT_NE(FindLabelStartingWith(parent, "Darken Ground"), nullptr);
}

// The map recomposes once a drag ends, not at every step: the drag arms the interactive-edit signal
// and the commit releases it.
TEST_F(TerrainGrassRegionRowsTest, ADragHoldsTheInteractiveEditUntilItCommits)
{
    Build(0.55f, 0.5f);
    Slider* slider = FindFirst<Slider>(parent);
    ASSERT_NE(slider, nullptr);
    ASSERT_EQ(Editor::TerrainInteractiveModifierEditArmCount(), 0);
    slider->SetValueWithoutNotify(0.3f);
    slider->NotifyValueChanging();
    EXPECT_EQ(Editor::TerrainInteractiveModifierEditArmCount(), 1) << "a drag step left the edit signal down";
    slider->SetValueWithoutNotify(0.25f);
    slider->NotifyValueChanged();
    EXPECT_EQ(Editor::TerrainInteractiveModifierEditArmCount(), 0) << "the commit left the edit signal held";
    EXPECT_FLOAT_EQ(world.GetComponent<Components::TerrainGrassEffect>(region)->GroundDarkeningScale, 0.25f);
}

} // namespace
