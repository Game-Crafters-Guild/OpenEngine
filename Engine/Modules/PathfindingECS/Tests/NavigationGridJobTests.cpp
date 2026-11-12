// A grid surface bakes as a job on the engine's job system: the update that starts it does not wait
// for it, the grid is published by the first update after it finishes, a bake requested meanwhile
// starts once that one is published, the map serves what it held before until then, and a bake whose
// surface, world or map goes away is cancelled.
#include "NavigationTestSurfaces.h"

#include "Components/Navigation/NavigationAgent.h"
#include "Components/Navigation/NavigationSurface.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/Component/ComponentRegistry.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Jobs/HeldWorkers.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "TerrainECS/TerrainService.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{

using Components::NavigationBakePhase;
using Components::NavigationSurface;

constexpr float32 kDeltaTime = 1.0f / 60.0f;
// The box field's square-cell digest and topology version (NavigationSurfaceTests).
constexpr uint64 kBoxFieldDigest = 13504515380522120182ull;
constexpr uint32 kBoxFieldTopology = 7827;

// A grid surface of `cellSize` cells over an 80 m floor under `boxes` small turned boxes: at 0.1 m,
// 640,000 cells, a bake that spans many updates.
ECS::Entity CreateLargeGridSurface(ECS::World& world, float32 cellSize, int32 boxes)
{
    Testing::CreateTurnedBox(world, 0.0f, -0.5f, 0.0f, 40.0f, 0.5f, 40.0f, 1.0f, 0.0f);
    for (int32 i = 0; i < boxes; ++i)
    {
        const float32 x = -38.0f + static_cast<float32>((i * 37) % 76);
        const float32 z = -38.0f + static_cast<float32>((i * 53) % 76);
        Testing::CreateTurnedBox(world, x, 1.0f, z, 0.4f, 1.0f, 0.3f, 0.6f, 0.8f);
    }
    auto entity = world.Create();
    NavigationSurface surface{};
    surface.Mode = Components::NavigationSurfaceMode::Grid;
    surface.FitBounds = false;
    surface.CellSize = cellSize;
    surface.Center = {0.0f, 2.0f, 0.0f};
    surface.Extent = {40.0f, 8.0f, 40.0f};
    entity.Set(surface);
    world.ProcessCommands();
    return entity;
}

// `count` mesh renderers showing the cube primitive at 0.2 m, with no collider, standing on the ground at y = 0 in
// rows of kCubesPerRow, kCubeSpacing apart from (-38, -38).
void CreateCubeMeshes(ECS::World& world, int32 count)
{
    constexpr int32 kCubesPerRow = 300;
    constexpr float32 kCubeSpacing = 0.25f;
    using Engine::Renderer::PrimitiveGenerator;
    for (int32 i = 0; i < count; ++i)
    {
        auto entity = world.Create();
        entity.Set(PrimitiveGenerator::MakePrimitiveMeshRenderer(nullptr, PrimitiveGenerator::CubeGuid(),
                                                                 PrimitiveGenerator::DefaultMaterialGuid()));
        entity.Set(Components::Transform{});
        Components::WorldTransform transform{};
        transform.Basis[0].x = transform.Basis[1].y = transform.Basis[2].z = 0.2f;
        transform.Position = Mathematics::WorldPosition{-38.0f + kCubeSpacing * static_cast<float32>(i % kCubesPerRow), 0.1,
                                                        -38.0f + kCubeSpacing * static_cast<float32>(i / kCubesPerRow)};
        transform.Version = 1u; // resolved, as TransformHierarchySystem leaves it
        entity.Set(transform);
    }
    world.ProcessCommands();
}

// An agent on the surface of `surface` at (-2.5, -2.5) with a path to (2.5, -2.5) requested.
ECS::Entity CreateAgentRequestingAPath(ECS::World& world, ECS::EntityHandle surface)
{
    auto agent = world.Create();
    Components::NavigationAgent navigationAgent{};
    navigationAgent.Surface = surface;
    navigationAgent.Radius = 0.2f;
    agent.Set(navigationAgent);
    Components::NavigationAgentState state{};
    state.DestinationX = 2.5f;
    state.DestinationZ = -2.5f;
    state.HasDestination = true;
    state.PathDirty = true;
    agent.Set(state);
    Components::WorldTransform start{};
    start.Position = Mathematics::WorldPosition{-2.5, 0.0, -2.5};
    start.Version = 1u;
    agent.Set(start);
    world.ProcessCommands();
    return agent;
}

class NavigationGridJobTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Engine::Initialize moves the CWD to WorkspaceDirectory, and Windows refuses to remove a
        // directory containing the CWD.
        originalCwd = std::filesystem::current_path();
        testDir = std::filesystem::temp_directory_path() /
                  ("ge_nav_grid_job_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(testDir);
        engine = std::make_unique<EngineCore>();
        ApplicationConfig config;
        config.WorkspaceDirectory = testDir.string();
        config.AssetDirectory = testDir.string();
        config.AssetDatabaseFile = "AssetDatabase.assetdb";
        config.AssetDatabaseCacheDirectory = ".Cache/AssetDatabase";
        ASSERT_TRUE(engine->Initialize(config));
        ASSERT_GT(engine->GetJobSystem().GetWorkerCount(), 0u);

        NavigationService::Initialize();
        world = std::make_unique<ECS::World>();
        RegisterNavigationWorldHooks(*world);
    }

    void TearDown() override
    {
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

    void Update()
    {
        system.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    // Background jobs queued or running on the engine's pool. A grid bake is one Background job, with
    // the chunks of its parallel passes while it runs them.
    uint32 BackgroundJobs() const
    {
        const JobSystem::JobSystemStatistics statistics = engine->GetJobSystem().GetStatistics();
        return statistics.Background.Queued + statistics.Background.Running;
    }

    // Waits, for at most ten seconds, until no more Background jobs are queued or running than `count`.
    void WaitForBackgroundJobs(uint32 count) const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (BackgroundJobs() > count && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Waits, for at most ten seconds, until a Background job runs while more than `queued` Background jobs wait: with
    // every worker but one held, the bake on that one has split a pass whose helpers cannot start, so the bake's own
    // thread runs every chunk of the pass. False on the timeout.
    bool WaitForTheBakeToSplitAPass(uint32 queued) const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const JobSystem::JobSystemStatistics statistics = engine->GetJobSystem().GetStatistics();
            if (statistics.Background.Running > 0 && statistics.Background.Queued > queued)
                return true;
            std::this_thread::yield();
        }
        return false;
    }

    // Waits, for at most ten seconds, until no Background job runs: the helpers of a split pass may still wait.
    void WaitUntilNoBackgroundJobRuns() const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (engine->GetJobSystem().GetStatistics().Background.Running > 0 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Updates until the surface's phase is neither Baking nor Waiting, for at most ten seconds.
    void UpdateUntilSettled(ECS::Entity& entity)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do
        {
            Update();
            const auto phase = Surface(entity).BakePhase;
            if (phase != NavigationBakePhase::Baking && phase != NavigationBakePhase::Waiting)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
    }

    // With every worker but one held, the bake's own thread runs every chunk of each pass it splits. A bake
    // cancelled as its first split pass starts (its surface destroyed, or the system, world and service torn
    // down) leaves the pool within a quarter of the whole bake. Then shuts the engine down, so a test calls it last.
    template <typename CreateSurface>
    void ExpectTheFirstSplitPassToStopWhenCancelled(const CreateSurface& createSurface)
    {
        JobSystem::WorkStealingThreadPool& pool = engine->GetJobSystem();
        const size_t heldWorkers = pool.GetWorkerCount() - 1;
        auto surface = createSurface();
        const uint32 jobsBefore = BackgroundJobs();

        std::chrono::steady_clock::duration wholeBake{};
        {
            Tests::HeldWorkers hold(pool, heldWorkers);
            ASSERT_FALSE(HasFailure()) << "not every worker but one was held";
            const auto bakeStart = std::chrono::steady_clock::now();
            UpdateUntilSettled(surface);
            wholeBake = std::chrono::steady_clock::now() - bakeStart;
            ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baked)
                << Navigation::FormatBakeStatus(Surface(surface));
        }
        WaitForBackgroundJobs(jobsBefore);
        const auto wholeBakeMs = std::chrono::duration_cast<std::chrono::milliseconds>(wholeBake).count();
        RecordProperty("WholeBakeMs", std::to_string(wholeBakeMs));

        std::chrono::steady_clock::duration stopped{};
        {
            Tests::HeldWorkers hold(pool, heldWorkers);
            ASSERT_FALSE(HasFailure()) << "not every worker but one was held";
            const uint32 queued = pool.GetStatistics().Background.Queued;
            ASSERT_TRUE(Navigation::RequestBake(*world, surface.GetHandle()));
            Update();
            ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baking);
            ASSERT_TRUE(WaitForTheBakeToSplitAPass(queued)) << "the bake split no pass";
            world->DestroyEntityImmediate(surface.GetHandle());
            const auto destroyed = std::chrono::steady_clock::now();
            Update();
            WaitUntilNoBackgroundJobRuns();
            stopped = std::chrono::steady_clock::now() - destroyed;
        }
        WaitForBackgroundJobs(jobsBefore);
        const auto stoppedMs = std::chrono::duration_cast<std::chrono::milliseconds>(stopped).count();
        RecordProperty("StoppedAfterMs", std::to_string(stoppedMs));
        EXPECT_EQ(BackgroundJobs(), jobsBefore);
        EXPECT_LT(stopped, wholeBake / 4) << "the bake ran " << stoppedMs << " ms after its surface was destroyed; "
                                          << "the whole bake on one worker takes " << wholeBakeMs << " ms";

        surface = createSurface();
        auto local = std::make_unique<NavigationSurfaceSystem>();
        // The engine's shutdown lets the held workers go: they take no more work, so the pass's helpers never start.
        const bool failedBeforeHold = HasFailure();
        Tests::HeldWorkers hold(pool, heldWorkers);
        ASSERT_EQ(HasFailure(), failedBeforeHold) << "not every worker but one was held";
        const uint32 queued = pool.GetStatistics().Background.Queued;
        local->Update(*world, kDeltaTime);
        world->ProcessCommands();
        ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baking);
        ASSERT_TRUE(WaitForTheBakeToSplitAPass(queued)) << "the bake split no pass";
        const auto teardownStart = std::chrono::steady_clock::now();
        local.reset();
        world.reset();
        ECS::ComponentRegistry::Clear();
        NavigationService::Shutdown();
        WaitUntilNoBackgroundJobRuns();
        const auto teardown = std::chrono::steady_clock::now() - teardownStart;
        // The engine's shutdown time grows with the machine's load, so the measured time ends before it.
        engine->Shutdown();
        engine.reset();
        const auto teardownMs = std::chrono::duration_cast<std::chrono::milliseconds>(teardown).count();
        RecordProperty("TeardownMs", std::to_string(teardownMs));
        EXPECT_LT(teardown, wholeBake / 4) << "the bake ran " << teardownMs << " ms after the teardown started; the "
                                           << "whole bake on one worker takes " << wholeBakeMs << " ms";
        NavigationService::Initialize();
    }

    const NavigationSurface& Surface(ECS::Entity& entity) { return *entity.Get<NavigationSurface>(); }
    const Pathfinding::GridMap* Grid(ECS::Entity& entity)
    {
        return ResolveNavigationSurfaceGrid(*world, Surface(entity));
    }

    Pathfinding::PathStatus FindPath(ECS::Entity& entity, float32 fromX, float32 fromZ, float32 toX, float32 toZ)
    {
        Pathfinding::PathRequest request;
        request.StartX = fromX;
        request.StartZ = fromZ;
        request.EndX = toX;
        request.EndZ = toZ;
        request.AgentRadius = 0.2f;
        auto& navigation = NavigationService::Get();
        Pathfinding::PathHandle path;
        const auto status = navigation.FindPath(Testing::SurfaceMap(*world, entity), request, path);
        if (path.IsValid())
            navigation.GetPathBuffer().FreePath(path);
        return status;
    }

    std::filesystem::path testDir;
    std::filesystem::path originalCwd;
    std::unique_ptr<EngineCore> engine;
    std::unique_ptr<ECS::World> world;
    NavigationSurfaceSystem system;
};

} // namespace

