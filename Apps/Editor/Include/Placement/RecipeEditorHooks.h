#pragma once

#include "Editor/Registries/EditorService.h"
#include "Events/Event.h"
#include "SplineRecipes/RecipeEditJournal.h"

#include <memory>
#include <cstdint>

namespace GameEngine::SplineRecipes { class SplineRecipeSystem; class SplineGroundAuthorityController; }

namespace GameEngine::Editor
{

struct EditorServiceContext;
class UndoRedoService;
class EditorChangeNotifications;

// The editor's side of the spline recipes: the undo journal of the recipe controllers, the
// fence's span override remap on spline point edits, and the release at a scene swap.
class RecipeEditorHooks final : public SplineRecipes::RecipeEditJournal, public IEditorService
{
public:
    // The editor service factory (EditorServiceRegistry): null with no spline recipe system.
    static std::unique_ptr<IEditorService> Create(const EditorServiceContext& context);

    // `undo` may be null: a ground edit then has no undo step.
    RecipeEditorHooks(UndoRedoService* undo, SplineRecipes::SplineRecipeSystem& recipes,
                      EditorChangeNotifications* notifications);
    ~RecipeEditorHooks() override;

    RecipeEditorHooks(const RecipeEditorHooks&) = delete;
    RecipeEditorHooks& operator=(const RecipeEditorHooks&) = delete;

    void RecordGroundEdit(ECS::World& world, const SplineRecipes::SplineGroundEdit& edit) override;
    void BeginGroundUpdate(ECS::World& world, SplineRecipes::SplineGroundAuthorityController& controller) override;
    void EndGroundUpdate() override;
    void ReleaseSceneScoped(ECS::World& world) override;

private:
    UndoRedoService* m_Undo = nullptr;
    SplineRecipes::SplineRecipeSystem& m_Recipes;
    EditorChangeNotifications* m_Notifications = nullptr;
    EventSubscription m_PointEditSubscription;
    ECS::World* m_World = nullptr;
    SplineRecipes::SplineGroundAuthorityController* m_GroundController = nullptr;
    std::uint64_t m_ActionId = 0;
    bool m_UpdatingGround = false;
};

} // namespace GameEngine::Editor
