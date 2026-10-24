#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Components/NavigationObstacle.h"
#include "Components/Navigation/NavigationSurface.h"
#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationScriptApi.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"
#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "NavigationTestSurfaces.h"
#include "Assets/AssetManager.h"
#include "Assets/FileWatchingService.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/ECSTemplates.h"
#include "ECS/ComponentRegistry.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneSchemaRegistry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{

constexpr float32 kDeltaTime = 1.0f / 60.0f;
constexpr uint32 kGridWidth = 8;
constexpr uint32 kGridDepth = 8;
constexpr float32 kCellSize = 1.0f;

// These tests run with no PhysicsWorldService, which is also the editor's edit-mode state: the
// surface bake reads no physics world.
class NavigationSurfaceGatingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        NavigationService::Initialize();
        world = std::make_unique<ECS::World>();
        RegisterNavigationWorldHooks(*world);
        Testing::CreateGroundPlane(*world);
    }

    void TearDown() override
    {
        world.reset();
        ECS::ComponentRegistry::Clear();
        NavigationService::Shutdown();
    }

    // A grid surface over the ground: an 8 x 8 m box around the entity's position.
    static Components::NavigationSurface MakeGridSurface()
    {
        Components::NavigationSurface surface{};
        surface.Mode = Components::NavigationSurfaceMode::Grid;
        surface.CellSize = kCellSize;
        surface.FitBounds = false;
        surface.Center = {0.0f, 0.0f, 0.0f};
        surface.Extent = {0.5f * kGridWidth * kCellSize, 1.0f, 0.5f * kGridDepth * kCellSize};
        return surface;
    }

    static Components::WorldTransform MakeWorldTransform(float32 x, float32 y, float32 z)
    {
        Components::WorldTransform wt{};
        wt.matrix[12] = x;
        wt.matrix[13] = y;
        wt.matrix[14] = z;
        wt.Version = 1u; // resolved, as TransformHierarchySystem leaves it
        return wt;
    }

    void RunBuild()
    {
        NavigationSurfaceSystem system;
        system.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    Pathfinding::GridMap* GridOf(ECS::Entity& entity)
    {
        const auto* surface = entity.Get<Components::NavigationSurface>();
        return surface ? ResolveNavigationSurfaceGrid(*world, *surface) : nullptr;
    }

    std::unique_ptr<ECS::World> world;
};

} // namespace

// A scene-loaded surface entity carries Transform and an unresolved WorldTransform (Version 0)
// until TransformHierarchySystem resolves it. Baking before then would bake around the origin and
// keep that bake until the next request, so the bake waits.
TEST_F(NavigationSurfaceGatingTest, SurfaceWithUnresolvedWorldTransformDefersItsBake)
{
    auto entity = world->Create();
    entity.Set(MakeGridSurface());
    entity.Set(Components::Transform{});
    world->ProcessCommands();

    RunBuild();

    const auto* surface = entity.Get<Components::NavigationSurface>();
    ASSERT_NE(surface, nullptr);
    EXPECT_FALSE(ResolveNavigationSurfaceMap(*world, *surface).IsValid())
        << "surface baked before its WorldTransform was resolved; it would sit at the origin";
}

// ...and once WorldTransform is resolved, the surface bakes around the placed position rather than
// the world origin.
TEST_F(NavigationSurfaceGatingTest, SurfaceBakesAroundItsPlacedPositionOnceWorldTransformIsResolved)
{
    constexpr float32 kPlacedX = 25.0f;
    constexpr float32 kPlacedY = 3.0f;
    constexpr float32 kPlacedZ = -40.0f;

    auto entity = world->Create();
    entity.Set(MakeGridSurface());
    entity.Set(Components::Transform{});
    world->ProcessCommands();

    RunBuild();
    ASSERT_EQ(GridOf(entity), nullptr);

    entity.Set(MakeWorldTransform(kPlacedX, kPlacedY, kPlacedZ));
    world->ProcessCommands();

    RunBuild();

    auto* gridMap = GridOf(entity);
    ASSERT_NE(gridMap, nullptr);
    const auto& settings = gridMap->GetSettings();
    EXPECT_FLOAT_EQ(settings.OriginX, kPlacedX - 0.5f * kGridWidth * kCellSize);
    EXPECT_FLOAT_EQ(settings.OriginY, kPlacedY - 1.0f);
    EXPECT_FLOAT_EQ(settings.OriginZ, kPlacedZ - 0.5f * kGridDepth * kCellSize);
}