// The update that starts a grid bake returns while the bake's job cannot even start, and the first
// update after the job finishes publishes the grid: the same grid the bake makes on the caller.
TEST_F(NavigationGridJobTest, UpdateStartsTheBakeWithoutWaitingAndALaterUpdatePublishesIt)
{
    auto surface = Testing::CreateBoxFieldGridSurface(*world, Pathfinding::GridType::Square);
    Tests::HeldWorkers hold(engine->GetJobSystem());
    ASSERT_FALSE(HasFailure()) << "not every worker was held";

    const auto start = std::chrono::steady_clock::now();
    Update();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(5)) << "the update waited for the bake's job";
    EXPECT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baking) << Navigation::FormatBakeStatus(Surface(surface));
    EXPECT_EQ(Surface(surface).BakeVersion, 0u);

    hold.Release();
    UpdateUntilSettled(surface);

    ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baked) << Navigation::FormatBakeStatus(Surface(surface));
    EXPECT_EQ(Surface(surface).BakeVersion, 1u);
    const auto* grid = Grid(surface);
    ASSERT_NE(grid, nullptr);
    EXPECT_EQ(Testing::GridDigest(*grid), kBoxFieldDigest);
    EXPECT_EQ(grid->GetTopologyVersion(), kBoxFieldTopology);
}

