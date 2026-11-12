// The Surface Rules rows and the provisioned mark: an edit of a rule row through the Inspector
// makes the rows the author's, so they stop following the terrain's height range.
//
// BOUNDARY: the rows are built in process over a live World with no terrain and no undo service.
// The discrete edit path (EditRules) is driven through the Measures dropdown; the drag path
// (MakeRuleDragHandlers) is not run.
#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Terrain/TerrainRule/TerrainRuleRows.h"

#include "Components/Terrain/TerrainModifierEffects.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "TerrainECS/TerrainDefaultSurfaceRules.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

#include <string>

using namespace GameEngine;

namespace
{

// The dropdown of the first inspector row whose label reads `text`.
Dropdown* FindRowDropdown(UIElement& element, const std::string& text)
{
    if (auto* label = dynamic_cast<Label*>(&element); label && label->GetText() == text)
    {
        UIElement* row = label->GetParent() ? label->GetParent()->GetParent() : nullptr;
        if (!row)
            return nullptr;
        for (const auto& cell : row->GetChildren())
            for (const auto& child : cell->GetChildren())
                if (auto* dropdown = dynamic_cast<Dropdown*>(child.get()))
                    return dropdown;
        return nullptr;
    }
    for (const auto& child : element.GetChildren())
        if (Dropdown* found = FindRowDropdown(*child, text))
            return found;
    return nullptr;
}

class TerrainRuleRowsProvisionedMarkTest : public ::testing::Test
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
};

TEST_F(TerrainRuleRowsProvisionedMarkTest, AnInspectorEditOfARuleRowClearsTheProvisionedMark)
{
    const Components::TerrainSurfaceRulesEffect provisioned = TerrainECS::MakeDefaultTerrainSurfaceRules(0.0f, 256.0f);
    ASSERT_TRUE(provisioned.ProvisionedDefaults);
    const ECS::EntityHandle entity = world.Create(provisioned).GetHandle();

    InspectorContext ctx;
    ctx.Parent = &parent;
    ctx.World = &world;
    ctx.Entity = entity;
    ctx.RequestInspectorRefresh = [] {};
    AddTerrainSurfaceRuleRows(ctx, provisioned, ECS::EntityHandle{}, nullptr);

    Dropdown* measures = FindRowDropdown(parent, "Measures");
    ASSERT_NE(measures, nullptr) << "the default rules show no condition row";
    measures->SetSelectedIndex(measures->GetSelectedIndex() == 0 ? 1 : 0);

    const auto* edited = world.GetComponent<Components::TerrainSurfaceRulesEffect>(entity);
    ASSERT_NE(edited, nullptr);
    EXPECT_FALSE(edited->ProvisionedDefaults) << "an edited row still follows the terrain's height range";
}

} // namespace
