#include "Placement/RecipeEditorHooks.h"

#include "Editor/Registries/EditorServiceRegistry.h"
#include "Placement/FenceSpanOverrideRemap.h"
#include "UndoRedo/SplineGroundCleanupCommand.h"
#include "UndoRedo/SplinePointEdits.h"
#include "UndoRedo/UndoRedoService.h"

#include "ECS/Systems.h"
#include "SplineRecipes/RecipeControllers.h"
#include "SplineRecipes/SplineRecipeSystem.h"

#include <memory>

namespace GameEngine::Editor
{

std::unique_ptr<IEditorService> RecipeEditorHooks::Create(const EditorServiceContext& context)
{
    auto* recipes = context.Systems.GetSystem<SplineRecipes::SplineRecipeSystem>();
    if (!recipes)
        return nullptr;
    return std::make_unique<RecipeEditorHooks>(context.Undo, *recipes);
}

RecipeEditorHooks::RecipeEditorHooks(UndoRedoService* undo, SplineRecipes::SplineRecipeSystem& recipes)
    : m_Undo(undo),
      m_Recipes(recipes),
      m_PointEditSubscription(SplinePointEdited().Subscribe(
          [&recipes](const SplinePointEdit& edit)
          { RemapFenceOverridesOnPointEdit(edit, recipes.GetControllers().FenceRunSpanCounts(edit.Entity)); }))
{
    m_Recipes.SetEditJournal(this);
}

RecipeEditorHooks::~RecipeEditorHooks()
{
    m_Recipes.SetEditJournal(nullptr);
}

void RecipeEditorHooks::RecordGroundCleanup(ECS::World& world,
                                            const SplineRecipes::SplineGroundCleanup& cleanup)
{
    if (!m_Undo)
    {
        cleanup.Apply(world);
        return;
    }
    m_Undo->Execute(std::make_unique<SplineGroundCleanupCommand>(&world, cleanup));
}

void RecipeEditorHooks::ReleaseSceneScoped(ECS::World& world)
{
    m_Recipes.ReleaseSceneScoped(world);
}

} // namespace GameEngine::Editor