// A bake requested while another is in flight waits for it: the bake in flight is published with the
// wall where it stood, then the requested one with the wall where it was moved to.
TEST_F(NavigationGridJobTest, ABakeRequestedWhileOneIsInFlightStartsOnceThatOneIsPublished)
{
    Testing::CreateTurnedBox(*world, 0.0f, -0.5f, 0.0f, 4.0f, 0.5f, 4.0f, 1.0f, 0.0f);
    auto wall = Testing::CreateTurnedBox(*world, -2.5f, 1.0f, 0.0f, 0.25f, 1.0f, 3.0f, 1.0f, 0.0f);
    auto surface = world->Create();
    NavigationSurface authored{};
    authored.Mode = Components::NavigationSurfaceMode::Grid;
    authored.CellSize = 1.0f;
    authored.FitBounds = false;
    authored.Extent = {4.0f, 4.0f, 4.0f};
    surface.Set(authored);
    world->ProcessCommands();

    Tests::HeldWorkers hold(engine->GetJobSystem());
    ASSERT_FALSE(HasFailure()) << "not every worker was held";
    const uint32 jobsBefore = BackgroundJobs();
    Update();
    ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baking);
    ASSERT_EQ(BackgroundJobs() - jobsBefore, 1u) << "the bake is not one queued Background job";

    auto* moved = wall.GetForWrite<Components::WorldTransform>();
    ASSERT_NE(moved, nullptr);
    moved->Position.X = 2.5;
    ASSERT_TRUE(Navigation::RequestBake(*world, surface.GetHandle()));
    Update();
    EXPECT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baking);
    EXPECT_EQ(BackgroundJobs() - jobsBefore, 1u) << "the requested bake started beside the one in flight";
    hold.Release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Surface(surface).BakeVersion == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Update();
    }

    ASSERT_EQ(Surface(surface).BakeVersion, 1u);
    EXPECT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baking) << "the requested bake did not start";
    EXPECT_TRUE(Grid(surface)->IsCellBlocked(1, 4)) << "the wall where it stood when the first bake started";
    UpdateUntilSettled(surface);
    ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baked);
    EXPECT_EQ(Surface(surface).BakeVersion, 2u);
    const auto* grid = Grid(surface);
    ASSERT_NE(grid, nullptr);
    EXPECT_TRUE(grid->IsCellBlocked(6, 4)) << "the wall where it was moved to";
    EXPECT_FALSE(grid->IsCellBlocked(1, 4));
}

// A mesh bake's tiles are bulk work. A Normal job published after a full update of tiles starts while
// those tiles still wait, so a bake never holds back a load's or a frame's work.
TEST_F(NavigationGridJobTest, AMeshBakesTilesDoNotDelayALaterNormalJob)
{
    constexpr uint32 kTiles = NavigationSurfaceSystem::kTileSnapshotsPerUpdate;
    JobSystem::WorkStealingThreadPool& pool = engine->GetJobSystem();
    ASSERT_LT(pool.GetWorkerCount(), kTiles) << "too many workers to see the order";
    Testing::CreateGroundPlane(*world);
    auto entity = world->Create();
    NavigationSurface surface{};
    surface.FitBounds = false;
    surface.Extent = {128.0f, 20.0f, 128.0f}; // 8 x 8 tiles of 32 m
    entity.Set(surface);
    world->ProcessCommands();

    std::atomic<int64> waitingAtProbe{-1};
    {
        Tests::HeldWorkers hold(pool);
        Update();
        ASSERT_EQ(Surface(entity).BakeTotal, kTiles);
        pool.EnqueueWork([&pool, &waitingAtProbe] {
            const JobSystem::JobSystemStatistics statistics = pool.GetStatistics();
            waitingAtProbe.store(int64(statistics.Normal.Queued) + int64(statistics.Background.Queued));
        });
    }
    ASSERT_TRUE(Tests::WaitUntil([&] { return waitingAtProbe.load() >= 0; }, std::chrono::seconds(10)));
    EXPECT_GE(waitingAtProbe.load(), int64(kTiles - pool.GetWorkerCount()))
        << "the Normal job waited behind the bake's tiles";
    UpdateUntilSettled(entity);
}

