#pragma once

// Surfaces for the navigation suites: level ground and a NavigationSurface baked over it by
// NavigationSurfaceSystem, as a scene would have them.

#include "Components/Navigation/NavigationSurface.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"
#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"
#include "Pathfinding/GridMap.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "TerrainECS/TerrainService.h"
#include "Types/Fnv1a.h"

#include <cmath>

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

// A flat 128 m terrain about the origin with a 1 m lattice, its heights all 0. TerrainService must be up.
inline TerrainECS::TerrainData* CreateFlatTerrain(ECS::World& world)
{
    Terrain::TerrainConfig config{};
    config.HeightmapWidth = config.HeightmapHeight = 129;
    config.WorldSizeX = config.WorldSizeZ = 128.0f;
    config.HeightScale = 10.0f;
    config.LODLevels = 2;
    const auto handle = TerrainECS::TerrainService::Get().CreateTerrain(config);
    auto* data = TerrainECS::TerrainService::Get().GetTerrainData(handle);
    data->MarkFullDirty();
    auto entity = world.Create();
    Components::Terrain terrain{};
    terrain.SizeX = terrain.SizeZ = 128.0f;
    terrain.HeightScale = 10.0f;
    terrain.BaseSource = Components::TerrainBaseSource::Flat;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    entity.Set(terrain);
    entity.Set(Components::Transform{});
    Components::WorldTransform placed{};
    placed.Version = 1u;
    entity.Set(placed);
    world.ProcessCommands();
    return data;
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

// A static box collider of half size (halfX, halfY, halfZ) about (x, y, z), turned about +Y so its
// local X axis lies along (cosine, 0, -sine).
inline ECS::Entity CreateTurnedBox(ECS::World& world, float32 x, float32 y, float32 z, float32 halfX, float32 halfY,
                                   float32 halfZ, float32 cosine, float32 sine)
{
    auto entity = world.Create();
    Components::PhysicsBody body{};
    body.motionType = Physics::MotionType::Static;
    entity.Set(body);
    entity.Set(Components::PhysicsCollider{});
    Components::BoxColliderShape shape{};
    shape.halfExtentsX = halfX;
    shape.halfExtentsY = halfY;
    shape.halfExtentsZ = halfZ;
    entity.Set(shape);
    entity.Set(Components::Transform{});
    Components::WorldTransform transform{};
    transform.Basis[0] = glm::vec3(cosine, 0.0f, -sine);
    transform.Basis[2] = glm::vec3(sine, 0.0f, cosine);
    transform.Position = Mathematics::WorldPosition{x, y, z};
    transform.Version = 1u; // resolved, as TransformHierarchySystem leaves it
    entity.Set(transform);
    return entity;
}

// A seeded sequence of floats from a linear congruential generator, the same on every platform. The
// standard distributions over std::mt19937 are not: their results differ between standard libraries.
class SeededSequence
{
  public:
    explicit SeededSequence(uint32 seed)
        : m_State(seed)
    {
    }

    // The next value in [low, high).
    float32 Next(float32 low, float32 high)
    {
        m_State = m_State * kMultiplier + kIncrement;
        return low + (high - low) * static_cast<float32>(m_State >> 8) / kTopBitsRange;
    }

  private:
    static constexpr uint32 kMultiplier = 1664525u;
    static constexpr uint32 kIncrement = 1013904223u;
    // 2^24: a value is the state's top 24 bits over their range.
    static constexpr float32 kTopBitsRange = 16777216.0f;

    uint32 m_State;
};

// A grid surface of 0.5 m cells over x, z in [-16, 16] on a floor box whose top is y = 0, under a
// seeded field of 1,000 static boxes (12,000 triangles, more than one chunk of the bake's triangle
// passes): turned, stacked and floating at random. Integer arithmetic and exact rotations only, so
// every platform builds the same triangles.
inline ECS::Entity CreateBoxFieldGridSurface(ECS::World& world, Pathfinding::GridType gridType)
{
    CreateTurnedBox(world, 0.0f, -0.5f, 0.0f, 16.0f, 0.5f, 16.0f, 1.0f, 0.0f);
    // Rotations whose cosine and sine are exact Pythagorean pairs.
    constexpr float32 kTurns[][2] = {{1.0f, 0.0f}, {0.6f, 0.8f}, {0.8f, 0.6f}, {0.28f, 0.96f}, {-0.6f, 0.8f}};
    SeededSequence random(4045u);
    for (uint32 i = 0; i < 1000; ++i)
    {
        const float32 x = random.Next(-15.0f, 15.0f), z = random.Next(-15.0f, 15.0f);
        const float32 halfX = random.Next(0.05f, 0.8f), halfY = random.Next(0.05f, 1.2f);
        const float32 halfZ = random.Next(0.05f, 0.8f);
        const float32 bottom = random.Next(-0.5f, 2.5f);
        const auto& turn = kTurns[static_cast<uint32>(random.Next(0.0f, 4.999f))];
        CreateTurnedBox(world, x, bottom + halfY, z, halfX, halfY, halfZ, turn[0], turn[1]);
    }
    auto entity = world.Create();
    Components::NavigationSurface surface{};
    surface.Mode = Components::NavigationSurfaceMode::Grid;
    surface.GridType = gridType;
    surface.FitBounds = false;
    surface.Center = {0.0f, 2.0f, 0.0f};
    surface.Extent = {16.0f, 8.0f, 16.0f};
    entity.Set(surface);
    world.ProcessCommands();
    return entity;
}

// A digest of a baked grid: every cell's blocked flag and its height to the millimetre, so a
// last-bit difference in a platform's floating point does not change it.
inline uint64 GridDigest(const Pathfinding::GridMap& grid)
{
    uint64 hash = Hashing::kFnv1a64OffsetBasis;
    for (uint32 z = 0; z < grid.GetSettings().Depth; ++z)
        for (uint32 x = 0; x < grid.GetSettings().Width; ++x)
        {
            hash = Hashing::Fnv1a64Value(hash, static_cast<uint8>(grid.IsCellBlocked(x, z)));
            hash = Hashing::Fnv1a64Value(hash, static_cast<int64>(std::llround(grid.GetCellHeight(x, z) * 1000.0f)));
        }
    return hash;
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
