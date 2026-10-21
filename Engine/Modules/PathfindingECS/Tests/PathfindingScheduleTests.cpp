#include <gtest/gtest.h>

#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "PathfindingECS/Systems/RegisterPathfindingSystems.h"
#include "PhysicsECS/Systems/RegisterPhysicsSystems.h"

#include <cstdint>
#include <string_view>

namespace
{

using namespace GameEngine;

constexpr size_t kWaveNotFound = SIZE_MAX;

// Stands in for a system another module owns, so these fixtures can supply the
// cross-module dependency names the pathfinding schedule declares. The schedule
// name is a constructor argument, so one type serves any number of names.
// Not final: the builder wraps it in Detail::NamedSystem<T>, which derives from it.
class StubSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "Stub"; }
    void Update(ECS::World&, float32) override {}
};

// Wave index of the named system in the manager's execution plan.
size_t FindWaveOf(const ECS::SystemManager& manager, std::string_view name)
{
    const auto& plan = manager.GetExecutionPlan();
    for (size_t wi = 0; wi < plan.Waves.size(); ++wi)
    {
        for (size_t idx : plan.Waves[wi].SystemIndices)
        {
            const char* systemName = manager.GetSequentialSystemName(idx);
            if (systemName && name == systemName)
                return wi;
        }
    }
    return kWaveNotFound;
}

} // namespace

// NavigationSurfaceSystem reads PhysicsBody and the collider shape components to find the static
// colliders it bakes, and the physics pipeline's systems write them; systems sharing a wave dispatch
// to parallel job workers. Depending on PhysicsWriteback, the pipeline's terminal system, puts the
// bake in a strictly later wave than every one of them.
TEST(PathfindingScheduleTests, NavigationSurfaceRunsAfterWholePhysicsPipeline)
{
    ECS::SystemScheduleBuilder builder;
    PhysicsECS::AddPhysicsSystemsToSchedule(builder);
    PathfindingECS::AddPathfindingSystemsToSchedule(builder);

    ECS::SystemManager manager;
    // Partial: TransformHierarchy is not registered in this two-module schedule,
    // which is the point of the fixture — the physics margin must hold on its own.
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Partial);

    const size_t bootstrapWave = FindWaveOf(manager, "PhysicsWorldBootstrap");
    const size_t initWave = FindWaveOf(manager, "PhysicsInit");
    const size_t stepWave = FindWaveOf(manager, "PhysicsStep");
    const size_t writebackWave = FindWaveOf(manager, "PhysicsWriteback");
    const size_t navBuildWave = FindWaveOf(manager, "NavigationSurfaceSystem");

    ASSERT_NE(bootstrapWave, kWaveNotFound);
    ASSERT_NE(initWave, kWaveNotFound);
    ASSERT_NE(stepWave, kWaveNotFound);
    ASSERT_NE(writebackWave, kWaveNotFound);
    ASSERT_NE(navBuildWave, kWaveNotFound);

    EXPECT_GT(navBuildWave, writebackWave);
    EXPECT_GT(navBuildWave, stepWave);
    EXPECT_GT(navBuildWave, initWave);
    EXPECT_GT(navBuildWave, bootstrapWave);
}

// A surface bakes around the entity's WorldTransform and places its geometry with theirs, which
// TransformHierarchySystem produces. Baking in an earlier wave reads unresolved poses.
TEST(PathfindingScheduleTests, NavigationSurfaceRunsAfterTransformHierarchy)
{
    ECS::SystemScheduleBuilder builder;
    builder.Add<StubSystem>("TransformHierarchy", ECS::SystemPhase::Extraction, 1, {});
    PhysicsECS::AddPhysicsSystemsToSchedule(builder);
    PathfindingECS::AddPathfindingSystemsToSchedule(builder);

    ECS::SystemManager manager;
    // Complete: every required name this schedule declares now resolves, so an
    // unresolved one is a defect. This is what catches a typo'd dependency name,
    // which would otherwise silently drop the edge and share a wave.
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Complete);

    const size_t hierarchyWave = FindWaveOf(manager, "TransformHierarchy");
    const size_t navBuildWave = FindWaveOf(manager, "NavigationSurfaceSystem");

    ASSERT_NE(hierarchyWave, kWaveNotFound);
    ASSERT_NE(navBuildWave, kWaveNotFound);
    EXPECT_GT(navBuildWave, hierarchyWave);
}

// The nav systems' own chain stays strictly ordered; each reads what the
// previous one wrote.
TEST(PathfindingScheduleTests, NavigationSystemsFormAStrictChain)
{
    ECS::SystemScheduleBuilder builder;
    builder.Add<StubSystem>("TransformHierarchy", ECS::SystemPhase::Extraction, 1, {});
    PhysicsECS::AddPhysicsSystemsToSchedule(builder);
    PathfindingECS::AddPathfindingSystemsToSchedule(builder);

    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Complete);

    const size_t buildWave = FindWaveOf(manager, "NavigationSurfaceSystem");
    const size_t pathWave = FindWaveOf(manager, "NavigationPathfindingSystem");
    const size_t moveWave = FindWaveOf(manager, "NavigationMovementSystem");
    const size_t debugWave = FindWaveOf(manager, "NavigationDebugSystem");

    ASSERT_NE(buildWave, kWaveNotFound);
    ASSERT_NE(pathWave, kWaveNotFound);
    ASSERT_NE(moveWave, kWaveNotFound);
    ASSERT_NE(debugWave, kWaveNotFound);

    EXPECT_GT(pathWave, buildWave);
    EXPECT_GT(moveWave, pathWave);
    EXPECT_GT(debugWave, moveWave);
}
