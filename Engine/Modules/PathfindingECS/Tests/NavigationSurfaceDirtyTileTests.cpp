// A baked mesh NavigationSurface's dirty queue: a terrain edit, a carving obstacle and MarkDirty each
// rebake exactly the tiles they touch whose content changed, a few an update, every other tile
// keeping its bytes; agents whose path crosses a rebaked tile replan.
#include "NavigationTestSurfaces.h"

#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Components/NavigationObstacle.h"
#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationScriptApi.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"

#include "Components/Terrain/Terrain.h"
#include "ECS/ComponentRegistry.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/PathBuffer.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "TerrainECS/TerrainService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{

namespace TE = GameEngine::TerrainECS;
using Components::NavigationBakePhase;
using Components::NavigationSurface;

using TileBytes = std::map<std::pair<int32, int32>, std::pair<std::vector<uint8>, uint64>>;

class NavigationSurfaceDirtyTileTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TE::TerrainService::Initialize();
        NavigationService::Initialize();
        world = std::make_unique<ECS::World>();
        RegisterNavigationWorldHooks(*world);
    }

    void TearDown() override
    {
        world.reset();
        ECS::ComponentRegistry::Clear();
        NavigationService::Shutdown();
        TE::TerrainService::Shutdown();
    }

    // A mesh surface over the box of half size `extent` about the origin: tiles of 32 m from
    // (-extent, -extent).
    ECS::Entity CreateSurface(float32 extent)
    {
        auto entity = world->Create();
        NavigationSurface surface{};
        surface.FitBounds = false;
        surface.Extent = {extent, 20.0f, extent};
        entity.Set(surface);
        world->ProcessCommands();
        return entity;
    }

    // A flat 128 m terrain about the origin with a 1 m lattice.
    TE::TerrainData* CreateTerrain()
    {
        Terrain::TerrainConfig config{};
        config.HeightmapWidth = config.HeightmapHeight = 129;
        config.WorldSizeX = config.WorldSizeZ = 128.0f;
        config.HeightScale = 10.0f;
        config.LODLevels = 2;
        const auto handle = TE::TerrainService::Get().CreateTerrain(config);
        auto* data = TE::TerrainService::Get().GetTerrainData(handle);
        data->MarkFullDirty();
        auto entity = world->Create();
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
        world->ProcessCommands();
        return data;
    }

    // A static 1 m high box of half size halfX x halfZ about (x, z) on the ground: geometry the
    // surface does not watch, which a game reports with MarkDirty.
    void CreateCrate(float32 x, float32 z, float32 halfX, float32 halfZ, float32 halfY = 0.5f)
    {
        auto entity = world->Create();
        Components::PhysicsBody body{};
        body.motionType = Physics::MotionType::Static;
        entity.Set(body);
        entity.Set(Components::PhysicsCollider{});
        Components::BoxColliderShape box{};
        box.halfExtentsX = halfX;
        box.halfExtentsY = halfY;
        box.halfExtentsZ = halfZ;
        entity.Set(box);
        entity.Set(Components::Transform{});
        Components::WorldTransform placed{};
        placed.matrix[0] = placed.matrix[5] = placed.matrix[10] = placed.matrix[15] = 1.0f;
        placed.matrix[12] = x;
        placed.matrix[13] = halfY;
        placed.matrix[14] = z;
        placed.Version = 1u;
        entity.Set(placed);
        world->ProcessCommands();
    }

    // One update of the surface system this test keeps, as a frame runs it.
    void Update()
    {
        system.Update(*world, 1.0f / 60.0f);
        world->ProcessCommands();
    }

    void BakeAndSettle(ECS::Entity& surface)
    {
        for (int i = 0; i < 64; ++i)
        {
            Update();
            const NavigationSurface& state = *surface.Get<NavigationSurface>();
            if (state.BakePhase == NavigationBakePhase::Baked && state.TilesDirty == 0 && state.TilesBaking == 0)
                return;
        }
        FAIL() << Navigation::FormatBakeStatus(*surface.Get<NavigationSurface>());
    }

    Pathfinding::DetourNavMap& Mesh(ECS::Entity& surface)
    {
        return *ResolveNavigationSurfaceMesh(*world, *surface.Get<NavigationSurface>());
    }

    TileBytes Tiles(ECS::Entity& surface)
    {
        std::vector<Pathfinding::NavMeshTileData> tiles;
        Mesh(surface).CopyTiles(tiles);
        TileBytes out;
        for (const auto& tile : tiles)
            out[{tile.Key.X, tile.Key.Z}] = {tile.Data, tile.ContentHash};
        return out;
    }

    // The tiles whose bytes or hash differ between the two copies.
    static std::vector<std::pair<int32, int32>> Changed(const TileBytes& before, const TileBytes& after)
    {
        std::vector<std::pair<int32, int32>> changed;
        for (const auto& [key, tile] : after)
        {
            const auto old = before.find(key);
            if (old == before.end() || old->second != tile)
                changed.push_back(key);
        }
        return changed;
    }

    using Keys = std::vector<std::pair<int32, int32>>;

    std::unique_ptr<ECS::World> world;
    NavigationSurfaceSystem system;
};

} // namespace