// A request on every update, each bake taking many updates: the surface publishes each bake it
// finishes, so the grid keeps up with the requests instead of waiting for them to stop.
TEST_F(NavigationGridJobTest, ARequestOnEveryUpdateStillPublishes)
{
    auto surface = CreateLargeGridSurface(*world, 0.1f, 400);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (Surface(surface).BakeVersion < 2 && std::chrono::steady_clock::now() < deadline)
    {
        ASSERT_TRUE(Navigation::RequestBake(*world, surface.GetHandle()));
        Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GE(Surface(surface).BakeVersion, 2u) << "no grid was published while the requests went on";
}

// A path requested during a surface's first bake waits for the grid as Pending, held back because the
// surface's map holds no completed bake, instead of failing on the one-cell placeholder; it is solved
// once the grid is published.
TEST_F(NavigationGridJobTest, APathRequestedDuringTheFirstBakeWaitsForTheGrid)
{
    Testing::CreateTurnedBox(*world, 0.0f, -0.5f, 0.0f, 4.0f, 0.5f, 4.0f, 1.0f, 0.0f);
    auto surface = world->Create();
    NavigationSurface authored{};
    authored.Mode = Components::NavigationSurfaceMode::Grid;
    authored.CellSize = 1.0f;
    authored.FitBounds = false;
    authored.Extent = {4.0f, 4.0f, 4.0f};
    surface.Set(authored);
    world->ProcessCommands();

    Tests::HeldWorkers hold(engine->GetJobSystem());
    ASSERT_FALSE(HasFailure()) << "not every worker was held";
    Update();
    ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baking);

    auto agent = CreateAgentRequestingAPath(*world, surface.GetHandle());

    NavigationPathfindingSystem paths;
    paths.Update(*world, kDeltaTime);
    world->ProcessCommands();
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(agent.Get<Components::NavigationAgentState>()->Status),
              Pathfinding::PathStatus::Pending);
    EXPECT_TRUE(agent.Get<Components::NavigationAgentState>()->PathDirty);
    EXPECT_EQ(agent.Get<Components::NavigationAgentState>()->Reason, Navigation::StatusReason::SurfaceNotBaked);

    hold.Release();
    UpdateUntilSettled(surface);
    ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baked);
    paths.Update(*world, kDeltaTime);
    world->ProcessCommands();
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(agent.Get<Components::NavigationAgentState>()->Status),
              Pathfinding::PathStatus::Complete);
}

// A first bake that failed leaves the surface's map the one-cell placeholder, which holds no completed
// bake: a path requested on it waits as Pending until a bake succeeds, instead of failing on the
// placeholder.
TEST_F(NavigationGridJobTest, APathRequestedAfterTheFirstBakeFailedWaitsForABake)
{
    auto surface = world->Create();
    NavigationSurface authored{};
    authored.Mode = Components::NavigationSurfaceMode::Grid;
    authored.CellSize = 1.0f;
    authored.FitBounds = false;
    authored.Extent = {4.0f, 4.0f, 4.0f};
    surface.Set(authored);
    world->ProcessCommands();

    Update();
    ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Failed) << Navigation::FormatBakeStatus(Surface(surface));
    ASSERT_EQ(Surface(surface).BakeFailure, Components::NavigationBakeFailure::NoGeometry);
    ASSERT_EQ(Surface(surface).BakeVersion, 0u);
    ASSERT_NE(Grid(surface), nullptr);

    auto agent = CreateAgentRequestingAPath(*world, surface.GetHandle());
    NavigationPathfindingSystem paths;
    paths.Update(*world, kDeltaTime);
    world->ProcessCommands();
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(agent.Get<Components::NavigationAgentState>()->Status),
              Pathfinding::PathStatus::Pending);
    EXPECT_TRUE(agent.Get<Components::NavigationAgentState>()->PathDirty);
    EXPECT_EQ(agent.Get<Components::NavigationAgentState>()->Reason, Navigation::StatusReason::SurfaceNotBaked);
}

