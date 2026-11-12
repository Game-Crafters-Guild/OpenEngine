// SceneLoader on a headless engine: the read runs before the clear, Revealed waits for the tracker,
// a newer load cancels an older one, Shutdown waits for every read, user systems start once across
// loads, OnInstantiated sees the lines that did not apply, and a load sets the resolve slice. An
// additive load keeps the world and its tick, waits for the tracker, and fails while another load runs.
// A load with PauseWorldDrawing pauses the cameras, resumes the ones it paused, and counts ready frames.
//
// Frames run through Application::Tick. While a test holds every job worker, so that a read
// stays queued, it ticks SceneLoader alone: the frame's systems may wait for workers.
#include "../Source/Engine/Loading/SceneLoadState.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Engine/Hosting/RuntimeHost.h"
#include "Engine/Loading/SceneLoadInstantiated.h"
#include "Engine/Loading/SceneLoader.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "NativeScripting/UserSystemRegistry.h"
#include "Scene/SceneIO.h"
#include "SceneLoadTracker/SceneLoadTracker.h"
#include "Scripting/ScriptsConfig.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Tracker = SceneLoadTracker;

namespace GameEngine
{
// Test-only seam onto the loader's host calls, its current load and the resolve slice that a load
// sets. In the engine only EngineCore calls Tick and Shutdown. Friended by SceneLoader and RenderingLoop.
struct SceneLoaderTestAccess
{
    static constexpr std::chrono::microseconds kFrameBound = SceneLoader::kFrameBound;

    static void Tick(SceneLoader& loader, std::chrono::steady_clock::time_point now,
                     std::chrono::microseconds gpuWait = std::chrono::microseconds{0})
    {
        loader.Tick(now, gpuWait);
    }
    static void Shutdown(SceneLoader& loader) { loader.Shutdown(); }
    static SceneLoadState* Current(SceneLoader& loader) { return loader.m_Current.get(); }
    static std::chrono::microseconds ResolveSlice(const Engine::Renderer::RenderingLoop& loop)
    {
        return loop.m_ResolveSlice;
    }
};
} // namespace GameEngine

namespace
{
constexpr int kFrameCap = 600;
constexpr uint64_t kSavedTick = 1'000'000;

class LoaderTestApplication final : public Application
{
  public:
    using Application::Application;
};

// Holds every job worker until Release, so that a job submitted meanwhile stays queued.
class WorkerHold
{
  public:
    explicit WorkerHold(JobSystem::WorkStealingThreadPool& pool)
    {
        const std::size_t workers = pool.GetWorkerCount();
        for (std::size_t i = 0; i < workers; ++i)
            m_Tasks.push_back(pool.Submit([this] { Hold(); }));
        for (int i = 0; i < 2000 && m_Held.load() < workers; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        m_AllHeld = m_Held.load() == workers;
    }
    WorkerHold(const WorkerHold&) = delete;
    WorkerHold& operator=(const WorkerHold&) = delete;
    ~WorkerHold()
    {
        Release();
        for (JobSystem::TaskHandle& task : m_Tasks)
            task.Wait();
    }

    bool AllHeld() const { return m_AllHeld; }
    void Release()
    {
        {
            std::lock_guard lock(m_Mutex);
            m_Released = true;
        }
        m_Wake.notify_all();
    }

  private:
    void Hold()
    {
        m_Held.fetch_add(1);
        std::unique_lock lock(m_Mutex);
        m_Wake.wait(lock, [this] { return m_Released; });
    }

    std::vector<JobSystem::TaskHandle> m_Tasks;
    std::atomic<std::size_t> m_Held{0};
    std::mutex m_Mutex;
    std::condition_variable m_Wake;
    bool m_Released = false;
    bool m_AllHeld = false;
};

// Counts its destruction and notes one off the main thread. A request's OnInstantiated
// holds it, so it is destroyed with the load's state.
struct ReleaseProbe
{
    ReleaseProbe(std::atomic<int>& released, std::atomic<bool>& offMainThread)
        : Released(released), OffMainThread(offMainThread)
    {
    }
    ReleaseProbe(const ReleaseProbe&) = delete;
    ReleaseProbe& operator=(const ReleaseProbe&) = delete;

    std::atomic<int>& Released;
    std::atomic<bool>& OffMainThread;
    std::thread::id MainThread = std::this_thread::get_id();

    ~ReleaseProbe()
    {
        if (std::this_thread::get_id() != MainThread)
            OffMainThread.store(true);
        Released.fetch_add(1);
    }
};

// Counts the OnStart calls the registry makes.
class CountingUserSystem final : public NativeScripting::IUserSystem
{
  public:
    void OnStart(ECS::World&) override { ++Starts; }
    void OnUpdate(ECS::World&, float) override {}
    void OnDestroy(ECS::World&) override {}
    const char* Name() const override { return "CountingUserSystem"; }

    int Starts = 0;
};

// Creates a camera in OnStart, as a game's native system can.
class CameraCreatingUserSystem final : public NativeScripting::IUserSystem
{
  public:
    void OnStart(ECS::World& world) override
    {
        world.Create(Components::Camera{}, Components::Transform{}, Components::MakeName("Started camera"));
        Started = true;
    }
    void OnUpdate(ECS::World&, float) override {}
    void OnDestroy(ECS::World&) override {}
    const char* Name() const override { return "CameraCreatingUserSystem"; }

    bool Started = false;
};

SceneLoadRequest ProbedRequest(const char* scene, std::atomic<int>& released, std::atomic<bool>& offMainThread)
{
    auto probe = std::make_shared<ReleaseProbe>(released, offMainThread);
    return {.Scene = scene, .OnInstantiated = [probe](SceneLoadInstantiated&) {}};
}

void WriteFile(const fs::path& path, const char* text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

class SceneLoaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_PreviousCwd = fs::current_path();
        m_Root = fs::temp_directory_path() / ("scene-loader-" + GUID::Generate().ToString());
        for (const char* name : {"A", "B", "C"})
        {
            const std::string text = std::string("[scene name=\"") + name + "\" version=1]\n[entity id=\"e\"]\nName.value = \"" +
                                     name + "\"\n";
            WriteFile(m_Root / "Assets/Scenes" / (std::string(name) + ".scene"), text.c_str());
        }
        // Name has no field "nickname": the load applies the entity and records one skip.
        WriteFile(m_Root / "Assets/Scenes/D.scene",
                  "[scene name=\"D\" version=1]\n[entity id=\"e\"]\nName.value = \"D\"\nName.nickname = \"x\"\n");
        // Two entities for the additive loads, saved at a tick that no test's frames reach.
        const std::string town = "[scene name=\"Town\" version=1 simulation_tick=" + std::to_string(kSavedTick) +
                                 "]\n[entity id=\"gate\"]\nName.value = \"Town gate\"\n"
                                 "[entity id=\"well\"]\nName.value = \"Town well\"\n";
        WriteFile(m_Root / "Assets/Scenes/Town.scene", town.c_str());
        // Cameras for the loads with PauseWorldDrawing. The scene saves "B map" paused.
        WriteFile(m_Root / "Assets/Scenes/CamA.scene", "[scene name=\"CamA\" version=1]\n[entity id=\"cam\"]\n"
                                                       "Name.value = \"A camera\"\nCamera.worldDrawingPaused = false\n");
        WriteFile(m_Root / "Assets/Scenes/CamB.scene",
                  "[scene name=\"CamB\" version=1]\n[entity id=\"cam\"]\nName.value = \"B camera\"\n"
                  "Camera.worldDrawingPaused = false\n[entity id=\"map\"]\nName.value = \"B map\"\n"
                  "Camera.worldDrawingPaused = true\n");
        WriteFile(m_Root / "Assets/Scenes/CamC.scene", "[scene name=\"CamC\" version=1]\n[entity id=\"cam\"]\n"
                                                       "Name.value = \"C camera\"\nCamera.worldDrawingPaused = false\n");
        WriteFile(m_Root / "Assets/Scenes/Outpost.scene",
                  "[scene name=\"Outpost\" version=1]\n[entity id=\"cam\"]\nName.value = \"Outpost camera\"\n"
                  "Camera.worldDrawingPaused = false\n");

        ScriptsConfig scripts{};
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scripts);
        ApplicationConfig config{};
        config.Name = "SceneLoaderTest";
        config.WorkspaceDirectory = m_Root.string();
        config.AssetDirectory = "Assets";
        config.Headless = true;
        m_Application = std::make_unique<LoaderTestApplication>(config);
        ASSERT_TRUE(m_Application->Initialize());
        // A read that a test holds must be submitted at its first loader tick.
        EngineCore::GetInstance().GetAssetManager().WaitForStartupScan(kAssetSourceAliasProject);
        m_Host = std::make_unique<RuntimeHost>(*m_Application, RuntimeHostDesc{}, RuntimeHostHooks{});
        ASSERT_TRUE(m_Host->InitWindow());
        ASSERT_TRUE(m_Host->InitRendering());
    }

