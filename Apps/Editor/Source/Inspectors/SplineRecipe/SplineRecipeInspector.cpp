#include "Inspectors/Spline/SplineInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/SplineRecipe/SplineRecipeStateNotice.h"
#include "UI/Controls/InspectorNotice.h"

#include "Components/Spline/SplineWall.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "SplineRecipes/SplineRecipesScriptApi.h"
#include "Types/StringUtils.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>

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

void ShowWords(EditorUI::InspectorNotice& notice, const BuildStateWords& words)
{
    notice.SetTitle(words.Title);
    if (words.Glyph != EditorUI::InspectorNotice::Glyph::OfKind)
        notice.SetGlyph(words.Glyph);
    notice.SetText(words.Text);
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

void BuildSplineWallInspector(const InspectorContext& context)
{
    if (!context.World || !context.World->GetComponent<Components::SplineWall>(context.Entity))
        return;
    AddSplineRecipeBuildStateNotice(context);
    RenderDefaultComponentInspector(context, ECS::GetComponentTypeId<Components::SplineWall>());
}

} // namespace

BuildStateWords DescribeBuildState(BuildState state, std::string_view reason, std::span<const std::string> warnings)
{
    using EditorUI::InspectorNotice;
    switch (state)
    {
    case BuildState::Pending:
        return {"Not built yet", "It builds once its inputs stop changing.", InspectorNotice::Kind::Information};
    case BuildState::Held:
        return {"Waiting", std::string(reason), InspectorNotice::Kind::Information, InspectorNotice::Glyph::Clock};
    case BuildState::Built:
        if (!reason.empty())
            return {"Built, but empty", std::string(reason), InspectorNotice::Kind::Warning};
        if (!warnings.empty())
            return {"Built, " + std::to_string(warnings.size()) + (warnings.size() == 1u ? " warning" : " warnings"),
                    Join(warnings, "\n"), InspectorNotice::Kind::Warning};
        return {"Built", "It is up to date.", InspectorNotice::Kind::Information, InspectorNotice::Glyph::Check};
    case BuildState::Failed:
        break;
    }
    return {"Failed", std::string(reason), InspectorNotice::Kind::Error};
}

void AddBuildStateNotice(const InspectorContext& context, BuildState state, std::string_view reason,
                         std::span<const std::string> warnings)
{
    if (!context.Parent || !context.World || !context.Entity.IsValid())
        return;
    const bool builtNothing = state == BuildState::Built && !reason.empty();
    const BuildStateWords words = DescribeBuildState(state, reason, warnings);
    EditorUI::InspectorNotice* notice = InspectorUI::AddNotice(context.Parent, std::string(), words.Kind);
    notice->SetId("spline-recipe-build-state");
    ShowWords(*notice, words);
    if (!context.SimulationRefreshCallbacks)
        return;
    BuildStateFollower follower{notice, context.World, context.Entity, context.RequestInspectorRefresh, state,
                                builtNothing};
    context.SimulationRefreshCallbacks->push_back([follower]() mutable
                                                  { FollowBuildState(follower); });
}

void AddSplineRecipeBuildStateNotice(const InspectorContext& context)
{
    if (!context.World || !context.Entity.IsValid())
        return;
    AddBuildStateNotice(context, SplineRecipes::StateOf(*context.World, context.Entity).State,
                        SplineRecipes::ReasonOf(*context.World, context.Entity), {});
}

void RegisterSplineWallInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<Components::SplineWall>(BuildSplineWallInspector);
}

} // namespace GameEngine