// Raising the ground under (0, 0) changes tile (1, 1) and no other: only it rebakes.
TEST_F(NavigationSurfaceDirtyTileTest, ATerrainEditRebakesExactlyItsTiles)
{
    TE::TerrainData* terrain = CreateTerrain();
    auto surface = CreateSurface(48.0f);
    BakeAndSettle(surface);
    const TileBytes before = Tiles(surface);
    const uint32 builtBefore = surface.Get<NavigationSurface>()->TilesBuilt;

    // Samples 60 to 68 are world -4 to 4 m on the 1 m lattice from -64.
    for (uint32 z = 60; z <= 68; ++z)
        for (uint32 x = 60; x <= 68; ++x)
            terrain->Heightfield.SetSample(x, z, 0.1f);
    terrain->MarkRegionDirty(60, 60, 69, 69);
    Update();

    EXPECT_EQ(surface.Get<NavigationSurface>()->TilesBuilt - builtBefore, 1u);
    EXPECT_EQ(Changed(before, Tiles(surface)), (Keys{{1, 1}}));
}

// A heightfield marked dirty without a height changing, as a terrain re-provision or a tile's
// modifier bake does, queues the tiles under it; each one's hash matches the tile it holds, so none
// is rebuilt or replaced.
TEST_F(NavigationSurfaceDirtyTileTest, ADirtyTileWhoseContentDidNotChangeIsNotRebuilt)
{
    TE::TerrainData* terrain = CreateTerrain();
    auto surface = CreateSurface(48.0f);
    BakeAndSettle(surface);
    const NavigationSurface start = *surface.Get<NavigationSurface>();

    terrain->MarkFullDirty();
    Update(); // 8 of the 9 tiles, the cap
    Update();

    const NavigationSurface& state = *surface.Get<NavigationSurface>();
    EXPECT_EQ(state.TilesBuilt, start.TilesBuilt);
    EXPECT_EQ(state.BakeVersion, start.BakeVersion);
    EXPECT_EQ(state.TilesDirty, 0u);
}

// A carving obstacle added on a baked surface cuts its cylinder out in the update that sees it, and
// leaves every other tile as it was.
TEST_F(NavigationSurfaceDirtyTileTest, ACarvingObstacleCutsWithinOneApply)
{
    Testing::CreateGroundPlane(*world);
    auto surface = CreateSurface(48.0f);
    BakeAndSettle(surface);
    ASSERT_TRUE(Mesh(surface).IsPointNavigable(0.0f, 0.0f, 0.0f, 0.5f));
    const TileBytes before = Tiles(surface);

    auto obstacle = world->Create();
    obstacle.Set(Components::NavigationObstacle{1.5f, 2.0f, true});
    Components::WorldTransform placed{};
    placed.Version = 1u;
    obstacle.Set(placed);
    world->ProcessCommands();
    Update();

    EXPECT_FALSE(Mesh(surface).IsPointNavigable(0.0f, 0.0f, 0.0f, 0.5f));
    EXPECT_TRUE(Mesh(surface).IsPointNavigable(6.0f, 0.0f, 0.0f, 0.5f));
    EXPECT_EQ(Changed(before, Tiles(surface)), (Keys{{1, 1}}));
}

// MarkDirty over a box queues the tiles under it, widened by the border a tile's build reads; they
// rebake and the version counts each one put into the map.
TEST_F(NavigationSurfaceDirtyTileTest, MarkDirtyRebakesTheTilesUnderTheBox)
{
    Testing::CreateGroundPlane(*world);
    auto surface = CreateSurface(48.0f);
    BakeAndSettle(surface);
    const NavigationSurface start = *surface.Get<NavigationSurface>();

    // A crate placed over world X -40 to -10, Z -40 to -30: chart-local X 8 to 38 (tiles 0 and 1),
    // Z 8 to 18 (tile 0).
    CreateCrate(-25.0f, -35.0f, 15.0f, 5.0f);
    Navigation::MarkDirty(*world, surface.GetHandle(), {-40.0f, 0.0f, -40.0f}, {-10.0f, 5.0f, -30.0f});
    Update();

    const NavigationSurface& state = *surface.Get<NavigationSurface>();
    EXPECT_EQ(state.TilesBuilt - start.TilesBuilt, 2u);
    EXPECT_EQ(state.BakeVersion - start.BakeVersion, 2u);
    EXPECT_EQ(state.TilesDirty, 0u);
}