// A surface baked once whose map is released bakes into a new map, which holds the one-cell placeholder
// and no completed bake until that bake is published: a path requested on the new map waits as Pending,
// as during a first bake.
TEST_F(NavigationGridJobTest, APathRequestedWhileABakeFillsANewMapWaitsForTheGrid)
{
    auto surface = Testing::CreateBoxFieldGridSurface(*world, Pathfinding::GridType::Square);
    UpdateUntilSettled(surface);
    ASSERT_EQ(Surface(surface).BakeVersion, 1u);
    ResetNavigationSurfaceMap(*world, *surface.GetForWrite<NavigationSurface>());

    Tests::HeldWorkers hold(engine->GetJobSystem());
    ASSERT_FALSE(HasFailure()) << "not every worker was held";
    Update();
    ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baking);
    ASSERT_NE(Grid(surface), nullptr);
    ASSERT_EQ(Grid(surface)->GetCellCount(), 1u);

    auto agent = CreateAgentRequestingAPath(*world, surface.GetHandle());
    NavigationPathfindingSystem paths;
    paths.Update(*world, kDeltaTime);
    world->ProcessCommands();
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(agent.Get<Components::NavigationAgentState>()->Status),
              Pathfinding::PathStatus::Pending);
    EXPECT_TRUE(agent.Get<Components::NavigationAgentState>()->PathDirty);
    EXPECT_EQ(agent.Get<Components::NavigationAgentState>()->Reason, Navigation::StatusReason::SurfaceNotBaked);
}

// While a grid bakes its phase is Baking. On a first bake the surface's map is the empty one-cell
// grid, so no path is found on it; on a re-bake the previous grid keeps serving, unchanged, until the
// new one is published.
TEST_F(NavigationGridJobTest, QueriesWhileTheGridBakesSeeThePreviousGrid)
{
    Testing::CreateTurnedBox(*world, 0.0f, -0.5f, 0.0f, 4.0f, 0.5f, 4.0f, 1.0f, 0.0f);
    auto wall = Testing::CreateTurnedBox(*world, 0.5f, 1.0f, -1.5f, 0.25f, 1.0f, 2.5f, 1.0f, 0.0f);
    auto surface = world->Create();
    NavigationSurface authored{};
    authored.Mode = Components::NavigationSurfaceMode::Grid;
    authored.CellSize = 1.0f;
    authored.FitBounds = false;
    authored.Extent = {4.0f, 4.0f, 4.0f};
    surface.Set(authored);
    world->ProcessCommands();

    {
        Tests::HeldWorkers hold(engine->GetJobSystem());
        ASSERT_FALSE(HasFailure()) << "not every worker was held";
        Update();
        ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baking);
        const auto* placeholder = Grid(surface);
        ASSERT_NE(placeholder, nullptr);
        EXPECT_EQ(placeholder->GetCellCount(), 1u);
        EXPECT_NE(FindPath(surface, -2.5f, -2.5f, 2.5f, -2.5f), Pathfinding::PathStatus::Complete);
    }
    UpdateUntilSettled(surface);
    ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baked);
    const auto* first = Grid(surface);
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(first->IsCellBlocked(4, 2)) << "the wall's first position";
    const uint32 firstTopology = first->GetTopologyVersion();

    auto* moved = wall.GetForWrite<Components::WorldTransform>();
    ASSERT_NE(moved, nullptr);
    moved->Position.X = -1.5;
    Tests::HeldWorkers hold(engine->GetJobSystem());
    ASSERT_FALSE(HasFailure()) << "not every worker was held";
    ASSERT_TRUE(Navigation::RequestBake(*world, surface.GetHandle()));
    Update();
    EXPECT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baking);
    EXPECT_EQ(Surface(surface).BakeVersion, 1u);
    const auto* serving = Grid(surface);
    ASSERT_EQ(serving, first) << "the previous grid is replaced only when the new one is published";
    EXPECT_TRUE(serving->IsCellBlocked(4, 2));
    EXPECT_EQ(serving->GetTopologyVersion(), firstTopology);
    EXPECT_EQ(FindPath(surface, -2.5f, -2.5f, 2.5f, -2.5f), Pathfinding::PathStatus::Complete);

    hold.Release();
    UpdateUntilSettled(surface);
    ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baked);
    EXPECT_EQ(Surface(surface).BakeVersion, 2u);
    EXPECT_TRUE(Grid(surface)->IsCellBlocked(2, 2)) << "the wall's new position";
    EXPECT_FALSE(Grid(surface)->IsCellBlocked(4, 2));
}

