// NavigationSurface's baked data: a mesh surface saved as a .navsurface file and opened again
// loads its tiles instead of baking them, and the tiles whose ground changed since are stale until
// their rebake.
#include "NavigationTestSurfaces.h"

#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "ECS/ComponentRegistry.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/NavSurfaceFile.h"
#include "PhysicsECS/Components/BoxColliderShape.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{

using Components::NavigationBakePhase;
using Components::NavigationSurface;

// A surface over the 96 m box about the origin: 3 x 3 tiles of 32 m from (-48, -48).
constexpr float32 kSurfaceHalfExtent = 48.0f;

class NavigationSurfaceBakedDataTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Engine::Initialize moves the CWD to WorkspaceDirectory, and Windows refuses to remove a
        // directory containing the CWD.
        originalCwd = std::filesystem::current_path();
        testDir = std::filesystem::temp_directory_path() /
                  ("ge_nav_baked_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(testDir);

        engine = std::make_unique<EngineCore>();
        ApplicationConfig config;
        config.WorkspaceDirectory = testDir.string();
        config.AssetDirectory = testDir.string();
        config.AssetDatabaseFile = "AssetDatabase.assetdb";
        config.AssetDatabaseCacheDirectory = ".Cache/AssetDatabase";
        ASSERT_TRUE(engine->Initialize(config));
        assetRoot = engine->GetAssetManager().GetAssetRoot();
        ASSERT_FALSE(assetRoot.empty());
        NavigationService::Initialize();
    }

    void TearDown() override
    {
        worlds.clear();
        ECS::ComponentRegistry::Clear();
        NavigationService::Shutdown();
        if (engine)
        {
            engine->Shutdown();
            engine.reset();
        }
        std::error_code ec;
        std::filesystem::current_path(originalCwd, ec);
        std::filesystem::remove_all(testDir, ec);
    }

    // A scene: level ground, a static 4 m crate centred at (crateX, crateZ) and a mesh surface,
    // whose BakedData is `bakedData` when that is not null.
    std::pair<ECS::World*, ECS::Entity> OpenScene(float32 crateX, float32 crateZ, const GUID& bakedData = GUID{},
                                                  float32 surfaceHalfExtent = kSurfaceHalfExtent,
                                                  float32 groundHalfExtent = 1000.0f)
    {
        ECS::World& world = *worlds.emplace_back(std::make_unique<ECS::World>());
        RegisterNavigationWorldHooks(world);
        Testing::CreateGroundPlane(world, groundHalfExtent);

        auto crate = world.Create();
        Components::PhysicsBody body{};
        body.motionType = Physics::MotionType::Static;
        crate.Set(body);
        crate.Set(Components::PhysicsCollider{});
        Components::BoxColliderShape box{};
        box.halfExtentsX = box.halfExtentsY = box.halfExtentsZ = 2.0f;
        crate.Set(box);
        crate.Set(Components::Transform{});
        Components::WorldTransform placed{};
        placed.matrix[12] = crateX;
        placed.matrix[13] = 2.0f;
        placed.matrix[14] = crateZ;
        placed.Version = 1u;
        crate.Set(placed);

        auto entity = world.Create();
        NavigationSurface surface{};
        surface.FitBounds = false;
        surface.Extent = {surfaceHalfExtent, 50.0f, surfaceHalfExtent};
        if (!bakedData.IsNull())
            surface.BakedData.Set(bakedData);
        entity.Set(surface);
        world.ProcessCommands();
        return {&world, entity};
    }

    // The surface's state when its bake first completed, before any rebake.
    struct FirstBaked
    {
        uint32 StaleTiles = 0;
        int32 NamedX = 0;
        int32 NamedZ = 0;
    };

    // Updates a NavigationSurfaceSystem until the surface's bake and its rebakes settle: tile jobs
    // run on the engine's workers, and the baked data loads through the asset manager.
    static FirstBaked Settle(ECS::World& world, ECS::Entity& entity)
    {
        NavigationSurfaceSystem system;
        FirstBaked first;
        bool baked = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline)
        {
            system.Update(world, 1.0f / 60.0f);
            world.ProcessCommands();
            const NavigationSurface& state = *entity.Get<NavigationSurface>();
            if (state.BakePhase == NavigationBakePhase::Baking || state.BakePhase == NavigationBakePhase::Waiting)
                continue;
            if (!baked)
            {
                baked = true;
                first = {state.StaleTiles, state.BlockingTileX, state.BlockingTileZ};
            }
            if (state.BakePhase != NavigationBakePhase::Baked || state.TilesDirty + state.TilesBaking == 0)
                return first;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return first;
    }

    // Writes the surface's baked tiles beside the scene and returns the file's asset; `damage`, when
    // given, changes the file's data before it is written.
    GUID SaveBakedData(ECS::World& world, ECS::Entity& entity, std::vector<uint8>& outBytes,
                       void (*damage)(Pathfinding::NavSurfaceData&) = nullptr)
    {
        EXPECT_TRUE(Navigation::WriteBakedSurface(world, entity.GetHandle(), outBytes));
        if (damage)
        {
            Pathfinding::NavSurfaceData data;
            std::string error;
            EXPECT_TRUE(Pathfinding::ReadNavSurface(outBytes, data, error)) << error;
            damage(data);
            EXPECT_TRUE(Pathfinding::WriteNavSurface(data, outBytes));
        }
        const std::filesystem::path file = assetRoot / "Level.navsurface";
        std::ofstream(file, std::ios::binary)
            .write(reinterpret_cast<const char*>(outBytes.data()), static_cast<std::streamsize>(outBytes.size()));
        const auto registered = engine->GetAssetManager().GetRegistry().RegisterAssetByPath(file);
        EXPECT_TRUE(registered.IsOk());
        return registered.IsOk() ? registered.Value() : GUID{};
    }

    static uint32 TileCount(ECS::World& world, ECS::Entity& entity)
    {
        const auto* mesh = ResolveNavigationSurfaceMesh(world, *entity.Get<NavigationSurface>());
        return mesh ? mesh->GetTileCount() : 0u;
    }

    std::filesystem::path testDir;
    std::filesystem::path originalCwd;
    std::filesystem::path assetRoot;
    std::unique_ptr<EngineCore> engine;
    std::vector<std::unique_ptr<ECS::World>> worlds;
};

} // namespace

