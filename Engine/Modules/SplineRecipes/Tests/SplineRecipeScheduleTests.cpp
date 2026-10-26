// Where the recipe system runs in the frame. The controllers register meshes,
// create and destroy entities and read the terrain height view, so the system
// must run alone on the updating thread, and after the two systems whose output
// a build reads: the terrain bake (TerrainModifiers) and the spline caches
// (SplineExtraction).

#include "SplineRecipes/SplineRecipeSystem.h"
#include "SplineECS/Systems/RegisterSplineSystems.h"
#include "TerrainECS/Systems/RegisterTerrainSystems.h"
#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>

namespace GameEngine::SplineRecipes
{
namespace
{

int WaveOf(const ECS::SystemManager& manager, std::string_view systemName)
{
    const ECS::SystemExecutionPlan& plan = manager.GetExecutionPlan();
    for (std::size_t wave = 0; wave < plan.Waves.size(); ++wave)
    {
        for (std::size_t index : plan.Waves[wave].SystemIndices)
        {
            const char* name = manager.GetSequentialSystemName(index);
            if (name && std::string_view(name) == systemName)
                return static_cast<int>(wave);
        }
    }
    return -1;
}

TEST(SplineRecipeSchedule, TheSystemUpdatesExclusively)
{
    EXPECT_TRUE(SplineRecipeSystem{nullptr}.RequiresExclusiveUpdate());
}

// Built from the modules' own schedule contributions only, so an undeclared edge
// leaves the recipe system in a wave beside the system it reads.
TEST(SplineRecipeSchedule, TheSystemRunsAfterTheTerrainBakeAndTheSplineCaches)
{
    ECS::SystemScheduleBuilder builder;
    SplineECS::AddSplineSystemsToSchedule(builder);
    TerrainECS::AddTerrainSystemsToSchedule(builder, nullptr);
    AddSplineRecipeSystemsToSchedule(builder, nullptr);

    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Partial);
    ASSERT_TRUE(manager.GetExecutionPlan().IsValid());

    const int recipeWave = WaveOf(manager, "SplineRecipes");
    ASSERT_GE(recipeWave, 0) << "AddSplineRecipeSystemsToSchedule does not add the recipe system";
    EXPECT_GT(recipeWave, WaveOf(manager, "TerrainModifiers"))
        << "the recipe system does not declare a dependency on TerrainModifiers";
    EXPECT_GT(recipeWave, WaveOf(manager, "SplineExtraction"))
        << "the recipe system does not declare a dependency on SplineExtraction";
}

} // namespace
} // namespace GameEngine::SplineRecipes