    void TearDown() override
    {
        if (ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld())
            NativeScripting::StopUserSystems(*world);
        NativeScripting::ClearUserSystems();
        if (m_Host)
            m_Host->Shutdown();
        m_Host.reset();
        if (m_Application)
            m_Application->Shutdown();
        m_Application.reset();
        std::error_code ec;
        fs::current_path(m_PreviousCwd, ec);
        fs::remove_all(m_Root, ec);
    }

    static SceneLoader& Loader() { return EngineCore::GetInstance().GetSceneLoader(); }
    static Tracker& LoadTracker() { return EngineCore::GetInstance().GetSceneLoadTracker(); }

    // Runs frames until `done`, at most kFrameCap of them. False at the cap.
    bool TickUntil(const std::function<bool()>& done)
    {
        for (int frame = 0; frame < kFrameCap && !done(); ++frame)
            m_Application->Tick();
        return done();
    }

    // Loads `scene` and runs frames until it is revealed.
    bool Show(const char* scene)
    {
        bool revealed = false;
        SceneLoadOperation load = Loader().Load({.Scene = scene});
        const EventSubscription sub = load.Revealed() += [&] { revealed = true; };
        return TickUntil([&] { return revealed; });
    }

    static bool Holds(std::string_view name)
    {
        ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
        bool found = false;
        if (world)
            world->Query<ECS::Read<Components::Name>>().Each(
                [&](const Components::Name& entityName) { found = found || entityName.View() == name; });
        return found;
    }

    // The WorldDrawingPaused of the camera named `name`, or nullopt when the world has no such camera.
    static std::optional<bool> CameraPaused(std::string_view name)
    {
        std::optional<bool> paused;
        if (ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld())
            world->Query<ECS::Read<Components::Name>, ECS::Read<Components::Camera>>().Each(
                [&](const Components::Name& entityName, const Components::Camera& camera)
                {
                    if (entityName.View() == name)
                        paused = camera.WorldDrawingPaused;
                });
        return paused;
    }

    static void SetCameraPaused(std::string_view name, bool paused)
    {
        EngineCore::GetInstance().GetPrimaryWorld()->Query<ECS::Read<Components::Name>, ECS::Write<Components::Camera>>().Each(
            [&](const Components::Name& entityName, Components::Camera& camera)
            {
                if (entityName.View() == name)
                    camera.WorldDrawingPaused = paused;
            });
    }

    // The category's failed count in the session, at the last tracker Tick.
    static uint64_t FailedIn(std::string_view id)
    {
        for (const Tracker::CategoryStatus& row : LoadTracker().GetStatus().Categories)
            if (row.Id == id)
                return row.Failed;
        return 0;
    }

    fs::path m_PreviousCwd;
    fs::path m_Root;
    std::unique_ptr<LoaderTestApplication> m_Application;
    std::unique_ptr<RuntimeHost> m_Host;
};
} // namespace

TEST_F(SceneLoaderTest, AReadThatFailsLeavesTheShownSceneAndFailsItsTicket)
{
    ASSERT_TRUE(Show("Scenes/A.scene"));
    const uint64_t failedReads = FailedIn("scene.read");
    std::optional<std::string> failure;
    SceneLoadOperation load = Loader().Load({.Scene = "Scenes/Missing.scene"});
    const EventSubscription sub = load.Failed() += [&](std::string_view reason) { failure = std::string(reason); };
    ASSERT_TRUE(TickUntil([&] { return failure.has_value(); }));
    EXPECT_FALSE(failure->empty());
    EXPECT_TRUE(Holds("A")) << "the read runs before the clear, so the shown scene stays";
    EXPECT_FALSE(Loader().IsLoading());
    m_Application->Tick();
    EXPECT_EQ(FailedIn("scene.read"), failedReads + 1) << "the failed read's ticket counts as failed";
}

TEST_F(SceneLoaderTest, TheInstantiatedHandlerCanFailTheLoadAndItsTicket)
{
    CountingUserSystem counter;
    ASSERT_TRUE(NativeScripting::RegisterUserSystem(&counter));
    m_Application->Tick();
    const uint64_t failedInstantiates = FailedIn("scene.instantiate");
    std::optional<std::string> failure;
    bool revealed = false;
    SceneLoadOperation load = Loader().Load(
        {.Scene = "Scenes/A.scene", .OnInstantiated = [](SceneLoadInstantiated& loaded) { loaded.Fail("refused"); }});
    const EventSubscription failedSub = load.Failed() += [&](std::string_view reason) { failure = std::string(reason); };
    const EventSubscription revealedSub = load.Revealed() += [&] { revealed = true; };
    const bool failed = TickUntil([&] { return failure.has_value(); });
    m_Application->Tick();
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    const bool started = world && NativeScripting::AreUserSystemsStarted(*world);
    // The registry keeps a raw pointer: stop and drop the system before it goes out of scope.
    if (world)
        NativeScripting::StopUserSystems(*world);
    NativeScripting::ClearUserSystems();
    ASSERT_TRUE(failed);
    EXPECT_EQ(*failure, "refused");
    EXPECT_EQ(counter.Starts, 0) << "a refused scene starts no user system";
    EXPECT_FALSE(started);
    EXPECT_TRUE(Holds("A")) << "the refused scene stays in the primary world";
    EXPECT_FALSE(revealed);
    EXPECT_FALSE(Loader().IsLoading());
    EXPECT_EQ(FailedIn("scene.instantiate"), failedInstantiates + 1) << "the refused load's ticket counts as failed";
}

TEST_F(SceneLoaderTest, TheInstantiatedHandlerSeesTheLinesThatDidNotApply)
{
    std::vector<Scene::SceneDegradation::Skip> skips;
    bool revealed = false;
    SceneLoadOperation load =
        Loader().Load({.Scene = "Scenes/D.scene",
                       .OnInstantiated = [&](SceneLoadInstantiated& loaded) { skips = loaded.Degradation().skips; }});
    const EventSubscription sub = load.Revealed() += [&] { revealed = true; };
    ASSERT_TRUE(TickUntil([&] { return revealed; }));
    ASSERT_EQ(skips.size(), 1u) << "Degradation() lists the line that did not apply";
    EXPECT_EQ(skips[0].component, "Name");
    EXPECT_EQ(skips[0].field, "nickname");
    EXPECT_TRUE(Holds("D")) << "a line that does not apply leaves the rest of the scene";
}

TEST_F(SceneLoaderTest, UserSystemsStartAtTheFirstInstantiateAndALaterLoadKeepsThem)
{
    CountingUserSystem counter;
    ASSERT_TRUE(NativeScripting::RegisterUserSystem(&counter));
    const bool shownA = Show("Scenes/A.scene");
    const int startsAfterA = counter.Starts;
    const bool shownB = Show("Scenes/B.scene");
    // The registry keeps a raw pointer: stop and drop the system before it goes out of scope.
    if (ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld())
        NativeScripting::StopUserSystems(*world);
    NativeScripting::ClearUserSystems();
    ASSERT_TRUE(shownA);
    ASSERT_TRUE(shownB);
    EXPECT_EQ(startsAfterA, 1) << "the first instantiate starts the user systems";
    EXPECT_EQ(counter.Starts, 1) << "a later load keeps the user systems that run";
}

