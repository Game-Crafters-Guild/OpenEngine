// A navigation bake over a paged terrain reads its finest height pages (RequireFinest): away from
// every camera it names the level-0 page it waits on and keeps a CPU window there until the page
// lands, then bakes.
#include "Components/Navigation/NavigationSurface.h"
#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Mathematics/Vector3.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainService.h"
#include "PagedTiledTerrain.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{

namespace TE = GameEngine::TerrainECS;
using Components::NavigationBakePhase;
using Components::NavigationSurface;
using Components::NavigationSurfaceMode;

constexpr float32 kHeightScale = 16.0f;
constexpr float32 kDeltaSeconds = 1.0f / 60.0f;

// A resolved world transform at (x, y, z), as TransformHierarchySystem leaves it.
Components::WorldTransform At(float32 x, float32 y, float32 z)
{
    Components::WorldTransform transform{};
    transform.matrix[0] = 1.0f;
    transform.matrix[5] = 1.0f;
    transform.matrix[10] = 1.0f;
    transform.matrix[12] = x;
    transform.matrix[13] = y;
    transform.matrix[14] = z;
    transform.Version = 1u;
    return transform;
}

// The terrain service's height pages need the engine's job system, so the engine runs (without its
// rendering loop, which would own the world services).
class NavigationSurfacePagedTerrainTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Engine::Initialize moves the CWD to WorkspaceDirectory, and Windows refuses to remove a
        // directory containing the CWD.
        originalCwd = std::filesystem::current_path();
        testDir = std::filesystem::temp_directory_path() /
                  ("ge_nav_paged_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(testDir);
        engine = std::make_unique<EngineCore>();
        ApplicationConfig config;
        config.WorkspaceDirectory = testDir.string();
        config.AssetDirectory = testDir.string();
        config.AssetDatabaseFile = "AssetDatabase.assetdb";
        config.AssetDatabaseCacheDirectory = ".Cache/AssetDatabase";
        ASSERT_TRUE(engine->Initialize(config));
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
        if (engine)
        {
            engine->Shutdown();
            engine.reset();
        }
        std::error_code ec;
        std::filesystem::current_path(originalCwd, ec);
        std::filesystem::remove_all(testDir, ec);
    }

    // One frame as the editor runs it: the navigation update, then the terrain's height pages
    // with one camera at `camera` (the reader windows the update asked for land at this Update).
    void Frame(NavigationSurfaceSystem& system, const TE::PagedTerrainRequest& request, const Mathematics::Vector3& camera)
    {
        system.Update(*world, kDeltaSeconds);
        world->ProcessCommands();
        TE::TerrainHeightPages& pages = TE::TerrainService::Get().GetHeightPages();
        pages.Request(request, {&camera, 1}, TE::PageLevelView{});
        pages.Update(++frame, kDeltaSeconds, false, 2);
    }

    const NavigationSurface& SurfaceOf(ECS::Entity& entity) { return *entity.Get<NavigationSurface>(); }

    std::filesystem::path testDir;
    std::filesystem::path originalCwd;
    std::unique_ptr<EngineCore> engine;
    std::unique_ptr<ECS::World> world;
    uint64 frame = 0;
};

} // namespace

TEST_F(NavigationSurfacePagedTerrainTest, ABakeAwayFromEveryCameraWaitsOnTheFinestPageThenBakes)
{
    TE::TerrainService& service = TE::TerrainService::Get();
    const TE::Test::PagedTiledTerrain paged = TE::Test::CreatePagedTiledTerrain(service, 2048.0f, 1024.0f, kHeightScale);
    TE::PagedTerrainRequest request = paged.Request;
    request.GpuPages = false; // only the CPU windows want level 0

    auto terrain = world->Create();
    terrain.Set(paged.Component);
    terrain.Set(Components::Transform{});
    terrain.Set(At(paged.Component.SizeX * 0.5f, 0.0f, paged.Component.SizeZ * 0.5f));
    // A 32 m grid about (1500, 300): inside level-0 page (11, 2), 1,400 m from the camera.
    auto surface = world->Create();
    NavigationSurface grid{};
    grid.Mode = NavigationSurfaceMode::Grid;
    grid.CellSize = 2.0f;
    grid.FitBounds = false;
    grid.Extent = {16.0f, 100.0f, 16.0f};
    surface.Set(grid);
    surface.Set(Components::Transform{});
    surface.Set(At(1500.0f, 0.0f, 300.0f));
    world->ProcessCommands();

    const Mathematics::Vector3 camera{100.0f, 50.0f, 100.0f};
    TE::TerrainHeightPages& pages = service.GetHeightPages();
    pages.Request(request, {&camera, 1}, TE::PageLevelView{});
    pages.Update(++frame, kDeltaSeconds, false, 2); // the pages have their source from here on

    NavigationSurfaceSystem system;
    Frame(system, request, camera); // the surface's first update schedules its bake
    Frame(system, request, camera);
    ASSERT_EQ(SurfaceOf(surface).BakePhase, NavigationBakePhase::Waiting) << Navigation::FormatBakeStatus(SurfaceOf(surface));
    EXPECT_EQ(SurfaceOf(surface).BakeWait, Components::NavigationBakeWait::TerrainPage);
    // The wait names a world point in the grid's bounds (one cell of slack), not a page.
    EXPECT_NEAR(SurfaceOf(surface).BlockingPoint.x, 1500.0f, 18.0f);
    EXPECT_NEAR(SurfaceOf(surface).BlockingPoint.z, 300.0f, 18.0f);

    constexpr uint64 kFramesToSettle = 2000;
    for (uint64 attempt = 0; attempt < kFramesToSettle && SurfaceOf(surface).BakePhase != NavigationBakePhase::Baked &&
                             SurfaceOf(surface).BakePhase != NavigationBakePhase::Failed;
         ++attempt)
    {
        Frame(system, request, camera);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(SurfaceOf(surface).BakePhase, NavigationBakePhase::Baked) << Navigation::FormatBakeStatus(SurfaceOf(surface));
}
