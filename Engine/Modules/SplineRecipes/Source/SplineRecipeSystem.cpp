#include "SplineRecipes/SplineRecipeSystem.h"

#include "ECS/SystemScheduling.h"
#include "SplineRecipes/RecipeControllers.h"

namespace GameEngine::SplineRecipes
{

SplineRecipeSystem::SplineRecipeSystem(Engine::Renderer::RenderServices* renderServices)
    : m_RenderServices(renderServices), m_Controllers(std::make_unique<RecipeControllers>())
{
}

SplineRecipeSystem::~SplineRecipeSystem() = default;

void SplineRecipeSystem::Update(ECS::World& world, float32 deltaTime)
{
    m_Controllers->Update(world, m_RenderServices, m_EditJournal, deltaTime);
}

void SplineRecipeSystem::SetEditJournal(RecipeEditJournal* journal)
{
    m_EditJournal = journal;
}

const RecipeControllers& SplineRecipeSystem::GetControllers() const
{
    return *m_Controllers;
}

void SplineRecipeSystem::ReleaseSceneScoped(ECS::World& world)
{
    m_Controllers->ReleaseSceneScoped(world, m_RenderServices);
}

void AddSplineRecipeSystemsToSchedule(ECS::SystemScheduleBuilder& b,
                                      Engine::Renderer::RenderServices* renderServices)
{
    b.Add<SplineRecipeSystem>("SplineRecipes", ECS::SystemPhase::Extraction, 1,
                              {"TerrainModifiers", "SplineExtraction"}, renderServices);
}

} // namespace GameEngine::SplineRecipes