TEST_F(SceneLoaderTest, RevealedComesOnALaterFrameOnceTheTrackerIsReady)
{
    Tracker::Category generation = LoadTracker().RegisterCategory({.Id = "test.generation", .Label = "Generating"});
    Tracker::WorkTicket step;
    std::optional<uint64_t> instantiatedFrame;
    std::optional<uint64_t> revealedFrame;
    SceneLoadOperation load = Loader().Load({.Scene = "Scenes/A.scene",
                                             .OnInstantiated = [&](SceneLoadInstantiated&)
                                             {
                                                 instantiatedFrame = m_Application->GetFrameCount();
                                                 step = generation.BeginWork(LoadTracker().Intern("generate"), 1.0);
                                             }});
    const EventSubscription sub = load.Revealed() += [&] { revealedFrame = m_Application->GetFrameCount(); };
    ASSERT_TRUE(TickUntil([&] { return instantiatedFrame.has_value(); }));
    for (int frame = 0; frame < 5; ++frame)
        m_Application->Tick();
    EXPECT_FALSE(revealedFrame.has_value()) << "a ticket begun in OnInstantiated holds the reveal";
    EXPECT_TRUE(Loader().IsLoading());

    step.Complete();
    ASSERT_TRUE(TickUntil([&] { return revealedFrame.has_value(); }));
    EXPECT_FALSE(Loader().IsLoading());

    // With nothing held, the reveal still waits for the snapshot of the frame after the instantiate.
    std::optional<uint64_t> nextInstantiated;
    std::optional<uint64_t> nextRevealed;
    SceneLoadOperation next = Loader().Load(
        {.Scene = "Scenes/B.scene",
         .OnInstantiated = [&](SceneLoadInstantiated&) { nextInstantiated = m_Application->GetFrameCount(); }});
    const EventSubscription nextSub = next.Revealed() += [&] { nextRevealed = m_Application->GetFrameCount(); };
    ASSERT_TRUE(TickUntil([&] { return nextRevealed.has_value(); }));
    ASSERT_TRUE(nextInstantiated.has_value());
    EXPECT_GT(*nextRevealed, *nextInstantiated) << "Revealed comes on a frame after the instantiate";
}

TEST_F(SceneLoaderTest, AShortLoadShowsNoBarAndIsRevealedInTheFrameItIsReady)
{
    SceneLoadOperation load = Loader().Load({.Scene = "Scenes/A.scene"});
    bool revealed = false;
    std::optional<bool> shownAtReveal;
    const EventSubscription sub = load.Revealed() += [&]
    {
        revealed = true;
        shownAtReveal = load.IsProgressShown();
    };
    bool readyBeforeReveal = false;
    ASSERT_TRUE(TickUntil(
        [&]
        {
            readyBeforeReveal = readyBeforeReveal || (load.IsReadyToShow() && !revealed);
            return revealed;
        }));
    EXPECT_FALSE(*shownAtReveal) << "a load this short shows no bar";
    EXPECT_FALSE(readyBeforeReveal) << "with no bar, the reveal does not wait after ready";
}

TEST_F(SceneLoaderTest, AGameReadsTheProgressEachFrameAndTheLoadIsRevealedInTheFrameItIsReady)
{
    // A 4 s estimate held past 1 s of load: the bar shows, and it is low when the load is ready.
    Tracker::Category generation = LoadTracker().RegisterCategory(
        {.Id = "test.generation", .Label = "Generating", .DefaultSecondsPerUnit = 4.0});
    Tracker::WorkTicket step;
    bool begun = false;
    SceneLoadOperation load = Loader().Load({.Scene = "Scenes/A.scene",
                                             .OnInstantiated = [&](SceneLoadInstantiated&)
                                             {
                                                 step = generation.BeginWork(LoadTracker().Intern("generate"), 1.0);
                                                 begun = true;
                                             }});
    int readyCount = 0;
    std::optional<float> progressAtReady;
    std::optional<uint64_t> readyFrame;
    std::optional<uint64_t> revealedFrame;
    std::optional<bool> shownAtReveal;
    int readyCountAtReveal = 0;
    const EventSubscription readySub = load.Ready() += [&]
    {
        ++readyCount;
        progressAtReady = load.Progress();
        readyFrame = m_Application->GetFrameCount();
    };
    const EventSubscription revealedSub = load.Revealed() += [&]
    {
        readyCountAtReveal = readyCount;
        shownAtReveal = load.IsProgressShown();
        revealedFrame = m_Application->GetFrameCount();
    };
    const Clock::time_point start = Clock::now();
    ASSERT_TRUE(TickUntil([&] { return begun; }));
    float last = 0.0f;
    while (Clock::now() - start < std::chrono::milliseconds(1200))
    {
        m_Application->Tick();
        ASSERT_GE(load.Progress(), last);
        last = load.Progress();
    }
    EXPECT_FALSE(load.IsReadyToShow());
    EXPECT_TRUE(load.IsProgressShown()) << "run 1.2 s with an estimated total over 2.5 s";
    EXPECT_GT(load.EstimatedSecondsRemaining(), 2.0) << "the held 4 s ticket";
    EXPECT_GT(last, 0.0f);
    EXPECT_LT(last, 0.99f);

    step.Complete();
    ASSERT_TRUE(TickUntil([&] { return revealedFrame.has_value(); }));
    ASSERT_TRUE(readyFrame.has_value());
    EXPECT_EQ(readyCount, 1);
    EXPECT_EQ(readyCountAtReveal, 1) << "Ready comes before Revealed";
    EXPECT_EQ(*progressAtReady, 1.0f) << "the speed limit does not hold the last step";
    EXPECT_EQ(*revealedFrame, *readyFrame) << "SceneRevealPolicy::AtReady reveals in the frame of Ready";
    EXPECT_TRUE(*shownAtReveal) << "the bar stays shown";
    EXPECT_EQ(load.EstimatedSecondsRemaining(), 0.0);
}

TEST_F(SceneLoaderTest, ALoadThatTheGameReleasesStaysReadyUntilReveal)
{
    SceneLoadOperation load =
        Loader().Load({.Scene = "Scenes/A.scene", .Reveal = SceneRevealPolicy::WhenGameReleases});
    int readyCount = 0;
    int revealedCount = 0;
    const EventSubscription readySub = load.Ready() += [&] { ++readyCount; };
    const EventSubscription revealedSub = load.Revealed() += [&] { ++revealedCount; };
    ASSERT_TRUE(TickUntil([&] { return readyCount == 1; }));
    for (int frame = 0; frame < 10; ++frame)
        m_Application->Tick();
    EXPECT_EQ(revealedCount, 0) << "the load waits for the game";
    EXPECT_TRUE(load.IsReadyToShow());
    EXPECT_EQ(load.Progress(), 1.0f);
    EXPECT_TRUE(Loader().IsLoading());

    load.Reveal();
    m_Application->Tick();
    EXPECT_EQ(revealedCount, 1) << "Revealed comes at the next frame boundary";
    EXPECT_FALSE(Loader().IsLoading());
    load.Reveal();
    for (int frame = 0; frame < 3; ++frame)
        m_Application->Tick();
    EXPECT_EQ(revealedCount, 1);
    EXPECT_EQ(readyCount, 1);
}

TEST_F(SceneLoaderTest, ARevealBeforeReadyOrFromReadyRevealsTheLoadInTheFrameOfReady)
{
    // {scene, Reveal() before ready}: a call right after Load, and a call from the Ready handler.
    for (const auto& [scene, early] : {std::pair{"Scenes/A.scene", true}, std::pair{"Scenes/B.scene", false}})
    {
        SceneLoadOperation load = Loader().Load({.Scene = scene, .Reveal = SceneRevealPolicy::WhenGameReleases});
        if (early)
            load.Reveal();
        std::optional<uint64_t> readyFrame;
        std::optional<uint64_t> revealedFrame;
        const EventSubscription readySub = load.Ready() += [&, revealInReady = !early]
        {
            readyFrame = m_Application->GetFrameCount();
            if (revealInReady)
                load.Reveal();
        };
        const EventSubscription revealedSub = load.Revealed() += [&] { revealedFrame = m_Application->GetFrameCount(); };
        ASSERT_TRUE(TickUntil([&] { return revealedFrame.has_value(); })) << scene;
        ASSERT_TRUE(readyFrame.has_value()) << scene;
        EXPECT_EQ(*revealedFrame, *readyFrame) << scene;
    }
}

