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
#include "ECS/Query.h"
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

// The map handle the surface on `entity` owns.
inline Pathfinding::NavMapHandle SurfaceMap(ECS::World& world, ECS::Entity& entity)
{
    const auto* surface = entity.Get<Components::NavigationSurface>();
    return surface ? ResolveNavigationSurfaceMap(world, *surface) : Pathfinding::NavMapHandle{};
}

// The entity whose NavigationSurface owns `map`, for an agent's Surface in a scene with several.
inline ECS::EntityHandle SurfaceOwning(ECS::World& world, Pathfinding::NavMapHandle map)
{
    ECS::EntityHandle owner{};
    world.Query<ECS::Read<Components::NavigationSurface>>().Each(
        [&](ECS::EntityHandle entity, const Components::NavigationSurface& surface) {
            if (ResolveNavigationSurfaceMap(world, surface) == map)
                owner = entity;
        });
    return owner;
}

// A baked grid NavigationSurface on a new entity that owns `map`, a grid the test fills in cell by
// cell, so agents walk it as they walk a baked surface. The suites that own such a map never run
// NavigationSurfaceSystem, which would bake over it.
inline ECS::Entity AdoptGridMap(ECS::World& world, Pathfinding::NavMapHandle map)
{
    Components::NavigationSurface surface{};
    surface.Mode = Components::NavigationSurfaceMode::Grid;
    surface.BakedMode = Components::NavigationSurfaceMode::Grid;
    surface.BakePhase = Components::NavigationBakePhase::Baked;
    surface.MapHoldsBake = true;
    surface.NavMapIndex = map.Index;
    surface.NavMapGeneration = map.Generation;
    surface.OwnerWorldId = world.GetWorldId();
    auto entity = world.Create();
    entity.Set(surface);
    world.ProcessCommands();
    return entity;
}

} // namespace GameEngine::PathfindingECS::Testing