// Tearing a surface down while its bake's job runs cancels the bake, and the pass it is in stops at its next chunk. The
// floor alone has too few triangles to split the triangle passes, so the first pass split is the slope pass, one chunk
// of rows per thread.
TEST_F(NavigationGridJobTest, TearingDownWhileTheBakeRunsCancelsIt)
{
    ExpectTheFirstSplitPassToStopWhenCancelled([this] { return CreateLargeGridSurface(*world, 0.05f, 0); });
}

// With terrain inside a grid surface's bounds, the bake's job builds the static triangles itself before its grid
// passes, splitting the mesh instances over the pool. With 60,000 cube meshes on the terrain that build is the first
// pass the bake splits, about half of the whole bake: cancelled as it starts, the build stops at its next chunk.
TEST_F(NavigationGridJobTest, TearingDownWhileTheBakeBuildsItsTrianglesCancelsIt)
{
    // The engine starts its world services only with its rendering loop; its shutdown stops them.
    TerrainECS::TerrainService::Initialize();
    Testing::CreateFlatTerrain(*world);
    CreateCubeMeshes(*world, 60000);
    ExpectTheFirstSplitPassToStopWhenCancelled([this] { return CreateLargeGridSurface(*world, 0.5f, 0); });
}

// A scene replaced while the old world's bake is still queued: the system bakes the new world's
// surface and publishes it once, and the old bake publishes nothing.
TEST_F(NavigationGridJobTest, AWorldReplacedMidBakePublishesOnlyTheNewWorldsGrid)
{
    CreateLargeGridSurface(*world, 0.1f, 400);
    const uint32 jobsBefore = BackgroundJobs();
    Tests::HeldWorkers hold(engine->GetJobSystem());
    ASSERT_FALSE(HasFailure()) << "not every worker was held";
    Update();
    world.reset();
    world = std::make_unique<ECS::World>();
    RegisterNavigationWorldHooks(*world);
    auto surface = Testing::CreateBoxFieldGridSurface(*world, Pathfinding::GridType::Square);
    Update();
    EXPECT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baking);

    hold.Release();
    UpdateUntilSettled(surface);
    WaitForBackgroundJobs(jobsBefore);
    for (int32 i = 0; i < 5; ++i)
        Update();
    ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baked);
    EXPECT_EQ(Surface(surface).BakeVersion, 1u);
    ASSERT_NE(Grid(surface), nullptr);
    EXPECT_EQ(Testing::GridDigest(*Grid(surface)), kBoxFieldDigest);
}

// A surface whose map is released while its bake is queued cancels that bake and, in the same update,
// bakes again into a new map; it publishes once, the right grid.
TEST_F(NavigationGridJobTest, AMapReleasedMidBakeBakesAgainIntoANewMap)
{
    auto surface = Testing::CreateBoxFieldGridSurface(*world, Pathfinding::GridType::Square);
    const uint32 jobsBefore = BackgroundJobs();
    Tests::HeldWorkers hold(engine->GetJobSystem());
    ASSERT_FALSE(HasFailure()) << "not every worker was held";
    Update();
    ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baking);
    const auto oldMap = Testing::SurfaceMap(*world, surface);
    ResetNavigationSurfaceMap(*world, *surface.GetForWrite<NavigationSurface>());
    Update();
    EXPECT_TRUE(Testing::SurfaceMap(*world, surface).IsValid())
        << "the bake into the released map was not cancelled, so the bake into a new map waits for it";

    hold.Release();
    UpdateUntilSettled(surface);
    WaitForBackgroundJobs(jobsBefore);
    for (int32 i = 0; i < 3; ++i)
        Update();
    ASSERT_EQ(Surface(surface).BakePhase, NavigationBakePhase::Baked);
    EXPECT_EQ(Surface(surface).BakeVersion, 1u);
    const auto newMap = Testing::SurfaceMap(*world, surface);
    EXPECT_TRUE(newMap.IsValid());
    EXPECT_FALSE(newMap == oldMap);
    ASSERT_NE(Grid(surface), nullptr);
    EXPECT_EQ(Testing::GridDigest(*Grid(surface)), kBoxFieldDigest);
}