TEST_F(SceneLoaderTest, ARevealOnACancelledLoadDoesNothing)
{
    SceneLoadOperation older =
        Loader().Load({.Scene = "Scenes/A.scene", .Reveal = SceneRevealPolicy::WhenGameReleases});
    bool olderReady = false;
    bool olderCancelled = false;
    bool olderRevealed = false;
    const EventSubscription olderReadySub = older.Ready() += [&] { olderReady = true; };
    const EventSubscription olderCancelledSub = older.Cancelled() += [&] { olderCancelled = true; };
    const EventSubscription olderRevealedSub = older.Revealed() += [&] { olderRevealed = true; };
    ASSERT_TRUE(TickUntil([&] { return olderReady; }));

    SceneLoadOperation newer =
        Loader().Load({.Scene = "Scenes/B.scene", .Reveal = SceneRevealPolicy::WhenGameReleases});
    bool newerReady = false;
    bool newerRevealed = false;
    const EventSubscription newerReadySub = newer.Ready() += [&] { newerReady = true; };
    const EventSubscription newerRevealedSub = newer.Revealed() += [&] { newerRevealed = true; };
    m_Application->Tick();
    ASSERT_TRUE(olderCancelled);
    older.Reveal();
    ASSERT_TRUE(TickUntil([&] { return newerReady; }));
    for (int frame = 0; frame < 3; ++frame)
        m_Application->Tick();
    EXPECT_FALSE(olderRevealed) << "a cancelled load never reveals";
    EXPECT_FALSE(newerRevealed) << "the older load's Reveal() does not release the newer load";
    EXPECT_TRUE(Holds("B"));
}

TEST_F(SceneLoaderTest, ALoadRequestedDuringAReadCancelsItBeforeTheClear)
{
    ASSERT_TRUE(Show("Scenes/A.scene"));
    bool cancelled = false;
    bool loadingInCancelled = false;
    bool olderRevealed = false;
    bool newerRevealed = false;
    WorkerHold hold(EngineCore::GetInstance().GetJobSystem());
    ASSERT_TRUE(hold.AllHeld());

    SceneLoadOperation older = Loader().Load({.Scene = "Scenes/B.scene"});
    const EventSubscription cancelledSub = older.Cancelled() += [&]
    {
        cancelled = true;
        loadingInCancelled = Loader().IsLoading();
    };
    const EventSubscription olderSub = older.Revealed() += [&] { olderRevealed = true; };
    SceneLoaderTestAccess::Tick(Loader(), Clock::now()); // the older read is queued behind the held workers
    SceneLoadOperation newer = Loader().Load({.Scene = "Scenes/C.scene"});
    const EventSubscription newerSub = newer.Revealed() += [&] { newerRevealed = true; };
    SceneLoaderTestAccess::Tick(Loader(), Clock::now());

    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(loadingInCancelled) << "the newer load is current when the older one raises Cancelled";
    EXPECT_TRUE(Holds("A")) << "a load cancelled before its clear leaves the shown scene";
    hold.Release();
    ASSERT_TRUE(TickUntil([&] { return newerRevealed; }));
    EXPECT_FALSE(olderRevealed);
    EXPECT_TRUE(Holds("C"));
    EXPECT_FALSE(Holds("B"));
    EXPECT_FALSE(Holds("A"));
}

TEST_F(SceneLoaderTest, ALoadRequestedAfterTheClearCancelsTheLoadThatWaitsToReveal)
{
    Tracker::Category generation = LoadTracker().RegisterCategory({.Id = "test.generation", .Label = "Generating"});
    Tracker::WorkTicket step;
    bool instantiated = false;
    bool cancelled = false;
    bool loadingInCancelled = false;
    bool newerRevealed = false;
    SceneLoadOperation older = Loader().Load({.Scene = "Scenes/A.scene",
                                              .OnInstantiated = [&](SceneLoadInstantiated&)
                                              {
                                                  instantiated = true;
                                                  step = generation.BeginWork(LoadTracker().Intern("generate"), 1.0);
                                              }});
    // The game drops its pending work when its load is cancelled.
    const EventSubscription cancelledSub = older.Cancelled() += [&]
    {
        cancelled = true;
        loadingInCancelled = Loader().IsLoading();
        step = {};
    };
    ASSERT_TRUE(TickUntil([&] { return instantiated; }));
    m_Application->Tick();
    ASSERT_TRUE(Holds("A"));

    SceneLoadOperation newer = Loader().Load({.Scene = "Scenes/B.scene"});
    const EventSubscription newerSub = newer.Revealed() += [&] { newerRevealed = true; };
    ASSERT_TRUE(TickUntil([&] { return newerRevealed; }));
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(loadingInCancelled) << "the newer load is current when the older one raises Cancelled";
    EXPECT_TRUE(Holds("B"));
    EXPECT_FALSE(Holds("A"));
}

TEST_F(SceneLoaderTest, ACancelledLoadIsReleasedOnTheMainThreadWhenItsReadEnds)
{
    std::atomic<int> released{0};
    std::atomic<bool> offMainThread{false};
    {
        WorkerHold hold(EngineCore::GetInstance().GetJobSystem());
        ASSERT_TRUE(hold.AllHeld());
        SceneLoadOperation older = Loader().Load(ProbedRequest("Scenes/B.scene", released, offMainThread));
        SceneLoaderTestAccess::Tick(Loader(), Clock::now()); // the older read is queued behind the held workers
        SceneLoadOperation newer = Loader().Load({.Scene = "Scenes/C.scene"});
        SceneLoaderTestAccess::Tick(Loader(), Clock::now()); // the older load is cancelled with its read in flight
    }
    ASSERT_TRUE(TickUntil([&] { return released.load() == 1 && !Loader().IsLoading(); }));
    EXPECT_FALSE(offMainThread.load()) << "the loader keeps a cancelled load until its read ends, then releases it";
    EXPECT_TRUE(Holds("C"));
}

TEST_F(SceneLoaderTest, AHeldOperationKeepsNoSceneDocumentOnceTheSceneIsInstantiated)
{
    bool revealed = false;
    SceneLoadOperation load = Loader().Load({.Scene = "Scenes/A.scene"});
    const EventSubscription sub = load.Revealed() += [&] { revealed = true; };
    SceneLoaderTestAccess::Tick(Loader(), Clock::now()); // submits the read
    SceneLoadState* state = SceneLoaderTestAccess::Current(Loader());
    ASSERT_NE(state, nullptr);
    state->ReadTask.Wait();
    const std::weak_ptr<const Scene::SceneDocumentFiles> document = state->Read.Document.Files;
    ASSERT_FALSE(document.expired()) << "the read gives the loader a document";

    ASSERT_TRUE(TickUntil([&] { return revealed; }));
    EXPECT_TRUE(Holds("A"));
    EXPECT_TRUE(document.expired()) << "the operation still holds the scene document after the instantiate";
}

TEST_F(SceneLoaderTest, AHeldOperationKeepsNoSceneDocumentOnceItsLoadIsCancelledAfterItsRead)
{
    bool cancelled = false;
    SceneLoadOperation older = Loader().Load({.Scene = "Scenes/A.scene"});
    const EventSubscription cancelledSub = older.Cancelled() += [&] { cancelled = true; };
    SceneLoaderTestAccess::Tick(Loader(), Clock::now()); // submits the read
    SceneLoadState* state = SceneLoaderTestAccess::Current(Loader());
    ASSERT_NE(state, nullptr);
    state->ReadTask.Wait();
    const std::weak_ptr<const Scene::SceneDocumentFiles> document = state->Read.Document.Files;
    ASSERT_FALSE(document.expired()) << "the read gives the loader a document";

    SceneLoadOperation newer = Loader().Load({.Scene = "Scenes/B.scene"});
    SceneLoaderTestAccess::Tick(Loader(), Clock::now()); // the older load is cancelled before its instantiate
    ASSERT_TRUE(cancelled);
    EXPECT_TRUE(document.expired()) << "the cancelled operation still holds the scene document";
}

TEST_F(SceneLoaderTest, AHeldOperationKeepsNoSceneDocumentOnceItsCancelledReadEnds)
{
    bool cancelled = false;
    WorkerHold hold(EngineCore::GetInstance().GetJobSystem());
    ASSERT_TRUE(hold.AllHeld());
    SceneLoadOperation older = Loader().Load({.Scene = "Scenes/A.scene"});
    const EventSubscription cancelledSub = older.Cancelled() += [&] { cancelled = true; };
    SceneLoaderTestAccess::Tick(Loader(), Clock::now()); // the older read is queued behind the held workers
    SceneLoadState* state = SceneLoaderTestAccess::Current(Loader());
    ASSERT_NE(state, nullptr);
    SceneLoadOperation newer = Loader().Load({.Scene = "Scenes/B.scene"});
    SceneLoaderTestAccess::Tick(Loader(), Clock::now()); // the older load is cancelled with its read in flight
    ASSERT_TRUE(cancelled);
    hold.Release();
    state->ReadTask.Wait();
    const std::weak_ptr<const Scene::SceneDocumentFiles> document = state->Read.Document.Files;
    ASSERT_FALSE(document.expired()) << "the cancelled read runs to its end and gives the load a document";

    SceneLoaderTestAccess::Tick(Loader(), Clock::now()); // the loader sees that the read ended
    EXPECT_TRUE(document.expired()) << "the cancelled operation still holds the scene document after its read";
}

