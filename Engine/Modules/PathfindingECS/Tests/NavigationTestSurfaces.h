#pragma once

// Surfaces for the navigation suites: level ground and a NavigationSurface baked over it by
// NavigationSurfaceSystem, as a scene would have them.

#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"
#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

namespace GameEngine::PathfindingECS::Testing
{

// A static body whose plane collider is level ground at y = 0, `halfExtent` metres about the origin.
inline ECS::Entity CreateGroundPlane(ECS::World& world, float32 halfExtent = 1000.0f)
{
    auto entity = world.Create();
    Components::PhysicsBody body{};
    body.motionType = Physics::MotionType::Static;
    entity.Set(body);
    entity.Set(Components::PhysicsCollider{});
    Components::PlaneColliderShape plane{};
    plane.halfExtent = halfExtent;
    entity.Set(plane);
    entity.Set(Components::Transform{});
    entity.Set(Components::WorldTransform{});
    world.ProcessCommands();
    return entity;
}

// Runs one NavigationSurfaceSystem update, which bakes a grid surface or up to
// kTileSnapshotsPerUpdate mesh tiles.
inline void BakeSurfaces(ECS::World& world)
{
    NavigationSurfaceSystem system;
    system.Update(world, 1.0f / 60.0f);
    world.ProcessCommands();
}

// A grid surface over level ground whose cell (0, 0) has its minimum corner at the world origin:
// cell (x, z) is centred on ((x + 0.5) * cellSize, (z + 0.5) * cellSize).
inline ECS::Entity CreateFlatGridSurface(ECS::World& world, uint32 width, uint32 depth, float32 cellSize,
                                         Pathfinding::GridType gridType = Pathfinding::GridType::Square)
{
    CreateGroundPlane(world);
    auto entity = world.Create();
    Components::NavigationSurface surface{};
    surface.Mode = Components::NavigationSurfaceMode::Grid;
    surface.GridType = gridType;
    surface.CellSize = cellSize;
    surface.FitBounds = false;
    const float32 halfX = 0.5f * static_cast<float32>(width) * cellSize;
    const float32 halfZ = 0.5f * static_cast<float32>(depth) * cellSize;
    surface.Center = {halfX, 0.0f, halfZ};
    surface.Extent = {halfX, 1.0f, halfZ};
    entity.Set(surface);
    world.ProcessCommands();
    BakeSurfaces(world);
    return entity;
}

// The map handle the surface on `entity` owns, for binding agents to it.
inline Pathfinding::NavMapHandle SurfaceMap(ECS::World& world, ECS::Entity& entity)
{
    const auto* surface = entity.Get<Components::NavigationSurface>();
    return surface ? ResolveNavigationSurfaceMap(world, *surface) : Pathfinding::NavMapHandle{};
}

} // namespace GameEngine::PathfindingECS::Testing