// A surface on an entity with no Transform at all has no placed position, so the world origin is
// correct for it and it must not be deferred forever.
TEST_F(NavigationSurfaceGatingTest, SurfaceWithoutAnyTransformBakesAroundTheWorldOrigin)
{
    auto entity = world->Create();
    entity.Set(MakeGridSurface());
    world->ProcessCommands();

    RunBuild();

    auto* gridMap = GridOf(entity);
    ASSERT_NE(gridMap, nullptr);
    EXPECT_FLOAT_EQ(gridMap->GetSettings().OriginX, -0.5f * kGridWidth * kCellSize);
    EXPECT_FLOAT_EQ(gridMap->GetSettings().OriginZ, -0.5f * kGridDepth * kCellSize);
}

// Loading a scene re-applies the component's properties to an entity that may already own a map.
// The reflected schema keeps the runtime binding, so the surface keeps its map (no orphan, no
// second allocation) and a re-bake reuses the same handle.
TEST_F(NavigationSurfaceGatingTest, SceneReloadKeepsTheSurfacesMap)
{
    auto entity = world->Create();
    entity.Set(MakeGridSurface());
    world->ProcessCommands();
    RunBuild();

    const auto first = ResolveNavigationSurfaceMap(*world, *entity.Get<Components::NavigationSurface>());
    ASSERT_TRUE(first.IsValid());

    const auto* schema = Scene::SceneSchemaRegistry::Find("NavigationSurface");
    ASSERT_NE(schema, nullptr);
    Scene::SceneLoadContext ctx{};
    std::string error;
    ASSERT_TRUE(schema->ApplyProperty(*world, entity.GetHandle(), ctx, "cellsize", "0.5", &error)) << error;
    world->ProcessCommands();

    EXPECT_EQ(ResolveNavigationSurfaceMap(*world, *entity.Get<Components::NavigationSurface>()), first);
    ASSERT_TRUE(Navigation::RequestBake(*world, entity.GetHandle()));
    RunBuild();
    EXPECT_EQ(ResolveNavigationSurfaceMap(*world, *entity.Get<Components::NavigationSurface>()), first)
        << "a re-bake allocated a new map; agents bound to the old handle would lose their surface";
    EXPECT_FLOAT_EQ(GridOf(entity)->GetSettings().CellSize, 0.5f);
}