TEST_F(SceneLoaderTest, AHeldOperationKeepsNoSceneDocumentOnceShutdownWaitsForItsCancelledRead)
{
    WorkerHold hold(EngineCore::GetInstance().GetJobSystem());
    ASSERT_TRUE(hold.AllHeld());
    SceneLoadOperation older = Loader().Load({.Scene = "Scenes/A.scene"});
    SceneLoaderTestAccess::Tick(Loader(), Clock::now()); // the older read is queued behind the held workers
    SceneLoadState* state = SceneLoaderTestAccess::Current(Loader());
    ASSERT_NE(state, nullptr);
    SceneLoadOperation newer = Loader().Load({.Scene = "Scenes/B.scene"});
    SceneLoaderTestAccess::Tick(Loader(), Clock::now()); // the older load is cancelled with its read in flight
    ASSERT_EQ(state->Current, SceneLoadState::Step::Cancelled);
    hold.Release();
    SceneLoaderTestAccess::Shutdown(Loader()); // waits for both reads; no Tick sees the older read end
    ASSERT_TRUE(state->Read) << "the cancelled read succeeds, so it made a document";
    EXPECT_EQ(state->Read.Document.Files, nullptr) << "the cancelled operation still holds the scene document";
}

TEST_F(SceneLoaderTest, ShutdownWaitsForEveryReadInFlight)
{
    std::atomic<int> released{0};
    std::atomic<bool> offMainThread{false};
    WorkerHold hold(EngineCore::GetInstance().GetJobSystem());
    ASSERT_TRUE(hold.AllHeld());
    {
        SceneLoadOperation older = Loader().Load(ProbedRequest("Scenes/B.scene", released, offMainThread));
        SceneLoaderTestAccess::Tick(Loader(), Clock::now());
        SceneLoadOperation newer = Loader().Load(ProbedRequest("Scenes/C.scene", released, offMainThread));
        SceneLoaderTestAccess::Tick(Loader(), Clock::now()); // both reads are queued; the older load is cancelled
    }
    std::thread releaser(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            hold.Release();
        });
    SceneLoaderTestAccess::Shutdown(Loader());
    const int releasedAtShutdown = released.load();
    releaser.join();
    EXPECT_EQ(releasedAtShutdown, 2) << "Shutdown waits for the cancelled read and the current one";
    EXPECT_FALSE(offMainThread.load());
    EXPECT_FALSE(Loader().IsLoading());
}

TEST_F(SceneLoaderTest, ALoadSetsTheResolveSliceUntilItIsRevealed)
{
    using Engine::Renderer::RenderingLoop;
    constexpr std::chrono::milliseconds kInstantiatedWork{20};
    RenderingLoop* loop = EngineCore::GetInstance().GetRenderingLoop();
    ASSERT_NE(loop, nullptr);
    Tracker::Category generation = LoadTracker().RegisterCategory({.Id = "test.generation", .Label = "Generating"});
    Tracker::WorkTicket step;
    bool instantiated = false;
    bool revealed = false;
    SceneLoadOperation load = Loader().Load({.Scene = "Scenes/A.scene",
                                             .OnInstantiated = [&](SceneLoadInstantiated&)
                                             {
                                                 instantiated = true;
                                                 step = generation.BeginWork(LoadTracker().Intern("generate"), 1.0);
                                                 std::this_thread::sleep_for(kInstantiatedWork);
                                             }});
    const EventSubscription sub = load.Revealed() += [&] { revealed = true; };
    ASSERT_TRUE(TickUntil([&] { return instantiated; }));
    EXPECT_LE(SceneLoaderTestAccess::ResolveSlice(*loop), SceneLoaderTestAccess::kFrameBound - kInstantiatedWork)
        << "the instantiate frame's slice leaves out the load work that frame did";
    m_Application->Tick();
    m_Application->Tick();
    EXPECT_GT(SceneLoaderTestAccess::ResolveSlice(*loop), RenderingLoop::kResolveFrameSlice)
        << "the frame bound leaves the resolve slice more than its default while a load runs";
    EXPECT_LE(SceneLoaderTestAccess::ResolveSlice(*loop), SceneLoaderTestAccess::kFrameBound);

    step.Complete();
    ASSERT_TRUE(TickUntil([&] { return revealed; }));
    EXPECT_EQ(SceneLoaderTestAccess::ResolveSlice(*loop), RenderingLoop::kResolveFrameSlice);
}

TEST_F(SceneLoaderTest, AGpuBoundFrameGivesItsWaitToTheResolveSlice)
{
    using Engine::Renderer::RenderingLoop;
    constexpr std::chrono::milliseconds kFrame{100};
    constexpr std::chrono::milliseconds kGpuWait{97};
    RenderingLoop* loop = EngineCore::GetInstance().GetRenderingLoop();
    ASSERT_NE(loop, nullptr);
    Tracker::Category generation = LoadTracker().RegisterCategory({.Id = "test.generation", .Label = "Generating"});
    Tracker::WorkTicket step;
    bool instantiated = false;
    SceneLoadOperation load = Loader().Load(
        {.Scene = "Scenes/A.scene",
         .OnInstantiated = [&](SceneLoadInstantiated&)
         {
             instantiated = true;
             step = generation.BeginWork(LoadTracker().Intern("generate"), 1.0);
         }});
    ASSERT_TRUE(TickUntil([&] { return instantiated; }));
    m_Application->Tick(); // the tracker's snapshot now holds the generation work, so the load stays open

    // Frames of 100 ms, longer than the frame bound. Of each, the GPU wait takes 97 ms.
    const Clock::time_point start = Clock::now();
    SceneLoaderTestAccess::Tick(Loader(), start, kGpuWait);
    SceneLoaderTestAccess::Tick(Loader(), start + kFrame, kGpuWait);
    EXPECT_GE(SceneLoaderTestAccess::ResolveSlice(*loop), SceneLoaderTestAccess::kFrameBound / 2)
        << "the main thread waits for the GPU for most of the frame: the slice uses that time";

    // The same frame without a GPU wait is bound by its main-thread work: the slice stays at its minimum.
    SceneLoaderTestAccess::Tick(Loader(), start + 2 * kFrame);
    EXPECT_EQ(SceneLoaderTestAccess::ResolveSlice(*loop), RenderingLoop::kResolveFrameSlice);

    step.Complete();
}

TEST_F(SceneLoaderTest, AnAdditiveLoadAddsItsEntitiesAndKeepsTheWorldAndItsTick)
{
    ASSERT_TRUE(Show("Scenes/A.scene"));
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    ASSERT_NE(world, nullptr);
    const std::size_t entitiesBefore = world->GetEntityCount();
    const uint64_t tickBefore = world->GetSimulationClock().GetTick();
    bool revealed = false;
    SceneLoadOperation load = Loader().Load({.Scene = "Scenes/Town.scene", .Mode = SceneLoadMode::Additive});
    const EventSubscription sub = load.Revealed() += [&] { revealed = true; };
    ASSERT_TRUE(TickUntil([&] { return revealed; }));
    EXPECT_TRUE(Holds("A")) << "an additive load keeps the entities that the world holds";
    EXPECT_TRUE(Holds("Town gate"));
    EXPECT_TRUE(Holds("Town well"));
    EXPECT_EQ(world->GetEntityCount(), entitiesBefore + 2);
    const uint64_t tick = world->GetSimulationClock().GetTick();
    EXPECT_GE(tick, tickBefore) << "the world's clock runs on through an additive load";
    EXPECT_LT(tick, kSavedTick) << "an additive load keeps the world's tick, not the tick the scene was saved at";
}

