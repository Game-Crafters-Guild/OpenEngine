// The ground authority's edits through the editor's undo journal: what the
// controller adds or removes lands in the journal, and undo reverts the pieces
// with their SplineGroundProvisioned marker. The controller and its test world are
// the module's (SplineRecipesTests); the journal is the editor's.

#include <gtest/gtest.h>

#include "Placement/RecipeEditorHooks.h"
#include "EditorChangeNotifications.h"
#include "Scene/SceneDocumentManager.h"
#include "SplineGroundAuthorityTestWorld.h"
#include "SplineRecipes/SplineGroundAuthority.h"
#include "SplineRecipes/SplineRecipeSystem.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplinePaving.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "ECS/ECSTemplates.h" // World's component templates: definitions, not just declarations
#include "ECS/World.h"
#include "SplineECS/SplineService.h"

#include <memory>
#include <utility>

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
    Editor::RecipeEditorHooks journal(&undo, recipes, nullptr);
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
    Editor::RecipeEditorHooks journal(&undo, recipes, nullptr);
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

TEST(SplineGroundAuthority, APavedAreaTurnedOffComesBackOnUndoWithItsGrassAndPaint)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle area = CreatePavedArea(world, SplineECS::SplineService::Get(), {});

    Editor::UndoRedoService undo;
    SplineRecipes::SplineRecipeSystem recipes(nullptr);
    Editor::RecipeEditorHooks journal(&undo, recipes, nullptr);
    SplineRecipes::SplineGroundAuthorityController controller;
    controller.Update(world, &journal);

    auto* live = world.GetComponentForWrite<Components::SplinePaving>(area);
    ASSERT_NE(live, nullptr);
    live->GroundAuthority = SplineGroundAuthority::None;
    controller.Update(world, &journal);
    ASSERT_EQ(world.GetComponent<Components::TerrainGrassEffect>(area), nullptr);
    ASSERT_EQ(world.GetComponent<Components::TerrainPaintLayerEffect>(area), nullptr);

    ASSERT_TRUE(undo.CanUndo());
    undo.Undo();
    EXPECT_NE(world.GetComponent<Components::TerrainGrassEffect>(area), nullptr);
    EXPECT_NE(world.GetComponent<Components::TerrainPaintLayerEffect>(area), nullptr);
    const auto* marker = world.GetComponent<Components::SplineGroundProvisioned>(area);
    ASSERT_NE(marker, nullptr);
    EXPECT_TRUE(marker->Grass && marker->Paint);
}

namespace
{

// The inspector's edit of a paving's Ground Authority, as one undo entry.
class SetPavingAuthority final : public Editor::IEditorCommand
{
public:
    SetPavingAuthority(ECS::World& world, ECS::EntityHandle area, SplineGroundAuthority value)
        : m_World(world), m_Area(area), m_Value(value)
    {
    }
    const char* GetName() const override { return "Change Spline Paving Ground Authority"; }
    void Do() override { Swap(); }
    void Undo() override { Swap(); }

private:
    void Swap() { std::swap(m_World.GetComponentForWrite<Components::SplinePaving>(m_Area)->GroundAuthority, m_Value); }

    ECS::World& m_World;
    ECS::EntityHandle m_Area;
    SplineGroundAuthority m_Value;
};

} // namespace

TEST(SplineGroundAuthority, PickingNoneIsOneUndoStepWithTheCleanupItCauses)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle area = CreatePavedArea(world, SplineECS::SplineService::Get(), {});

    Editor::UndoRedoService undo;
    SplineRecipes::SplineRecipeSystem recipes(nullptr);
    Editor::RecipeEditorHooks journal(&undo, recipes, nullptr);
    SplineRecipes::SplineGroundAuthorityController controller;
    controller.Update(world, &journal);

    undo.Execute(std::make_unique<SetPavingAuthority>(world, area, SplineGroundAuthority::None));
    controller.Update(world, &journal);
    ASSERT_EQ(world.GetComponent<Components::TerrainGrassEffect>(area), nullptr);
    EXPECT_EQ(undo.GetUndoCount(), 1u) << "the cleanup joins the edit that caused it";
    EXPECT_STREQ(undo.PeekUndoName(), "Change Spline Paving Ground Authority");

    undo.Undo();
    controller.Update(world, &journal);
    EXPECT_EQ(world.GetComponent<Components::SplinePaving>(area)->GroundAuthority, SplineGroundAuthority::Auto);
    EXPECT_NE(world.GetComponent<Components::TerrainGrassEffect>(area), nullptr);
    EXPECT_NE(world.GetComponent<Components::TerrainModifierVolume>(area), nullptr);
    EXPECT_FALSE(undo.CanUndo());

    undo.Redo();
    controller.Update(world, &journal);
    EXPECT_EQ(world.GetComponent<Components::SplinePaving>(area)->GroundAuthority, SplineGroundAuthority::None);
    EXPECT_EQ(world.GetComponent<Components::TerrainGrassEffect>(area), nullptr);
    EXPECT_EQ(world.GetComponent<Components::TerrainModifierVolume>(area), nullptr);
    EXPECT_EQ(undo.GetUndoCount(), 1u);
}

