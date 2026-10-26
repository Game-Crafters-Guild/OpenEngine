#include "Inspectors/SplineInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/InspectorNotice.h"

#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplineWall.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "SplineRecipes/SplineRecipesScriptApi.h"

#include <functional>
#include <string>

namespace GameEngine
{
namespace
{

using SplineRecipes::BuildState;

// A Built recipe with a reason made nothing: the reason says why.
bool BuiltNothing(ECS::World& world, ECS::EntityHandle recipe, BuildState state)
{
    return state == BuildState::Built && !SplineRecipes::ReasonOf(world, recipe).empty();
}

// A wait ends without an action, so it shows on the neutral plate; an empty build asks for one.
EditorUI::InspectorNotice::Kind NoticeKind(BuildState state, bool builtNothing)
{
    if (state == BuildState::Failed)
        return EditorUI::InspectorNotice::Kind::Error;
    if (builtNothing)
        return EditorUI::InspectorNotice::Kind::Warning;
    return EditorUI::InspectorNotice::Kind::Information;
}

// The words of the state: a title with no period, and a text of full sentences.
void ShowState(EditorUI::InspectorNotice& notice, ECS::World& world, ECS::EntityHandle recipe, BuildState state)
{
    switch (state)
    {
    case BuildState::Pending:
        notice.SetTitle("Not built yet");
        notice.SetText("It builds once its inputs stop changing.");
        return;
    case BuildState::Held:
        notice.SetTitle("Waiting");
        notice.SetGlyph(EditorUI::InspectorNotice::Glyph::Clock);
        notice.SetText(SplineRecipes::ReasonOf(world, recipe));
        return;
    case BuildState::Built:
        if (BuiltNothing(world, recipe, state))
        {
            notice.SetTitle("Built, but empty");
            notice.SetText(SplineRecipes::ReasonOf(world, recipe));
            return;
        }
        notice.SetTitle("Built");
        notice.SetGlyph(EditorUI::InspectorNotice::Glyph::Check);
        notice.SetText("The output matches the recipe.");
        return;
    case BuildState::Failed:
        notice.SetTitle("Failed");
        notice.SetText(SplineRecipes::ReasonOf(world, recipe));
        return;
    }
}

// The notice of one recipe and the state it shows.
struct BuildStateFollower
{
    EditorUI::InspectorNotice* Notice = nullptr;
    ECS::World* World = nullptr;
    ECS::EntityHandle Recipe{};
    std::function<void()> RequestRefresh;
    BuildState Shown = BuildState::Pending;
    bool ShownBuiltNothing = false;
    bool RefreshRequested = false;
};

// The recipe system sets the state; the notice follows it. A new state can need another kind
// of notice, so it rebuilds the inspector.
void FollowBuildState(BuildStateFollower& follower)
{
    if (follower.RefreshRequested)
        return;
    const BuildState current = SplineRecipes::StateOf(*follower.World, follower.Recipe).State;
    const bool builtNothing = BuiltNothing(*follower.World, follower.Recipe, current);
    if (current == follower.Shown && builtNothing == follower.ShownBuiltNothing)
    {
        if (current == BuildState::Held || current == BuildState::Failed || builtNothing)
            follower.Notice->SetText(SplineRecipes::ReasonOf(*follower.World, follower.Recipe));
        return;
    }
    if (!follower.RequestRefresh)
        return;
    follower.RefreshRequested = true;
    follower.RequestRefresh();
}

template <typename RecipeT>
void BuildSplineRecipeInspector(const InspectorContext& context)
{
    if (!context.World || !context.World->GetComponent<RecipeT>(context.Entity))
        return;
    AddSplineRecipeBuildStateNotice(context);
    RenderDefaultComponentInspector(context, ECS::GetComponentTypeId<RecipeT>());
}

} // namespace

void AddSplineRecipeBuildStateNotice(const InspectorContext& context)
{
    if (!context.Parent || !context.World || !context.Entity.IsValid())
        return;
    const BuildState state = SplineRecipes::StateOf(*context.World, context.Entity).State;
    const bool builtNothing = BuiltNothing(*context.World, context.Entity, state);
    EditorUI::InspectorNotice* notice =
        InspectorUI::AddNotice(context.Parent, std::string(), NoticeKind(state, builtNothing));
    notice->SetId("spline-recipe-build-state");
    ShowState(*notice, *context.World, context.Entity, state);
    if (!context.SimulationRefreshCallbacks)
        return;
    BuildStateFollower follower{notice, context.World, context.Entity, context.RequestInspectorRefresh, state,
                                builtNothing};
    context.SimulationRefreshCallbacks->push_back([follower]() mutable
                                                  { FollowBuildState(follower); });
}

void RegisterSplineRecipeInspectors()
{
    InspectorRegistry& registry = InspectorRegistry::Get();
    registry.RegisterComponentInspector<Components::SplineExtrude>(BuildSplineRecipeInspector<Components::SplineExtrude>);
    registry.RegisterComponentInspector<Components::SplineWall>(BuildSplineRecipeInspector<Components::SplineWall>);
}

} // namespace GameEngine