TEST_F(SceneLoaderTest, AnAdditiveLoadIsReadyToShowOnlyOnceTheTrackerIsReady)
{
    ASSERT_TRUE(Show("Scenes/A.scene"));
    Tracker::Category generation = LoadTracker().RegisterCategory({.Id = "test.generation", .Label = "Generating"});
    Tracker::WorkTicket step;
    std::optional<uint64_t> instantiatedFrame;
    std::optional<uint64_t> readyFrame;
    std::optional<uint64_t> revealedFrame;
    SceneLoadOperation load = Loader().Load({.Scene = "Scenes/Town.scene",
                                             .Mode = SceneLoadMode::Additive,
                                             .OnInstantiated = [&](SceneLoadInstantiated&)
                                             {
                                                 instantiatedFrame = m_Application->GetFrameCount();
                                                 step = generation.BeginWork(LoadTracker().Intern("generate"), 1.0);
                                             }});
    const EventSubscription readySub = load.Ready() += [&] { readyFrame = m_Application->GetFrameCount(); };
    const EventSubscription revealedSub = load.Revealed() += [&] { revealedFrame = m_Application->GetFrameCount(); };
    ASSERT_TRUE(TickUntil([&] { return instantiatedFrame.has_value(); }));
    for (int frame = 0; frame < 5; ++frame)
        m_Application->Tick();
    EXPECT_TRUE(Holds("Town gate")) << "the entities are in the world before the load is ready";
    EXPECT_FALSE(load.IsReadyToShow()) << "a ticket begun in OnInstantiated holds an additive load too";
    EXPECT_FALSE(readyFrame.has_value());
    EXPECT_FALSE(revealedFrame.has_value());

    step.Complete();
    ASSERT_TRUE(TickUntil([&] { return revealedFrame.has_value(); }));
    EXPECT_TRUE(load.IsReadyToShow());
    ASSERT_TRUE(readyFrame.has_value());
    EXPECT_EQ(*readyFrame, *revealedFrame) << "an AtReady load is revealed in the frame it is ready";
    EXPECT_GT(*readyFrame, *instantiatedFrame);
}

TEST_F(SceneLoaderTest, AnAdditiveLoadFailsWhileAnotherLoadIsInProgress)
{
    ASSERT_TRUE(Show("Scenes/A.scene"));
    Tracker::Category generation = LoadTracker().RegisterCategory({.Id = "test.generation", .Label = "Generating"});
    Tracker::WorkTicket step;
    bool singleInstantiated = false;
    bool singleRevealed = false;
    SceneLoadOperation single = Loader().Load({.Scene = "Scenes/B.scene",
                                               .OnInstantiated = [&](SceneLoadInstantiated&)
                                               {
                                                   singleInstantiated = true;
                                                   step = generation.BeginWork(LoadTracker().Intern("generate"), 1.0);
                                               }});
    const EventSubscription singleSub = single.Revealed() += [&] { singleRevealed = true; };
    ASSERT_TRUE(TickUntil([&] { return singleInstantiated; }));

    std::optional<std::string> failure;
    bool loadingInFailed = false;
    SceneLoadOperation refused = Loader().Load({.Scene = "Scenes/Town.scene", .Mode = SceneLoadMode::Additive});
    const EventSubscription refusedSub = refused.Failed() += [&](std::string_view reason)
    {
        failure = std::string(reason);
        loadingInFailed = Loader().IsLoading();
    };
    m_Application->Tick();
    ASSERT_TRUE(failure.has_value()) << "an additive load fails at the next frame boundary while a load runs";
    EXPECT_NE(failure->find("in progress"), std::string::npos) << *failure;
    EXPECT_TRUE(loadingInFailed) << "the load in progress is current when the additive one raises Failed";
    step.Complete();
    ASSERT_TRUE(TickUntil([&] { return singleRevealed; })) << "the load in progress goes on";
    EXPECT_TRUE(Holds("B"));
    EXPECT_FALSE(Holds("Town gate")) << "the refused load does not touch the world";

    // Of two additive loads requested in one frame, the first starts and the second fails.
    bool firstRevealed = false;
    bool secondFailed = false;
    SceneLoadOperation first = Loader().Load({.Scene = "Scenes/Town.scene", .Mode = SceneLoadMode::Additive});
    SceneLoadOperation second = Loader().Load({.Scene = "Scenes/C.scene", .Mode = SceneLoadMode::Additive});
    const EventSubscription firstSub = first.Revealed() += [&] { firstRevealed = true; };
    const EventSubscription secondSub = second.Failed() += [&](std::string_view) { secondFailed = true; };
    ASSERT_TRUE(TickUntil([&] { return firstRevealed; }));
    EXPECT_TRUE(secondFailed);
    EXPECT_TRUE(Holds("B"));
    EXPECT_TRUE(Holds("Town gate"));
    EXPECT_FALSE(Holds("C"));
}

TEST_F(SceneLoaderTest, ASingleLoadCancelsAnAdditiveLoadAndClearsItsEntities)
{
    ASSERT_TRUE(Show("Scenes/A.scene"));
    Tracker::Category generation = LoadTracker().RegisterCategory({.Id = "test.generation", .Label = "Generating"});
    Tracker::WorkTicket step;
    bool instantiated = false;
    bool cancelled = false;
    bool loadingInCancelled = false;
    bool singleRevealed = false;
    SceneLoadOperation additive = Loader().Load({.Scene = "Scenes/Town.scene",
                                                 .Mode = SceneLoadMode::Additive,
                                                 .OnInstantiated = [&](SceneLoadInstantiated&)
                                                 {
                                                     instantiated = true;
                                                     step = generation.BeginWork(LoadTracker().Intern("generate"), 1.0);
                                                 }});
    const EventSubscription cancelledSub = additive.Cancelled() += [&]
    {
        cancelled = true;
        loadingInCancelled = Loader().IsLoading();
        step = {};
    };
    ASSERT_TRUE(TickUntil([&] { return instantiated; }));
    ASSERT_TRUE(Holds("A"));
    ASSERT_TRUE(Holds("Town gate"));

    SceneLoadOperation single = Loader().Load({.Scene = "Scenes/B.scene"});
    const EventSubscription singleSub = single.Revealed() += [&] { singleRevealed = true; };
    ASSERT_TRUE(TickUntil([&] { return singleRevealed; }));
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(loadingInCancelled) << "the single load is current when the additive one raises Cancelled";
    EXPECT_TRUE(Holds("B"));
    EXPECT_FALSE(Holds("A"));
    EXPECT_FALSE(Holds("Town gate")) << "the single load clears the entities that the additive load added";
}

TEST_F(SceneLoaderTest, AnAdditiveLoadLeavesTheResolveSliceAtItsDefault)
{
    using Engine::Renderer::RenderingLoop;
    RenderingLoop* loop = EngineCore::GetInstance().GetRenderingLoop();
    ASSERT_NE(loop, nullptr);
    ASSERT_TRUE(Show("Scenes/A.scene"));
    ASSERT_EQ(SceneLoaderTestAccess::ResolveSlice(*loop), RenderingLoop::kResolveFrameSlice);
    Tracker::Category generation = LoadTracker().RegisterCategory({.Id = "test.generation", .Label = "Generating"});
    Tracker::WorkTicket step;
    bool instantiated = false;
    bool revealed = false;
    SceneLoadOperation load = Loader().Load({.Scene = "Scenes/Town.scene",
                                             .Mode = SceneLoadMode::Additive,
                                             .OnInstantiated = [&](SceneLoadInstantiated&)
                                             {
                                                 instantiated = true;
                                                 step = generation.BeginWork(LoadTracker().Intern("generate"), 1.0);
                                             }});
    const EventSubscription sub = load.Revealed() += [&] { revealed = true; };
    ASSERT_TRUE(TickUntil([&] { return instantiated; }));
    m_Application->Tick();
    m_Application->Tick();
    EXPECT_TRUE(Loader().IsLoading());
    EXPECT_EQ(SceneLoaderTestAccess::ResolveSlice(*loop), RenderingLoop::kResolveFrameSlice)
        << "an additive load runs during play, so the frame keeps its default resolve slice";
    step.Complete();
    ASSERT_TRUE(TickUntil([&] { return revealed; }));
}

