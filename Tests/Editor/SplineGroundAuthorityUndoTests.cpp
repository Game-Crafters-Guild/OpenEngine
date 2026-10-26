// The ground authority's recipe-created cleanup through the editor's undo
// journal: what the controller removes lands in the journal, and undo restores
// the pieces with their provenance ledger. The controller and its test world are
// the module's (SplineRecipesTests); the journal is the editor's.

#include <gtest/gtest.h>

#include "Placement/RecipeEditorHooks.h"
#include "SplineGroundAuthorityTestWorld.h"
#include "SplineRecipes/SplineGroundAuthority.h"
#include "SplineRecipes/SplineRecipeSystem.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Spline/SplineExtrude.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "ECS/ECSTemplates.h" // World's component templates: definitions, not just declarations
#include "ECS/World.h"
#include "SplineECS/SplineService.h"

using namespace GameEngine;
using GameEngine::Components::SplineGroundAuthority;
using namespace GameEngine::SplineGroundAuthorityTestWorld;

TEST(SplineGroundAuthority, RecipeCreatedCleanupIsUndoable)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.Profile = Components::SplineExtrudeProfile::Crown;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    Editor::UndoRedoService undo;
    SplineRecipes::SplineRecipeSystem recipes(nullptr);
    Editor::RecipeEditorHooks journal(&undo, recipes);
    SplineRecipes::SplineGroundAuthorityController controller;
    controller.Update(world, &journal);
    ASSERT_NE(world.GetComponent<Components::TerrainFlattenEffect>(run), nullptr);

    auto* live = world.GetComponentForWrite<Components::SplineExtrude>(run);
    ASSERT_NE(live, nullptr);
    live->GroundAuthority = SplineGroundAuthority::None;
    controller.Update(world, &journal);

    EXPECT_EQ(world.GetComponent<Components::TerrainFlattenEffect>(run), nullptr);
    EXPECT_EQ(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr);
    EXPECT_EQ(world.GetComponent<Components::TerrainModifierVolume>(run), nullptr);
    EXPECT_EQ(world.GetComponent<Components::SplineGroundProvisioned>(run), nullptr)
        << "the ledger leaves with the pieces it records";
    ASSERT_TRUE(undo.CanUndo()) << "recipe-created cleanup must land in the undo journal";

    undo.Undo();

    const auto* flatten = world.GetComponent<Components::TerrainFlattenEffect>(run);
    ASSERT_NE(flatten, nullptr) << "undo must restore the removed pieces";
    EXPECT_EQ(flatten->Blend, Components::TerrainModifierBlend::Average);
    EXPECT_NE(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr);
    EXPECT_NE(world.GetComponent<Components::TerrainModifierVolume>(run), nullptr);
    const auto* marker = world.GetComponent<Components::SplineGroundProvisioned>(run);
    ASSERT_NE(marker, nullptr) << "undo must restore the provenance ledger with the pieces";
    EXPECT_TRUE(marker->Flatten && marker->Claim && marker->Volume);
}

TEST(SplineGroundAuthority, OwnsToGradesRemovesOnlyARecipeCreatedClaim)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.Profile = Components::SplineExtrudeProfile::Crown;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    Editor::UndoRedoService undo;
    SplineRecipes::SplineRecipeSystem recipes(nullptr);
    Editor::RecipeEditorHooks journal(&undo, recipes);
    SplineRecipes::SplineGroundAuthorityController controller;
    controller.Update(world, &journal);
    ASSERT_NE(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr);

    auto* live = world.GetComponentForWrite<Components::SplineExtrude>(run);
    ASSERT_NE(live, nullptr);
    live->GroundAuthority = SplineGroundAuthority::Grades;
    controller.Update(world, &journal);

    EXPECT_EQ(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr)
        << "the recipe-created claim comes off when the authority stops owning";
    const auto* flatten = world.GetComponent<Components::TerrainFlattenEffect>(run);
    ASSERT_NE(flatten, nullptr);
    EXPECT_TRUE(flatten->RespectClaims) << "a graded run defers to claims again";
    EXPECT_TRUE(Components::EffectPoolName(*flatten).empty());
    const auto* marker = world.GetComponent<Components::SplineGroundProvisioned>(run);
    ASSERT_NE(marker, nullptr);
    EXPECT_FALSE(marker->Claim);
    EXPECT_TRUE(marker->Flatten && marker->Volume);

    ASSERT_TRUE(undo.CanUndo());
    undo.Undo();
    EXPECT_NE(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr)
        << "undo must restore the claim";
    const auto* restored = world.GetComponent<Components::SplineGroundProvisioned>(run);
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->Claim);
}