// At most kTileRebakesPerUpdate dirty tiles start rebaking per update; the rest wait, counted.
TEST_F(NavigationSurfaceDirtyTileTest, TheRebakeCapHolds)
{
    Testing::CreateGroundPlane(*world);
    auto surface = CreateSurface(80.0f); // 5 x 5 tiles
    BakeAndSettle(surface);
    const uint32 builtBefore = surface.Get<NavigationSurface>()->TilesBuilt;

    CreateCrate(0.0f, 0.0f, 80.0f, 80.0f);
    Navigation::MarkDirty(*world, surface.GetHandle(), {-80.0f, 0.0f, -80.0f}, {80.0f, 0.0f, 80.0f});
    Update();

    const NavigationSurface& state = *surface.Get<NavigationSurface>();
    EXPECT_EQ(state.TilesBuilt - builtBefore, NavigationSurfaceSystem::kTileRebakesPerUpdate);
    EXPECT_EQ(state.TilesDirty, 25u - NavigationSurfaceSystem::kTileRebakesPerUpdate);
    EXPECT_EQ(Navigation::FormatBakeStatus(state).substr(Navigation::FormatBakeStatus(state).rfind(". ")),
              ". Rebaking: 17 tiles");
    for (int i = 0; i < 3; ++i)
        Update();
    EXPECT_EQ(surface.Get<NavigationSurface>()->TilesBuilt - builtBefore, 25u);
    EXPECT_EQ(surface.Get<NavigationSurface>()->TilesDirty, 0u);
}

// An agent whose path crosses a rebaked tile is asked to replan and keeps its path meanwhile; one
// whose path does not is left alone.
TEST_F(NavigationSurfaceDirtyTileTest, AnAgentWhosePathCrossesARebakedTileReplans)
{
    Testing::CreateGroundPlane(*world);
    auto surface = CreateSurface(48.0f);
    BakeAndSettle(surface);
    const Pathfinding::NavMapHandle map = Testing::SurfaceMap(*world, surface);
    auto& paths = NavigationService::Get().GetPathBuffer();

    const auto createAgent = [&](std::vector<Pathfinding::PathPoint> points) {
        auto entity = world->Create();
        Components::NavigationAgent agent{};
        entity.Set(agent);
        Components::NavigationAgentState state{};
        state.NavMapIndex = map.Index;
        state.NavMapGeneration = map.Generation;
        state.HasDestination = true;
        state.PathDirty = false;
        const Pathfinding::PathHandle path = paths.AllocatePath(points.data(), static_cast<uint32>(points.size()));
        state.PathIndex = path.Index;
        state.PathGeneration = path.Generation;
        entity.Set(state);
        world->ProcessCommands();
        return entity;
    };
    // Through tile (0, 0) (world -48 to -16) and on into tile (1, 0); the other stays in tile (2, 2).
    auto crossing = createAgent({{-40.0f, 0.0f, -40.0f}, {0.0f, 0.0f, -40.0f}});
    auto away = createAgent({{25.0f, 0.0f, 25.0f}, {40.0f, 0.0f, 40.0f}});

    CreateCrate(-35.0f, -35.0f, 5.0f, 5.0f);
    Navigation::MarkDirty(*world, surface.GetHandle(), {-40.0f, 0.0f, -40.0f}, {-30.0f, 0.0f, -30.0f});
    Update();

    const auto& crossingState = *crossing.Get<Components::NavigationAgentState>();
    EXPECT_TRUE(crossingState.ReplanRequested);
    EXPECT_FALSE(crossingState.PathDirty) << "the agent keeps walking its path until the new one lands";
    EXPECT_FALSE(away.Get<Components::NavigationAgentState>()->ReplanRequested);
}

// A box marked while the surface is still baking is kept until the bake completes: the tiles the
// bake built before the crate was placed rebake with it.
TEST_F(NavigationSurfaceDirtyTileTest, MarkDirtyDuringABakeIsKept)
{
    Testing::CreateGroundPlane(*world);
    auto surface = CreateSurface(240.0f); // 15 x 15 tiles: 4 updates at 64 a frame
    Update();
    ASSERT_EQ(surface.Get<NavigationSurface>()->BakePhase, NavigationBakePhase::Baking);

    // A 3 m crate on tile (2, 0), which the first update already built.
    CreateCrate(-160.0f, -224.0f, 4.0f, 4.0f, 1.5f);
    EXPECT_TRUE(Navigation::MarkDirty(*world, surface.GetHandle(), {-164.0f, 0.0f, -228.0f}, {-156.0f, 3.0f, -220.0f}));
    BakeAndSettle(surface);

    EXPECT_TRUE(Mesh(surface).IsPointNavigable(-160.0f, 3.0f, -224.0f, 0.5f)) << "the crate's top is not in the mesh";
}

// A grid surface has no tiles to mark: MarkDirty says so.
TEST_F(NavigationSurfaceDirtyTileTest, MarkDirtyRefusesAGridSurface)
{
    auto grid = Testing::CreateFlatGridSurface(*world, 8, 8, 1.0f);
    EXPECT_FALSE(Navigation::MarkDirty(*world, grid.GetHandle(), {0.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 4.0f}));
}