// The scene opened again in a fresh world loads every tile from the file: no tile is built, none is
// stale, and the surface would save the same bytes. Red when anything but content (a counter, a
// handle) feeds a tile's hash.
TEST_F(NavigationSurfaceBakedDataTest, AReopenedSceneLoadsItsTilesWithoutABake)
{
    auto [baked, bakedSurface] = OpenScene(0.0f, 0.0f);
    Settle(*baked, bakedSurface);
    ASSERT_EQ(bakedSurface.Get<NavigationSurface>()->BakePhase, NavigationBakePhase::Baked);
    std::vector<uint8> saved;
    const GUID file = SaveBakedData(*baked, bakedSurface, saved);

    auto [reopened, surface] = OpenScene(0.0f, 0.0f, file);
    Settle(*reopened, surface);

    const NavigationSurface& state = *surface.Get<NavigationSurface>();
    ASSERT_EQ(state.BakePhase, NavigationBakePhase::Baked) << Navigation::FormatBakeStatus(state);
    EXPECT_EQ(state.TilesBuilt, 0u);
    EXPECT_EQ(state.StaleTiles, 0u);
    EXPECT_EQ(TileCount(*reopened, surface), TileCount(*baked, bakedSurface));
    std::vector<uint8> resaved;
    ASSERT_TRUE(Navigation::WriteBakedSurface(*reopened, surface.GetHandle(), resaved));
    EXPECT_EQ(resaved, saved);
}

