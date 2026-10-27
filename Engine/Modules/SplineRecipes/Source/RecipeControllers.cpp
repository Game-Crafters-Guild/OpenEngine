#include "SplineRecipes/RecipeControllers.h"

#include "Core/CpuProfiler.h"
#include "SplineRecipes/SplineExtrudeController.h"
#include "SplineRecipes/SplineFenceController.h"
#include "SplineRecipes/SplineGroundAuthority.h"
#include "SplineRecipes/SplinePlacementController.h"
#include "SplineRecipes/SplineWallController.h"

namespace GameEngine::SplineRecipes
{

RecipeControllers::RecipeControllers()
    : m_Placement(std::make_unique<SplinePlacementController>()),
      m_Fence(std::make_unique<SplineFenceController>()),
      m_Extrude(std::make_unique<SplineExtrudeController>()),
      m_Wall(std::make_unique<SplineWallController>()),
      m_GroundAuthority(std::make_unique<SplineGroundAuthorityController>())
{
}

RecipeControllers::~RecipeControllers() = default;

void RecipeControllers::UpdateGenerators(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                                         float32 deltaSeconds)
{
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplinePlacement");
        m_Placement->Update(world, renderServices, deltaSeconds, m_BuildStates);
    }
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplineFence");
        m_Fence->Update(world, renderServices, deltaSeconds, m_BuildStates);
    }
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplineExtrude");
        m_Extrude->Update(world, renderServices, deltaSeconds, m_BuildStates);
    }
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplineWall");
        m_Wall->Update(world, renderServices, deltaSeconds, m_BuildStates);
    }
}

void RecipeControllers::Update(ECS::World& world, Engine::Renderer::RenderServices* renderServices,
                               RecipeEditJournal* journal, float32 deltaSeconds)
{
    m_BuildStates.BeginUpdate(world);
    if (renderServices)
        UpdateGenerators(world, *renderServices, deltaSeconds);
    else
        m_BuildStates.FailEveryRecipe(world, kNoRenderDeviceReason);
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplineGroundAuthority");
        m_GroundAuthority->Update(world, journal);
    }
    m_BuildStates.EndUpdate(world);
}

void RecipeControllers::ReleaseSceneScoped(ECS::World& world,
                                           Engine::Renderer::RenderServices* renderServices)
{
    m_Placement->Shutdown(world);
    m_Fence->Shutdown(world, renderServices);
    m_Extrude->Shutdown(world, renderServices);
    m_Wall->Shutdown(world, renderServices);
    m_GroundAuthority->Shutdown();
}

std::span<const uint32> RecipeControllers::FenceRunSpanCounts(ECS::EntityHandle entity) const
{
    return m_Fence->RunSpanCounts(entity);
}

std::span<const FenceMeshBounds> RecipeControllers::FencePartBounds(ECS::EntityHandle entity) const
{
    return m_Fence->PartBounds(entity);
}

} // namespace GameEngine::SplineRecipes