TEST_F(SceneLoaderTest, OnlyASingleLoadIsReplacingTheShownScene)
{
    ASSERT_TRUE(Show("Scenes/A.scene"));
    Tracker::Category generation = LoadTracker().RegisterCategory({.Id = "test.generation", .Label = "Generating"});
    for (const auto& [scene, mode] :
         {std::pair{"Scenes/Town.scene", SceneLoadMode::Additive}, std::pair{"Scenes/B.scene", SceneLoadMode::Single}})
    {
        const bool replaces = mode == SceneLoadMode::Single;
        Tracker::WorkTicket step;
        bool instantiated = false;
        bool revealed = false;
        SceneLoadOperation load = Loader().Load({.Scene = scene,
                                                 .Mode = mode,
                                                 .OnInstantiated = [&](SceneLoadInstantiated&)
                                                 {
                                                     instantiated = true;
                                                     step = generation.BeginWork(LoadTracker().Intern("generate"), 1.0);
                                                 }});
        const EventSubscription sub = load.Revealed() += [&] { revealed = true; };
        EXPECT_TRUE(Loader().IsLoading()) << scene << " is queued";
        EXPECT_EQ(Loader().IsReplacingScene(), replaces) << scene << " is queued: only a single load replaces the scene";
        ASSERT_TRUE(TickUntil([&] { return instantiated; }));
        EXPECT_TRUE(Loader().IsLoading()) << scene << " waits for its ticket";
        EXPECT_EQ(Loader().IsReplacingScene(), replaces)
            << scene << " waits for its ticket: only a single load replaces the scene";
        step.Complete();
        ASSERT_TRUE(TickUntil([&] { return revealed; }));
        EXPECT_FALSE(Loader().IsLoading());
        EXPECT_FALSE(Loader().IsReplacingScene());
    }
}

TEST_F(SceneLoaderTest, ADefaultConstructedOperationRaisesNothing)
{
    SceneLoadOperation none;
    int raised = 0;
    const EventSubscription revealedSub = none.Revealed() += [&] { ++raised; };
    const EventSubscription cancelledSub = none.Cancelled() += [&] { ++raised; };
    const EventSubscription failedSub = none.Failed() += [&](std::string_view) { ++raised; };
    ASSERT_TRUE(Show("Scenes/A.scene"));
    EXPECT_EQ(raised, 0);
}

TEST_F(SceneLoaderTest, APausingLoadPausesTheShownAndTheNewCamerasAndResumesThemBeforeReady)
{
    ASSERT_TRUE(Show("Scenes/CamA.scene"));
    ASSERT_EQ(CameraPaused("A camera"), false);
    std::optional<bool> newPausedAtInstantiated;
    std::optional<bool> newPausedAtReady;
    SceneLoadOperation load = Loader().Load(
        {.Scene = "Scenes/CamB.scene",
         .OnInstantiated = [&](SceneLoadInstantiated&) { newPausedAtInstantiated = CameraPaused("B camera"); },
         .PauseWorldDrawing = true});
    const EventSubscription readySub = load.Ready() += [&] { newPausedAtReady = CameraPaused("B camera"); };
    m_Application->Tick();
    EXPECT_EQ(CameraPaused("A camera"), true) << "the load pauses the shown scene's cameras in its first frame";
    ASSERT_TRUE(TickUntil([&] { return newPausedAtReady.has_value(); }));
    EXPECT_EQ(newPausedAtInstantiated, true) << "OnInstantiated sees the new scene's cameras paused";
    EXPECT_EQ(newPausedAtReady, false) << "the world draws again before Ready";
}

TEST_F(SceneLoaderTest, TheLoaderResumesOnlyTheCamerasThatItPaused)
{
    // A camera that the game paused and a camera that the scene saves paused stay paused.
    ASSERT_TRUE(Show("Scenes/CamA.scene"));
    bool shown = false;
    SceneLoadOperation added = Loader().Load({.Scene = "Scenes/CamB.scene", .Mode = SceneLoadMode::Additive});
    const EventSubscription addedSub = added.Revealed() += [&] { shown = true; };
    ASSERT_TRUE(TickUntil([&] { return shown; }));
    SetCameraPaused("A camera", true);

    bool ready = false;
    std::optional<bool> outpostPausedAtInstantiated;
    SceneLoadOperation load = Loader().Load(
        {.Scene = "Scenes/Outpost.scene",
         .Mode = SceneLoadMode::Additive,
         .OnInstantiated = [&](SceneLoadInstantiated&) { outpostPausedAtInstantiated = CameraPaused("Outpost camera"); },
         .PauseWorldDrawing = true});
    const EventSubscription readySub = load.Ready() += [&] { ready = true; };
    m_Application->Tick();
    EXPECT_EQ(CameraPaused("B camera"), true);
    ASSERT_TRUE(TickUntil([&] { return ready; }));
    EXPECT_EQ(outpostPausedAtInstantiated, true);
    EXPECT_EQ(CameraPaused("B camera"), false);
    EXPECT_EQ(CameraPaused("Outpost camera"), false);
    EXPECT_EQ(CameraPaused("A camera"), true) << "the game paused it before the load";
    EXPECT_EQ(CameraPaused("B map"), true) << "the scene saves it paused";
}

TEST_F(SceneLoaderTest, APausingLoadPausesACameraThatOnInstantiatedAddsAndResumesItBeforeReady)
{
    // Scenes/A.scene has no camera: the handler adds one, as the Player does for its startup scene.
    const auto addedCameraPaused = []
    {
        std::optional<bool> paused;
        EngineCore::GetInstance().GetPrimaryWorld()->Query<ECS::Read<Components::Camera>>().Each(
            [&paused](const Components::Camera& camera) { paused = camera.WorldDrawingPaused; });
        return paused;
    };
    bool instantiated = false;
    std::optional<bool> pausedAtReady;
    SceneLoadOperation load = Loader().Load({.Scene = "Scenes/A.scene",
                                             .OnInstantiated =
                                                 [this, &instantiated](SceneLoadInstantiated& loaded)
                                             {
                                                 m_Host->EnsureCamera(loaded.World());
                                                 instantiated = true;
                                             },
                                             .PauseWorldDrawing = true});
    const EventSubscription readySub =
        load.Ready() += [&pausedAtReady, &addedCameraPaused] { pausedAtReady = addedCameraPaused(); };
    ASSERT_TRUE(TickUntil([&instantiated] { return instantiated; }));
    EXPECT_EQ(addedCameraPaused(), true) << "the frame after the instantiate draws no world";
    ASSERT_TRUE(TickUntil([&pausedAtReady] { return pausedAtReady.has_value(); }));
    EXPECT_EQ(pausedAtReady, false) << "the world draws again before Ready";
}

TEST_F(SceneLoaderTest, APausingLoadPausesACameraThatAUserSystemCreatesInOnStartAndResumesItWithTheSceneCameras)
{
    // The first load of the session starts the user systems, after OnInstantiated.
    CameraCreatingUserSystem cameraSystem;
    ASSERT_TRUE(NativeScripting::RegisterUserSystem(&cameraSystem));
    std::optional<bool> startedPausedAtReady;
    std::optional<bool> scenePausedAtReady;
    SceneLoadOperation load = Loader().Load({.Scene = "Scenes/CamA.scene", .PauseWorldDrawing = true});
    const EventSubscription readySub = load.Ready() += [&startedPausedAtReady, &scenePausedAtReady]
    {
        startedPausedAtReady = CameraPaused("Started camera");
        scenePausedAtReady = CameraPaused("A camera");
    };
    const bool started = TickUntil([&cameraSystem] { return cameraSystem.Started; });
    const std::optional<bool> startedPausedAfterStart = CameraPaused("Started camera");
    const bool ready = TickUntil([&startedPausedAtReady] { return startedPausedAtReady.has_value(); });
    // The registry keeps a raw pointer: stop and drop the system before it goes out of scope.
    if (ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld())
        NativeScripting::StopUserSystems(*world);
    NativeScripting::ClearUserSystems();
    ASSERT_TRUE(started);
    EXPECT_EQ(startedPausedAfterStart, true) << "the frame after the instantiate draws no world";
    ASSERT_TRUE(ready);
    EXPECT_EQ(startedPausedAtReady, false) << "the world draws again before Ready";
    EXPECT_EQ(scenePausedAtReady, false);
}

