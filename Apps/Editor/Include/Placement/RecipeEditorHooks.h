#pragma once

#include "Editor/Registries/EditorService.h"
#include "Events/Event.h"
#include "SplineRecipes/RecipeEditJournal.h"

#include <memory>

namespace GameEngine::SplineRecipes { class SplineRecipeSystem; }

namespace GameEngine::Editor
{

struct EditorServiceContext;
class UndoRedoService;

// The editor's side of the spline recipes: the undo journal of the recipe controllers, the
// fence's span override remap on spline point edits, and the release at a scene swap.
class RecipeEditorHooks final : public SplineRecipes::RecipeEditJournal, public IEditorService
{
public:
    // The editor service factory (EditorServiceRegistry): null with no spline recipe system.
    static std::unique_ptr<IEditorService> Create(const EditorServiceContext& context);

    // `undo` may be null: a cleanup then applies with no undo step.
    RecipeEditorHooks(UndoRedoService* undo, SplineRecipes::SplineRecipeSystem& recipes);
    ~RecipeEditorHooks() override;

    RecipeEditorHooks(const RecipeEditorHooks&) = delete;
    RecipeEditorHooks& operator=(const RecipeEditorHooks&) = delete;

    void RecordGroundCleanup(ECS::World& world, const SplineRecipes::SplineGroundCleanup& cleanup) override;
    void ReleaseSceneScoped(ECS::World& world) override;

private:
    UndoRedoService* m_Undo = nullptr;
    SplineRecipes::SplineRecipeSystem& m_Recipes;
    EventSubscription m_PointEditSubscription;
};

} // namespace GameEngine::Editor
