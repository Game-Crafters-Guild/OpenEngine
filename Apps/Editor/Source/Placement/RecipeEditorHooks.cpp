#include "Placement/RecipeEditorHooks.h"

#include "Editor/Registries/EditorServiceRegistry.h"
#include "EditorChangeNotifications.h"
#include "Placement/FenceSpanOverrideRemap.h"
#include "UndoRedo/Spline/SplineGroundEditCommand.h"
#include "UndoRedo/Spline/SplinePointEdits.h"
#include "UndoRedo/UndoRedoService.h"

#include "ECS/Systems.h"
#include "SplineRecipes/SplineGroundAuthority.h"
#include "SplineRecipes/RecipeControllers.h"
#include "SplineRecipes/SplineRecipeSystem.h"

#include <functional>
#include <memory>

namespace GameEngine::Editor
{

std::unique_ptr<IEditorService> RecipeEditorHooks::Create(const EditorServiceContext& context)
{
    auto* recipes = context.Systems.GetSystem<SplineRecipes::SplineRecipeSystem>();
    if (!recipes)
        return nullptr;
    return std::make_unique<RecipeEditorHooks>(context.Undo, *recipes, context.Notifications);
}

RecipeEditorHooks::RecipeEditorHooks(UndoRedoService* undo, SplineRecipes::SplineRecipeSystem& recipes,
                                     EditorChangeNotifications* notifications)
    : m_Undo(undo),
      m_Recipes(recipes),
      m_Notifications(notifications),
      m_PointEditSubscription(SplinePointEdited().Subscribe(
          [&recipes](const SplinePointEdit& edit)
          { RemapFenceOverridesOnPointEdit(edit, recipes.GetControllers().FenceRunSpanCounts(edit.Entity)); }))
{
    m_Recipes.SetEditJournal(this);
    if (m_Undo)
        m_Undo->SetBeforeCommit([this]()
        {
            if (m_World && m_GroundController && !m_UpdatingGround)
                m_GroundController->Update(*m_World, this);
        });
}

RecipeEditorHooks::~RecipeEditorHooks()
{
    if (m_Undo)
        m_Undo->SetBeforeCommit({});
    m_Recipes.SetEditJournal(nullptr);
}

void RecipeEditorHooks::RecordGroundEdit(ECS::World& world, const SplineRecipes::SplineGroundEdit& edit)
{
    if (!m_Undo)
        return;
    if (m_ActionId != 0 && m_Undo->AppendConsequence(m_ActionId,
            std::make_unique<SplineGroundEditCommand>(&world, edit)))
        return;
    if (edit.Removes())
        m_Undo->CommitAlreadyApplied(std::make_unique<SplineGroundEditCommand>(&world, edit),
            std::bind_front(NotifyWorldStructure, m_Notifications, &world));
}

void RecipeEditorHooks::BeginGroundUpdate(ECS::World& world, SplineRecipes::SplineGroundAuthorityController& controller)
{
    m_UpdatingGround = true;
    m_World = &world;
    m_GroundController = &controller;
    m_ActionId = m_Undo ? m_Undo->GetPendingActionId() : 0;
}

void RecipeEditorHooks::EndGroundUpdate()
{
    m_UpdatingGround = false;
}

void RecipeEditorHooks::ReleaseSceneScoped(ECS::World& world)
{
    m_World = nullptr;
    m_GroundController = nullptr;
    m_ActionId = 0;
    m_Recipes.ReleaseSceneScoped(world);
}

} // namespace GameEngine::Editor