// An authored obstacle radius is unvalidated and drives an O(cellRadius^2)
// stamp per obstacle per grid every frame. Radius=1000 at CellSize=0.1 is 4e8
// iterations, and cellRadius*cellRadius overflows uint32 past ~65535. The
// clamp bounds that loop.
//
// A centred obstacle cannot tell a correct clamp from one that loses coverage:
// the farthest cell from the centre of an 8x8 grid is only 32 cells-squared
// away, inside any clamp at or above 6. CornerObstacleWithHugeRadiusStillCovers
// is the probe that discriminates; keep both.
TEST_F(NavigationSurfaceGatingTest, HugeObstacleRadiusIsClampedToGridExtent)
{
    auto gridEntity = Testing::CreateFlatGridSurface(*world, kGridWidth, kGridDepth, kCellSize);
    ASSERT_NE(GridOf(gridEntity), nullptr);

    auto obstacleEntity = world->Create();
    Components::NavigationObstacle obstacle{};
    obstacle.Radius = 1000.0f;
    obstacleEntity.Set(obstacle);
    obstacleEntity.Set(MakeWorldTransform(4.0f, 0.0f, 4.0f));
    world->ProcessCommands();

    // Completing at all is the assertion: unclamped this is ~4e8 iterations per
    // frame at CellSize=1, and far worse for a finer grid.
    RunBuild();

    auto* gridMap = GridOf(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    // The obstacle covers the whole grid, so every cell is dynamically blocked.
    EXPECT_TRUE(gridMap->IsCellDynamicBlocked(0, 0));
    EXPECT_TRUE(gridMap->IsCellDynamicBlocked(kGridWidth - 1, kGridDepth - 1));
}

// The clamp bounds the loop; it must not shrink the footprint. An obstacle whose
// radius spans the grid blocks every cell of it wherever it sits, so the corner
// case is the real test: from cell (0,0) the opposite corner is
// (W-1)^2 + (D-1)^2 = 98 cells-squared away, which exceeds max(W,D)^2 = 64.
// Clamping the radius to max(W,D) therefore drops the far corner, while any
// clamp at or above the grid diagonal keeps it.
TEST_F(NavigationSurfaceGatingTest, CornerObstacleWithHugeRadiusStillCoversFarCorner)
{
    auto gridEntity = Testing::CreateFlatGridSurface(*world, kGridWidth, kGridDepth, kCellSize);
    ASSERT_NE(GridOf(gridEntity), nullptr);

    auto obstacleEntity = world->Create();
    Components::NavigationObstacle obstacle{};
    obstacle.Radius = 1000.0f;
    obstacleEntity.Set(obstacle);
    // Cell (0,0): WorldToCell floors, and the grid's minimum corner is (0,0,0).
    obstacleEntity.Set(MakeWorldTransform(0.5f, 0.0f, 0.5f));
    world->ProcessCommands();

    RunBuild();

    auto* gridMap = GridOf(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    EXPECT_TRUE(gridMap->IsCellDynamicBlocked(0, 0));
    EXPECT_TRUE(gridMap->IsCellDynamicBlocked(kGridWidth - 1, kGridDepth - 1))
        << "clamp shrank the obstacle footprint: the far corner is inside a "
           "radius that spans the grid, but outside the clamped radius";
}

// Radius <= 0 blocks nothing at all, including the obstacle's own cell. This is
// a deliberate change from taking ceil() of a non-positive radius: that path
// blocked the centre cell for Radius=0, and for a NaN radius it ran
// static_cast<uint32>(NaN), which is undefined. Rejecting the whole non-positive
// domain up front is both defined and the more defensible reading of "an
// obstacle with no radius".
TEST_F(NavigationSurfaceGatingTest, NonPositiveObstacleRadiusBlocksNothing)
{
    auto gridEntity = Testing::CreateFlatGridSurface(*world, kGridWidth, kGridDepth, kCellSize);
    ASSERT_NE(GridOf(gridEntity), nullptr);

    auto obstacleEntity = world->Create();
    Components::NavigationObstacle obstacle{};
    obstacle.Radius = 0.0f;
    obstacleEntity.Set(obstacle);
    obstacleEntity.Set(MakeWorldTransform(4.0f, 0.0f, 4.0f));
    world->ProcessCommands();

    RunBuild();

    auto* gridMap = GridOf(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    EXPECT_FALSE(gridMap->IsCellDynamicBlocked(4, 4))
        << "a zero-radius obstacle blocked its own cell";
}

// The .navgrid watch covers the project asset root. With no asset
// manager there is no root, and a pattern rooted at an empty directory matches
// no file — so subscribing one buys a permanently dead callback and a watcher
// that logs "Directory does not exist" every time it is started. The system
// must subscribe to nothing instead.
//
// The system is held alive across the sampling: its subscriptions are RAII, so
// reading the stats after it is destroyed would pass no matter what it did.
TEST_F(NavigationSurfaceGatingTest, NoNavigationAssetWatchIsCreatedWithoutAnAssetRoot)
{
    namespace fs = std::filesystem;

    FileWatchingService& watchService = FileWatchingService::GetInstance();
    ASSERT_TRUE(watchService.StartWatching());
    ASSERT_TRUE(watchService.IsWatching())
        << "the subscribe path under test only runs while the service is armed";

    const auto before = watchService.GetStats();

    NavigationSurfaceSystem system;
    system.Update(*world, kDeltaTime);
    world->ProcessCommands();

    const auto afterBuild = watchService.GetStats();
    EXPECT_EQ(afterBuild.TotalSubscriptions, before.TotalSubscriptions)
        << "NavigationSurfaceSystem subscribed a file watch with no asset root";
    EXPECT_EQ(afterBuild.TotalDirectories, before.TotalDirectories)
        << "an empty watch directory created a watcher that can never start";

    // Positive control: these counters do move for a real subscription, so the
    // equalities above report a skipped subscribe and not a dead instrument.
    const fs::path probeDir = fs::temp_directory_path() / "ge_nav_watch_probe";
    std::error_code ec;
    fs::create_directories(probeDir, ec);
    {
        auto probe = watchService.Subscribe(FilePattern(probeDir, ".*", {".navgrid"}, true),
                                            [](const FileChangeEvent&) {});
        const auto withProbe = watchService.GetStats();
        EXPECT_EQ(withProbe.TotalSubscriptions, before.TotalSubscriptions + 1);
    }
    fs::remove_all(probeDir, ec);

    watchService.StopWatching();
}

// The path a real scene open actually takes. The editor does not mutate the
// surviving component in place; it tears the old entities down and builds new
// ones, so the release has to hang off component teardown rather than off the
// schema's property apply. Without the OnRemove hook the map is orphaned and
// each scene open consumes another NavigationWorld slot.
TEST_F(NavigationSurfaceGatingTest, DestroyingSurfaceEntityReleasesItsNavMap)
{
    auto first = world->Create();
    first.Set(MakeGridSurface());
    world->ProcessCommands();
    RunBuild();

    const auto firstMap = ResolveNavigationSurfaceMap(*world, *first.Get<Components::NavigationSurface>());
    ASSERT_TRUE(firstMap.IsValid());

    world->DestroyEntity(first.GetHandle());
    world->ProcessCommands();

    auto second = world->Create();
    second.Set(MakeGridSurface());
    world->ProcessCommands();
    RunBuild();

    const auto secondMap = ResolveNavigationSurfaceMap(*world, *second.Get<Components::NavigationSurface>());
    ASSERT_TRUE(secondMap.IsValid());
    EXPECT_EQ(secondMap.Index, firstMap.Index)
        << "destroyed surface's map was not released; every scene open leaks one map per surface";
}

namespace
{

// Fixture with a real mounted asset root, which is what NavigationSurfaceSystem
// scopes its .navgrid watch to. Constructing an EngineCore binds the
// EngineCore::GetInstance() singleton the system reads, so the watch resolves
// to this test's temp directory.
class NavigationAssetWatchTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Engine::Initialize moves the CWD to WorkspaceDirectory, and Windows
        // refuses to remove a directory containing the CWD.
        originalCwd = std::filesystem::current_path();

        testDir = std::filesystem::temp_directory_path() /
                  ("ge_nav_watch_" + std::to_string(
                       std::chrono::steady_clock::now().time_since_epoch().count()));
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

        // The system only subscribes while the service is armed.
        ASSERT_TRUE(FileWatchingService::GetInstance().StartWatching());

        NavigationService::Initialize();
        world = std::make_unique<ECS::World>();
        RegisterNavigationWorldHooks(*world);
        Testing::CreateGroundPlane(*world);
    }

    void TearDown() override
    {
        FileWatchingService::GetInstance().StopWatching();
        world.reset();
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

    // Rewrites the file so a Modified event is produced even if the create was
    // already delivered and debounced.
    void TouchNavGrid(const std::filesystem::path& path, int revision) const
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "navgrid probe revision " << revision;
    }

    std::filesystem::path testDir;
    std::filesystem::path originalCwd;
    std::filesystem::path assetRoot;
    std::unique_ptr<EngineCore> engine;
    std::unique_ptr<ECS::World> world;
};

} // namespace

// The watch has to be rooted at the project asset root and filtered to .navgrid, which only an
// end-to-end event proves: a grid surface with authored cells bakes again when a .navgrid under
// that root is edited.
//
// This is the assertion that fails if the system regresses to an empty watch directory:
// FileWatchingService refuses that pattern, so no event ever arrives and the bake never repeats.
TEST_F(NavigationAssetWatchTest, EditingANavGridUnderTheAssetRootRebakesGridSurfaces)
{
    auto entity = world->Create();
    Components::NavigationSurface surface{};
    surface.Mode = Components::NavigationSurfaceMode::Grid;
    surface.CellSize = kCellSize;
    surface.FitBounds = false;
    surface.Extent = {4.0f, 1.0f, 4.0f};
    GUID::Data authoredBytes{};
    authoredBytes.fill(0x42);
    const GUID authored(authoredBytes);
    surface.AuthoredCells.Set(authored); // only grids with authored cells re-bake on an edit
    entity.Set(surface);
    world->ProcessCommands();

    NavigationSurfaceSystem system;
    system.Update(*world, kDeltaTime);
    world->ProcessCommands();
    const uint64 firstVersion = entity.Get<Components::NavigationSurface>()->BakeVersion;
    ASSERT_GT(firstVersion, 0u);

    const std::filesystem::path probe = assetRoot / "WatchProbe.navgrid";
    bool rebaked = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (int revision = 0; std::chrono::steady_clock::now() < deadline; ++revision)
    {
        TouchNavGrid(probe, revision);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        system.Update(*world, kDeltaTime);
        world->ProcessCommands();

        if (entity.Get<Components::NavigationSurface>()->BakeVersion > firstVersion)
        {
            rebaked = true;
            break;
        }
    }

    EXPECT_TRUE(rebaked) << "editing a .navgrid under the asset root did not reach the grid surface; the "
                            ".navgrid watch is not subscribed on the project asset root";
}

// A model a mesh renderer shows that has not loaded yet holds the bake: the bake requests the load,
// says what it waits for, and reads the model once it lands.
TEST_F(NavigationAssetWatchTest, BakeWaitsForAModelStillLoadingAndThenReadsIt)
{
    // No ground plane: the model is the only thing the surface can bake.
    world = std::make_unique<ECS::World>();
    RegisterNavigationWorldHooks(*world);

    const std::filesystem::path floorPath = assetRoot / "NavigationFloor.obj";
    {
        std::ofstream out(floorPath, std::ios::binary);
        out << "v -4 0 -4\nv 4 0 -4\nv 4 0 4\nv -4 0 4\nf 1 3 2\nf 1 4 3\n";
    }
    AssetManager& assets = engine->GetAssetManager();
    const auto registered = assets.GetRegistry().RegisterAssetByPath(floorPath);
    ASSERT_TRUE(registered.IsOk());
    const GUID model = registered.Value();
    ASSERT_FALSE(assets.IsAssetLoaded(model));

    auto renderer = world->Create();
    Components::MeshRenderer meshRenderer{};
    meshRenderer.modelAssetGuid.Set(model);
    renderer.Set(meshRenderer);
    renderer.Set(Components::WorldTransform{});
    auto entity = world->Create();
    Components::NavigationSurface surface{};
    surface.Mode = Components::NavigationSurfaceMode::Grid;
    surface.CellSize = kCellSize;
    surface.FitBounds = false;
    surface.Extent = {3.0f, 1.0f, 3.0f};
    entity.Set(surface);
    world->ProcessCommands();

    NavigationSurfaceSystem system;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline)
    {
        system.Update(*world, kDeltaTime);
        world->ProcessCommands();
        const auto& current = *entity.Get<Components::NavigationSurface>();
        if (current.BakePhase != Components::NavigationBakePhase::Waiting)
            break;
        EXPECT_EQ(Navigation::FormatBakeStatus(current), "Waiting for: model NavigationFloor is still loading");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto& baked = *entity.Get<Components::NavigationSurface>();
    EXPECT_EQ(baked.BakePhase, Components::NavigationBakePhase::Baked) << Navigation::FormatBakeStatus(baked);
    EXPECT_TRUE(assets.IsAssetLoaded(model));
}

// Shutdown-order hazard: the editor destroys the SystemManager (and every
// system in it) before it stops file watching, so a navigation-asset event can
// land while the system is being destroyed.
//
// A higher-priority co-subscriber on the same file reproduces that window
// exactly: it runs first, inside the same dispatch loop, and destroys the
// system while the system's own callback is already matched and pending. The
// system's unsubscribe must keep that pending call from running.
TEST_F(NavigationAssetWatchTest, EventDispatchedWhileTheSystemIsDestroyedDoesNotTouchIt)
{
    auto system = std::make_unique<NavigationSurfaceSystem>();
    system->Update(*world, kDeltaTime);
    world->ProcessCommands();

    std::atomic<bool> systemDestroyed{false};

    // Priority 10 > the system's default 0, so this runs first in the dispatch
    // loop. Both subscriptions stay on the asset root's watcher, so tearing the
    // system down here cannot stop (and self-join) the watcher thread we are on.
    // Matching the system's own Modified-only filter is what makes this bite:
    // on any other event type the system's callback returns before touching
    // its captured state, so destroying during one would prove nothing.
    constexpr int kAheadOfTheSystem = 10;
    auto destroyer = FileWatchingService::GetInstance().Subscribe(
        FilePattern(assetRoot, ".*", {".navgrid"}, true),
        [&system, &systemDestroyed](const FileChangeEvent& event) {
            if (event.Type != FileChangeType::Modified)
                return;
            if (!systemDestroyed.exchange(true))
                system.reset();
        },
        kAheadOfTheSystem);

    const std::filesystem::path probe = assetRoot / "TeardownProbe.navgrid";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (int revision = 0; !systemDestroyed.load() &&
                           std::chrono::steady_clock::now() < deadline; ++revision)
    {
        TouchNavGrid(probe, revision);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ASSERT_TRUE(systemDestroyed.load())
        << "no .navgrid event was delivered, so the teardown window was never entered";

    // Let the dispatch that destroyed the system finish its loop, past the
    // system's own pending callback. Reaching here without a crash or a
    // sanitizer report is the assertion.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(system, nullptr);
}