// The crate moved from tile (1, 1) to tile (2, 1) between the save and the reopen: exactly those
// two tiles load stale, the notice names one, and they and nothing else rebake.
TEST_F(NavigationSurfaceBakedDataTest, AMovedCrateMakesExactlyItsTilesStale)
{
    auto [baked, bakedSurface] = OpenScene(8.0f, 8.0f);
    Settle(*baked, bakedSurface);
    ASSERT_EQ(bakedSurface.Get<NavigationSurface>()->BakePhase, NavigationBakePhase::Baked);
    std::vector<uint8> saved;
    const GUID file = SaveBakedData(*baked, bakedSurface, saved);

    auto [reopened, surface] = OpenScene(40.0f, 8.0f, file);
    const FirstBaked first = Settle(*reopened, surface);

    EXPECT_EQ(first.StaleTiles, 2u);
    const std::set<std::pair<int32, int32>> expected = {{1, 1}, {2, 1}};
    EXPECT_TRUE(expected.contains({first.NamedX, first.NamedZ})) << first.NamedX << ", " << first.NamedZ;
    const NavigationSurface& state = *surface.Get<NavigationSurface>();
    ASSERT_EQ(state.BakePhase, NavigationBakePhase::Baked) << Navigation::FormatBakeStatus(state);
    EXPECT_EQ(state.TilesBuilt, 2u);
    EXPECT_EQ(state.StaleTiles, 0u);
}

// A tile with no walkable area (here, the ring of tiles past the ground's edge) is saved with its
// hash like any other, so the reopened scene builds none.
TEST_F(NavigationSurfaceBakedDataTest, EmptyTilesAreSavedAndReopenWithoutABake)
{
    // 5 x 5 tiles of 32 m from -80; the ground covers -40 to 40, so the outer ring holds nothing.
    auto [baked, bakedSurface] = OpenScene(0.0f, 0.0f, GUID{}, 80.0f, 40.0f);
    Settle(*baked, bakedSurface);
    ASSERT_EQ(bakedSurface.Get<NavigationSurface>()->BakePhase, NavigationBakePhase::Baked);
    ASSERT_LT(TileCount(*baked, bakedSurface), 25u);
    std::vector<uint8> saved;
    const GUID file = SaveBakedData(*baked, bakedSurface, saved);

    auto [reopened, surface] = OpenScene(0.0f, 0.0f, file, 80.0f, 40.0f);
    Settle(*reopened, surface);

    const NavigationSurface& state = *surface.Get<NavigationSurface>();
    ASSERT_EQ(state.BakePhase, NavigationBakePhase::Baked) << Navigation::FormatBakeStatus(state);
    EXPECT_EQ(state.TilesBuilt, 0u);
    EXPECT_EQ(state.StaleTiles, 0u);
}

// A saved tile whose bytes are damaged (cut to 100 bytes, its header still declaring the rest) is
// refused by the mesh, counted as stale and rebuilt; every other tile loads.
TEST_F(NavigationSurfaceBakedDataTest, ADamagedSavedTileRebakesAndTheRestLoad)
{
    auto [baked, bakedSurface] = OpenScene(0.0f, 0.0f);
    Settle(*baked, bakedSurface);
    ASSERT_EQ(bakedSurface.Get<NavigationSurface>()->BakePhase, NavigationBakePhase::Baked);
    std::vector<uint8> saved;
    const GUID file = SaveBakedData(*baked, bakedSurface, saved, [](Pathfinding::NavSurfaceData& data) {
        for (Pathfinding::NavSurfaceTile& tile : data.Profiles[0].Charts[0].Tiles)
            if (tile.Data.size() > 100)
            {
                tile.Data.resize(100);
                return;
            }
    });

    auto [reopened, surface] = OpenScene(0.0f, 0.0f, file);
    const FirstBaked first = Settle(*reopened, surface);

    EXPECT_EQ(first.StaleTiles, 1u);
    const NavigationSurface& state = *surface.Get<NavigationSurface>();
    ASSERT_EQ(state.BakePhase, NavigationBakePhase::Baked) << Navigation::FormatBakeStatus(state);
    EXPECT_EQ(state.TilesBuilt, 1u);
    EXPECT_EQ(state.StaleTiles, 0u);
    EXPECT_EQ(TileCount(*reopened, surface), TileCount(*baked, bakedSurface));
}
