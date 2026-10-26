#include "SplineRecipes/RecipeBuildStates.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplineFence.h"
#include "Components/Spline/SplinePlacement.h"
#include "Components/Spline/SplineWall.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

namespace GameEngine::SplineRecipes
{
namespace
{

// True when `entity` matches a recipe controller's query: a recipe the controllers update.
bool IsRecipe(const ECS::World& world, ECS::EntityHandle entity)
{
    if (!world.HasComponent<Components::SplineComponent>(entity) ||
        !world.HasComponent<Components::WorldTransform>(entity))
        return false;
    return world.HasComponent<Components::SplineExtrude>(entity) ||
           world.HasComponent<Components::SplineWall>(entity) ||
           world.HasComponent<Components::SplineFence>(entity) ||
           world.HasComponent<Components::SplinePlacement>(entity);
}

// Calls `visit` with each recipe of kind RecipeT: the entities a recipe controller's query matches.
template <typename RecipeT, typename VisitT>
void ForEachRecipeOfKind(ECS::World& world, VisitT&& visit)
{
    world.Query<ECS::Read<Components::SplineComponent>, ECS::Read<RecipeT>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle e, const Components::SplineComponent&, const RecipeT&,
                  const Components::WorldTransform&) { visit(e); });
}

template <typename VisitT>
void ForEachRecipe(ECS::World& world, VisitT&& visit)
{
    ForEachRecipeOfKind<Components::SplineExtrude>(world, visit);
    ForEachRecipeOfKind<Components::SplineWall>(world, visit);
    ForEachRecipeOfKind<Components::SplineFence>(world, visit);
    ForEachRecipeOfKind<Components::SplinePlacement>(world, visit);
}

} // namespace

void RecipeBuildStates::BeginUpdate(const ECS::World& world)
{
    const uint64 resetGeneration = world.GetLifecycleResetGeneration();
    if (m_World != &world || m_WorldResetGeneration != resetGeneration)
    {
        m_Entries.Clear();
        m_SettledAtLastUpdate = false;
    }
    m_World = &world;
    m_WorldResetGeneration = resetGeneration;
    ++m_VisitStamp;
}

void RecipeBuildStates::Set(ECS::EntityHandle recipe, BuildState state, std::string_view reason)
{
    Entry& entry = m_Entries.GetOrInsert(recipe.id);
    entry.State = state;
    entry.Reason.assign(reason);
    entry.VisitStamp = m_VisitStamp;
}

void RecipeBuildStates::FailEveryRecipe(ECS::World& world, std::string_view reason)
{
    ForEachRecipe(world, [&](ECS::EntityHandle recipe) { Set(recipe, BuildState::Failed, reason); });
}

void RecipeBuildStates::EndUpdate(ECS::World& world)
{
    m_Entries.EraseIf([this](uint32, const Entry& entry) { return entry.VisitStamp != m_VisitStamp; });
    m_Failures.clear();
    for (const auto& [id, entry] : m_Entries)
    {
        if (entry.State != BuildState::Failed)
            continue;
        ECS::EntityHandle recipe;
        recipe.id = id;
        m_Failures.push_back({recipe, BuildState::Failed});
    }
    m_SettledAtLastUpdate = IsSceneSettled(world);
}

const RecipeBuildStates::Entry* RecipeBuildStates::Find(const ECS::World& world,
                                                        ECS::EntityHandle recipe) const
{
    if (m_World != &world || m_WorldResetGeneration != world.GetLifecycleResetGeneration())
        return nullptr;
    return m_Entries.Find(recipe.id);
}

RecipeStatus RecipeBuildStates::StateOf(ECS::World& world, ECS::EntityHandle recipe) const
{
    if (!world.IsValid(recipe) || !IsRecipe(world, recipe))
        return {recipe, BuildState::Built};
    const Entry* entry = Find(world, recipe);
    return {recipe, entry ? entry->State : BuildState::Pending};
}

std::string_view RecipeBuildStates::ReasonOf(ECS::World& world, ECS::EntityHandle recipe) const
{
    const Entry* entry = Find(world, recipe);
    return entry ? std::string_view(entry->Reason) : std::string_view();
}

bool RecipeBuildStates::IsSceneSettled(ECS::World& world) const
{
    bool settled = true;
    ForEachRecipe(world, [&](ECS::EntityHandle recipe)
    {
        const BuildState state = StateOf(world, recipe).State;
        if (state == BuildState::Pending || state == BuildState::Held)
            settled = false;
    });
    return settled;
}

bool RecipeBuildStates::IsSceneSettledAtLastUpdate(const ECS::World& world) const
{
    return m_World == &world && m_WorldResetGeneration == world.GetLifecycleResetGeneration() &&
           m_SettledAtLastUpdate;
}

std::span<const RecipeStatus> RecipeBuildStates::Failures(ECS::World& world) const
{
    if (m_World != &world || m_WorldResetGeneration != world.GetLifecycleResetGeneration())
        return {};
    return m_Failures;
}

} // namespace GameEngine::SplineRecipes