TEST(SplineGroundAuthority, UndoOfPickingOwnsRemovesWhatItProvisionedAndKeepsTheRedo)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    Components::SplinePaving none{};
    none.GroundAuthority = SplineGroundAuthority::None;
    const ECS::EntityHandle area = CreatePavedArea(world, SplineECS::SplineService::Get(), none);

    Editor::UndoRedoService undo;
    SplineRecipes::SplineRecipeSystem recipes(nullptr);
    Editor::RecipeEditorHooks journal(&undo, recipes, nullptr);
    SplineRecipes::SplineGroundAuthorityController controller;
    controller.Update(world, &journal);
    ASSERT_EQ(world.GetComponent<Components::TerrainGrassEffect>(area), nullptr);

    undo.Execute(std::make_unique<SetPavingAuthority>(world, area, SplineGroundAuthority::Owns));
    controller.Update(world, &journal);
    ASSERT_NE(world.GetComponent<Components::TerrainGrassEffect>(area), nullptr);
    EXPECT_EQ(undo.GetUndoCount(), 1u) << "the provisioning joins the edit that caused it";

    undo.Undo();
    controller.Update(world, &journal);
    EXPECT_EQ(world.GetComponent<Components::SplinePaving>(area)->GroundAuthority, SplineGroundAuthority::None);
    EXPECT_EQ(world.GetComponent<Components::TerrainGrassEffect>(area), nullptr);
    EXPECT_EQ(world.GetComponent<Components::TerrainModifierVolume>(area), nullptr);
    EXPECT_EQ(world.GetComponent<Components::SplineGroundProvisioned>(area), nullptr);
    EXPECT_EQ(undo.GetUndoCount(), 0u) << "the undo adds no entry";
    EXPECT_TRUE(undo.CanRedo());

    undo.Redo();
    controller.Update(world, &journal);
    EXPECT_EQ(world.GetComponent<Components::SplinePaving>(area)->GroundAuthority, SplineGroundAuthority::Owns);
    EXPECT_NE(world.GetComponent<Components::TerrainGrassEffect>(area), nullptr);
    EXPECT_NE(world.GetComponent<Components::TerrainGroundClaimEffect>(area), nullptr);
    EXPECT_EQ(undo.GetUndoCount(), 1u);
}

TEST(SplineGroundAuthority, StandaloneCleanupOwnsItsSceneRevisionAcrossSave)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const auto area = CreatePavedArea(world, SplineECS::SplineService::Get(), {});
    Editor::UndoRedoService undo;
    Editor::EditorChangeNotifications notifications;
    SplineRecipes::SplineRecipeSystem recipes(nullptr);
    Editor::RecipeEditorHooks journal(&undo, recipes, &notifications);
    SplineRecipes::SplineGroundAuthorityController controller;
    controller.Update(world, &journal);
    Editor::SceneDocumentManager document;
    document.SetWorld(&world);
    document.SetUndoRedoService(&undo);
    document.SetChangeNotifications(&notifications);
    document.MarkClean();

    world.GetComponentForWrite<Components::SplinePaving>(area)->GroundAuthority = SplineGroundAuthority::None;
    controller.Update(world, &journal);
    ASSERT_EQ(world.GetComponent<Components::TerrainGrassEffect>(area), nullptr);
    ASSERT_TRUE(undo.CanUndo());
    EXPECT_TRUE(document.IsDirty());
    document.MarkClean();
    undo.Undo();
    EXPECT_TRUE(document.IsDirty());
    EXPECT_NE(world.GetComponent<Components::TerrainGrassEffect>(area), nullptr);
    undo.Redo();
    EXPECT_FALSE(document.IsDirty());
}
