// The Replace Rules control: choosing a preset replaces the block's rows as one undo step, and
// one undo puts back the rows the author had.
//
// BOUNDARY: the control is built in process over a live World with no terrain; the choice is
// driven through the Dropdown's selection, not through a pointer event on its open list.
#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Terrain/TerrainRule/TerrainRulePresetPicker.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "TerrainECS/TerrainDefaultSurfaceRules.h"
#include "TerrainECS/TerrainService.h"
#include "UI/Controls/Dropdown.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstring>

using namespace GameEngine;

namespace
{

// The first Dropdown under `element`, depth first.
Dropdown* FindDropdown(UIElement& element)
{
    if (auto* dropdown = dynamic_cast<Dropdown*>(&element))
        return dropdown;
    for (const auto& child : element.GetChildren())
        if (Dropdown* found = FindDropdown(*child))
            return found;
    return nullptr;
}

bool SameBytes(const Components::TerrainSurfaceRulesEffect& a, const Components::TerrainSurfaceRulesEffect& b)
{
    return std::memcmp(&a, &b, sizeof(Components::TerrainSurfaceRulesEffect)) == 0;
}

class TerrainRulePresetPickerTest : public ::testing::Test
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

    ECS::World world{nullptr};
    UIElement parent{"div"};
    Editor::EditorChangeNotifications notifications;
    Editor::UndoRedoService undo;
};

TEST_F(TerrainRulePresetPickerTest, EachPresetReplacesTheRowsAndOneUndoRestoresThemByteExact)
{
    Components::TerrainSurfaceRulesEffect authored = TerrainECS::MakeDefaultTerrainSurfaceRules(0.0f, 256.0f);
    authored.ProvisionedDefaults = false;
    authored.Rules[0].Conditions[0].Min += 3.0f;
    const ECS::EntityHandle entity = world.Create(authored).GetHandle();
    const Components::TerrainSurfaceRulesEffect before = *world.GetComponent<Components::TerrainSurfaceRulesEffect>(entity);

    InspectorContext ctx;
    ctx.Parent = &parent;
    ctx.World = &world;
    ctx.Entity = entity;
    ctx.ChangeNotifications = &notifications;
    ctx.Undo = &undo;
    const TerrainECS::TerrainHeightRangeMeters range{-3000.0f, 3000.0f};
    AddTerrainRulePresetRow(ctx, Components::TerrainDomain::Spherical, range);

    Dropdown* control = FindDropdown(parent);
    ASSERT_NE(control, nullptr) << "the Replace Rules control is missing";
    const size_t presetCount = TerrainRulePresetOptions(Components::TerrainDomain::Spherical).size();
    ASSERT_GE(presetCount, 2u) << "a planet lists fewer than two rule sets";

    for (size_t i = 0; i < presetCount; ++i)
    {
        control->SetSelectedIndex(static_cast<int>(i));
        const auto* replaced = world.GetComponent<Components::TerrainSurfaceRulesEffect>(entity);
        ASSERT_NE(replaced, nullptr);
        EXPECT_FALSE(SameBytes(*replaced, before)) << "preset " << i << " left the rows as they were";
        EXPECT_FALSE(replaced->ProvisionedDefaults) << "the preset's rows still follow the height range";
        ASSERT_EQ(undo.GetUndoCount(), 1u) << "preset " << i << " is not one undo step";

        undo.Undo();
        const auto* restored = world.GetComponent<Components::TerrainSurfaceRulesEffect>(entity);
        ASSERT_NE(restored, nullptr);
        EXPECT_TRUE(SameBytes(*restored, before)) << "one undo after preset " << i << " did not restore the rows";
        undo.Clear();
        control->SetSelectedIndexWithoutNotify(-1);
    }
}

} // namespace