TEST_F(SceneLoaderTest, AfterTheResumeAChainThatAsksTwoFramesLateHoldsReadyAndTheReveal)
{
    Tracker::Category builds = LoadTracker().RegisterCategory({.Id = "test.builds", .Label = "Building"});
    SceneLoadOperation load = Loader().Load({.Scene = "Scenes/CamB.scene", .PauseWorldDrawing = true});
    std::optional<uint64_t> readyFrame;
    std::optional<uint64_t> revealedFrame;
    const EventSubscription readySub = load.Ready() += [&] { readyFrame = m_Application->GetFrameCount(); };
    const EventSubscription revealedSub = load.Revealed() += [&] { revealedFrame = m_Application->GetFrameCount(); };
    ASSERT_TRUE(TickUntil([&] { return CameraPaused("B camera") == false; }));
    ASSERT_FALSE(readyFrame.has_value()) << "the frame of the resume is not ready";

    // From the resume to Ready: the bar does not go down and stays under 1, the estimate does not
    // go up, and the operation is ready only once Ready is raised.
    float lastProgress = load.Progress();
    double lastEstimate = load.EstimatedSecondsRemaining();
    const auto frame = [&]
    {
        m_Application->Tick();
        EXPECT_GE(load.Progress(), lastProgress);
        EXPECT_LE(load.EstimatedSecondsRemaining(), lastEstimate);
        EXPECT_EQ(load.IsReadyToShow(), readyFrame.has_value());
        EXPECT_EQ(load.Progress() == 1.0f, readyFrame.has_value());
        lastProgress = load.Progress();
        lastEstimate = load.EstimatedSecondsRemaining();
    };
    // The first live frame asks for a pipeline, and the chain asks for the next one two frames after
    // the first is built.
    Tracker::WorkTicket first = builds.BeginWork(LoadTracker().Intern("first"), 1.0);
    frame();
    first.Complete();
    frame();
    frame();
    Tracker::WorkTicket second = builds.BeginWork(LoadTracker().Intern("second"), 1.0);
    frame();
    EXPECT_FALSE(readyFrame.has_value()) << "two ready frames between the builds do not make the load ready";
    EXPECT_FALSE(revealedFrame.has_value());

    second.Complete();
    int framesAfterBuild = 0;
    for (; framesAfterBuild < SceneLoader::kReadyFramesAfterResume + 2 && !readyFrame; ++framesAfterBuild)
        frame();
    ASSERT_TRUE(readyFrame.has_value());
    EXPECT_EQ(framesAfterBuild, SceneLoader::kReadyFramesAfterResume) << "Ready comes with the last of the ready frames";
    EXPECT_EQ(revealedFrame, readyFrame) << "SceneRevealPolicy::AtReady reveals in the frame of Ready";
}

TEST_F(SceneLoaderTest, ANewerPausingLoadTakesOverThePauseFromALoadThatResumed)
{
    bool olderReady = false;
    bool olderCancelled = false;
    SceneLoadOperation older = Loader().Load({.Scene = "Scenes/CamB.scene", .PauseWorldDrawing = true});
    const EventSubscription olderReadySub = older.Ready() += [&] { olderReady = true; };
    const EventSubscription olderCancelledSub = older.Cancelled() += [&] { olderCancelled = true; };
    ASSERT_TRUE(TickUntil([&] { return CameraPaused("B camera") == false; }));

    std::optional<bool> newPausedAtReady;
    SceneLoadOperation newer = Loader().Load({.Scene = "Scenes/CamC.scene", .PauseWorldDrawing = true});
    const EventSubscription newerReadySub = newer.Ready() += [&] { newPausedAtReady = CameraPaused("C camera"); };
    m_Application->Tick();
    EXPECT_TRUE(olderCancelled);
    EXPECT_EQ(CameraPaused("B camera"), true) << "the newer load pauses the world that the older load resumed";
    ASSERT_TRUE(TickUntil([&] { return newPausedAtReady.has_value(); }));
    EXPECT_FALSE(olderReady);
    EXPECT_EQ(newPausedAtReady, false) << "the newer load resumes the world";
}

TEST_F(SceneLoaderTest, AFailedPausingLoadResumesTheCamerasThatItPausedBeforeItRaisesFailed)
{
    // The Failed handler sees the world draw, so a game can pause it again behind its error screen.
    ASSERT_TRUE(Show("Scenes/CamA.scene"));
    std::optional<bool> pausedAtReadFailed;
    SceneLoadOperation missing = Loader().Load({.Scene = "Scenes/Missing.scene", .PauseWorldDrawing = true});
    const EventSubscription missingSub =
        missing.Failed() += [&](std::string_view) { pausedAtReadFailed = CameraPaused("A camera"); };
    m_Application->Tick();
    ASSERT_EQ(CameraPaused("A camera"), true);
    ASSERT_TRUE(TickUntil([&] { return pausedAtReadFailed.has_value(); }));
    EXPECT_EQ(pausedAtReadFailed, false) << "a read that fails resumes the shown scene";

    std::optional<bool> pausedAtRefused;
    SceneLoadOperation load = Loader().Load(
        {.Scene = "Scenes/CamB.scene",
         .OnInstantiated = [](SceneLoadInstantiated& instantiated) { instantiated.Fail("the game refuses it"); },
         .PauseWorldDrawing = true});
    const EventSubscription refusedSub =
        load.Failed() += [&](std::string_view) { pausedAtRefused = CameraPaused("B camera"); };
    ASSERT_TRUE(TickUntil([&] { return pausedAtRefused.has_value(); }));
    EXPECT_EQ(pausedAtRefused, false) << "a refused scene draws again";
    EXPECT_EQ(CameraPaused("B map"), true) << "the scene saves it paused";
}

TEST_F(SceneLoaderTest, ANewerPausingLoadTakesOverThePausesOfTheLoadThatItReplaces)
{
    ASSERT_TRUE(Show("Scenes/CamA.scene"));
    bool olderCancelled = false;
    SceneLoadOperation older = Loader().Load({.Scene = "Scenes/CamB.scene", .PauseWorldDrawing = true});
    const EventSubscription olderSub = older.Cancelled() += [&] { olderCancelled = true; };
    m_Application->Tick();
    ASSERT_EQ(CameraPaused("A camera"), true);

    bool drew = false;
    std::optional<bool> pausedAtFailed;
    SceneLoadOperation newer = Loader().Load({.Scene = "Scenes/Missing.scene", .PauseWorldDrawing = true});
    const EventSubscription newerSub =
        newer.Failed() += [&](std::string_view) { pausedAtFailed = CameraPaused("A camera"); };
    ASSERT_TRUE(TickUntil(
        [&]
        {
            drew = drew || (!pausedAtFailed && CameraPaused("A camera") == false);
            return pausedAtFailed.has_value();
        }));
    EXPECT_TRUE(olderCancelled);
    EXPECT_FALSE(drew) << "no frame draws the world between the two loads";
    EXPECT_EQ(pausedAtFailed, false) << "the newer load resumes the pauses that it took over when it fails";
}

TEST_F(SceneLoaderTest, ANewerLoadWithoutTheSwitchResumesThePausesOfTheLoadThatItReplaces)
{
    ASSERT_TRUE(Show("Scenes/CamA.scene"));
    bool olderCancelled = false;
    SceneLoadOperation older = Loader().Load({.Scene = "Scenes/CamB.scene", .PauseWorldDrawing = true});
    const EventSubscription olderSub = older.Cancelled() += [&] { olderCancelled = true; };
    m_Application->Tick();
    ASSERT_EQ(CameraPaused("A camera"), true);

    bool newerFailed = false;
    SceneLoadOperation newer = Loader().Load({.Scene = "Scenes/Missing.scene"});
    const EventSubscription newerSub = newer.Failed() += [&](std::string_view) { newerFailed = true; };
    m_Application->Tick();
    EXPECT_TRUE(olderCancelled);
    EXPECT_EQ(CameraPaused("A camera"), false) << "the newer load resumes the world in the frame that it starts";
    ASSERT_TRUE(TickUntil([&] { return newerFailed; }));
    EXPECT_EQ(CameraPaused("A camera"), false);
}

TEST_F(SceneLoaderTest, WithoutPauseWorldDrawingALoadLeavesTheCamerasAndIsReadyAtTheFirstReadyFrame)
{
    ASSERT_TRUE(Show("Scenes/CamA.scene"));
    std::optional<uint64_t> instantiatedFrame;
    std::optional<uint64_t> readyFrame;
    SceneLoadOperation load = Loader().Load(
        {.Scene = "Scenes/CamC.scene",
         .OnInstantiated = [&](SceneLoadInstantiated&) { instantiatedFrame = m_Application->GetFrameCount(); }});
    const EventSubscription readySub = load.Ready() += [&] { readyFrame = m_Application->GetFrameCount(); };
    bool paused = false;
    ASSERT_TRUE(TickUntil(
        [&]
        {
            paused = paused || CameraPaused("A camera") == true || CameraPaused("C camera") == true;
            return readyFrame.has_value();
        }));
    EXPECT_FALSE(paused);
    ASSERT_TRUE(instantiatedFrame.has_value());
    EXPECT_EQ(*readyFrame, *instantiatedFrame + 1) << "Ready comes with the first snapshot after the instantiate";
}
