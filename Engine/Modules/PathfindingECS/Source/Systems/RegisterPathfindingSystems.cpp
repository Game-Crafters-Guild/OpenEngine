#include "PathfindingECS/Systems/RegisterPathfindingSystems.h"
#include "ECS/SystemScheduling.h"
#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "PathfindingECS/Systems/NavigationDebugSystem.h"

namespace GameEngine::PathfindingECS
{

void AddPathfindingSystemsToSchedule(ECS::SystemScheduleBuilder& schedule)
{
    // NavigationSurfaceSystem bakes from finished products:
    //   - TransformHierarchy resolves the WorldTransforms the bake places the surface and its
    //     geometry with. A freshly loaded entity holds no pose until it runs (Version 0).
    //   - PhysicsWriteback terminates the physics pipeline, whose systems write the PhysicsBody and
    //     collider components the bake reads to find static colliders; after it, the bake never
    //     shares a wave with a writer of those components.
    //   - TerrainModifiers and TerrainExtraction build the terrain heightfields the bake reads through
    //     TerrainService. Optional: a schedule without the terrain module has no terrain to bake.
    schedule.Add<NavigationSurfaceSystem>("NavigationSurfaceSystem", ECS::SystemPhase::Early, 10,
                                          {"TransformHierarchy", "PhysicsWriteback",
                                           ECS::OptionalDependency("TerrainModifiers"),
                                           ECS::OptionalDependency("TerrainExtraction")});
    schedule.Add<NavigationPathfindingSystem>("NavigationPathfindingSystem", ECS::SystemPhase::Early, 20,
                                 {"NavigationSurfaceSystem"});
    schedule.Add<NavigationMovementSystem>("NavigationMovementSystem", ECS::SystemPhase::Early, 30,
                              {"NavigationPathfindingSystem"});
    schedule.Add<NavigationDebugSystem>("NavigationDebugSystem", ECS::SystemPhase::Render, 5,
                               {"NavigationMovementSystem"});
}

} // namespace GameEngine::PathfindingECS
