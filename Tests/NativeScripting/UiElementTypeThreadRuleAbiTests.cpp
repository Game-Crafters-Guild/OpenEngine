// The UI element-type registry's thread rules at the real exports: a claim or a verdict is admitted only on the
// thread running a registration window, whatever that thread is, and the unload sweep only on the main thread
// outside any job body, whichever thread runs the body. The engine is a host's (the NativeAOT Player's scripts
// configuration), so a window is queued for the main-thread pump these arms run. This binary starts no CLR, so
// the callbacks installed here are the process's first and only ones.

#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Build/PackagedGameLayout.h"
#include "EngineLogCapture.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scripting/ManagedElementTypes.h"
#include "Scripting/ScriptingABI.h"
#include "TestTempDir.h"
#include "UI/Registration/ElementRegistration.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using GameEngine::EngineCore;
using GameEngine::Scripting::ManagedElementTypes;
using JobSystem::JobCounter;
using JobSystem::WorkStealingThreadPool;

// What the registration window runs, as the managed binding's PerformRegistrations does.
std::function<void()> g_Window;

intptr_t GE_CDECL CreateNothing(uint64_t, uint64_t) { return 0; }
void GE_CDECL ApplyNothing(intptr_t, const char*, const char*) {}
void GE_CDECL ReleaseNothing(intptr_t) {}
void GE_CDECL PerformRegistrations()
{
    if (g_Window)
        g_Window();
}

// Unique per call: a tag another owner holds is refused for that reason, not the one under test.
std::string FreshTag(const char* stem)
{
    static int s_Counter = 0;
    return std::string("ge-thread-rule-") + stem + "-" + std::to_string(++s_Counter);
}

// A pool whose one worker is held in a job of its own until Release, so a job tagged to a counter the main thread
// waits on runs on the main thread, as a participating waiter's job.
class HeldPool
{
  public:
    HeldPool()
    {
        m_Pool.EnqueueWork([this] {
            m_Held.store(true);
            m_Release.get_future().wait();
        });
        while (!m_Held.load())
            std::this_thread::yield();
    }
    ~HeldPool() { Release(); }
    HeldPool(const HeldPool&) = delete;
    HeldPool& operator=(const HeldPool&) = delete;

    WorkStealingThreadPool& Pool() { return m_Pool; }
    void Release()
    {
        if (!m_Released)
            m_Release.set_value();
        m_Released = true;
    }

  private:
    WorkStealingThreadPool m_Pool{1};
    std::atomic<bool> m_Held{false};
    std::promise<void> m_Release;
    bool m_Released = false;
};

// Runs `body` as a job on a pool worker and waits for it.
void RunOnAWorker(const std::function<void()>& body)
{
    WorkStealingThreadPool pool(1);
    std::promise<void> done;
    pool.EnqueueWork([&] {
        body();
        done.set_value();
    });
    done.get_future().wait();
}

// Runs `body` on the main thread as a job a participating Wait runs, while the pool's only worker is held.
void RunAsAWaitersJob(const std::function<void()>& body)
{
    HeldPool held;
    JobCounter counter;
    held.Pool().Run([&] { body(); }, counter);
    held.Pool().Wait(counter);
    held.Release();
}

// Runs `body` on the main thread as the one ParallelFor unit, which the forking caller runs itself.
void RunAsACallersUnit(const std::function<void()>& body)
{
    WorkStealingThreadPool pool(2);
    JobSystem::ParallelFor(&pool, 1, [&](size_t) { body(); });
}

class UiElementTypeThreadRuleTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        EngineCore::GetInstance().Shutdown();
        ASSERT_TRUE(Initialize());
        // First-wins and process-wide, installed once with the marshaller and the predicate the ABI backs.
        static const GE_Result s_Installed =
            GE_UI_SetElementTypeCallbacks(&CreateNothing, &ApplyNothing, &ReleaseNothing, &PerformRegistrations);
        ASSERT_EQ(s_Installed, GE_Result_Ok);
        ASSERT_EQ(ManagedElementTypes::Instance().Callbacks().PerformRegistrations, &PerformRegistrations);
        ASSERT_EQ(GE_UI_AcquireTypeOwner(&m_Owner), GE_Result_Ok);
    }

    void TearDown() override
    {
        g_Window = {};
        EngineCore::GetInstance().Shutdown();
    }

    bool Initialize()
    {
        GameEngine::ApplicationConfig config{};
        config.WorkspaceDirectory = m_Workspace.Path().string();
        config.AssetDirectory = "Assets";
        EngineCore::GetInstance().SetScriptsConfig(
            GameEngine::BuildPlayerScriptsConfig(GameEngine::PackagedGameLayout{}, /*useNativeAOT=*/true));
        return EngineCore::GetInstance().Initialize(config);
    }

    static void Pump() { EngineCore::GetInstance().GetScriptManager().ProcessMainThreadTasks(); }

    // Claims `tag` for the owner from a window, the admitted path.
    uint64_t ClaimInAWindow(const std::string& tag)
    {
        uint64_t tagId = 0;
        g_Window = [&] { EXPECT_EQ(GE_UI_RegisterElementType(m_Owner, tag.c_str(), &tagId), GE_Result_Ok); };
        EXPECT_EQ(GE_UI_RequestTypeRegistrationWindow(), GE_Result_Ok);
        Pump();
        g_Window = {};
        return tagId;
    }

    uint64_t m_Owner = 0;

  private:
    GameEngine::TestUtils::ScopedTempDir m_Workspace{
        GameEngine::TestUtils::MakeUniqueTempDirectory("UiElementTypeThreadRule")};
};
} // namespace

// Outside a window a claim is refused on every thread and from every kind of body, the main thread's included,
// and the refusal names the window to use.
TEST_F(UiElementTypeThreadRuleTest, UiRegistrationOutsideAWindowIsRefusedNamingTheWindow)
{
    std::vector<std::string> lines;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&lines);
    const size_t refusedBefore = ManagedElementTypes::Instance().RefusedOutsideWindowCount();

    struct Arm
    {
        const char* Name;
        std::function<void(const std::function<void()>&)> Run;
    };
    const Arm arms[] = {
        {"a body on a worker", &RunOnAWorker},
        {"a waiter's job on the main thread", &RunAsAWaitersJob},
        {"a caller's ParallelFor unit on the main thread", &RunAsACallersUnit},
        {"the main thread outside any body", [](const std::function<void()>& body) { body(); }},
        {"a thread of the test's own",
         [](const std::function<void()>& body) {
             std::thread thread(body);
             thread.join();
         }},
    };
    for (const Arm& arm : arms)
    {
        const std::string tag = FreshTag("outside");
        GE_Result result = GE_Result_Fail;
        uint64_t tagId = 1234;
        arm.Run([&] { result = GE_UI_RegisterElementType(m_Owner, tag.c_str(), &tagId); });
        EXPECT_EQ(result, GE_Result_Ok) << arm.Name << ": a refused claim is a per-type skip, not an error";
        EXPECT_EQ(tagId, 0u) << arm.Name;
    }
    Logger::Log::Flush();
    EXPECT_EQ(ManagedElementTypes::Instance().RefusedOutsideWindowCount() - refusedBefore, std::size(arms));
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(lines, "GE_UI_RequestTypeRegistrationWindow"), std::size(arms));
}

// A reload thread asks for the window; the window runs on the main thread at the next pump, and its claims are
// admitted there.
TEST_F(UiElementTypeThreadRuleTest, RegistrationWindowFromAWorkerRunsOnThePumpInsideTheWindow)
{
    const std::string tag = FreshTag("window");
    uint64_t tagId = 0;
    bool ranOnMainThread = false;
    g_Window = [&] {
        ranOnMainThread = EngineCore::GetInstance().IsMainThread();
        EXPECT_EQ(GE_UI_RegisterElementType(m_Owner, tag.c_str(), &tagId), GE_Result_Ok);
    };
    RunOnAWorker([] { EXPECT_EQ(GE_UI_RequestTypeRegistrationWindow(), GE_Result_Ok); });
    EXPECT_EQ(tagId, 0u) << "the window waits for the pump";

    Pump();
    EXPECT_TRUE(ranOnMainThread);
    EXPECT_NE(tagId, 0u) << "a claim inside the window is admitted";
}

// The engine's own sweep runs in the hot reload's swap task, pumped on the main thread outside any body: admitted.
TEST_F(UiElementTypeThreadRuleTest, UnloadSweepOnTheSwapTaskIsAdmitted)
{
    const std::string tag = FreshTag("sweep");
    ASSERT_NE(ClaimInAWindow(tag), 0u);
    const size_t refusedBefore = ManagedElementTypes::Instance().RefusedOffMainThreadCount();

    GE_Result result = GE_Result_Fail;
    EngineCore::GetInstance().GetScriptManager().QueueMainThreadTask([&] {
        uint64_t orphaned = 0;
        result = GE_UI_OrphanElementTypes(m_Owner, &orphaned);
    });
    Pump();

    EXPECT_EQ(result, GE_Result_Ok);
    EXPECT_EQ(ManagedElementTypes::Instance().RefusedOffMainThreadCount(), refusedBefore)
        << "the swap task's sweep is never the permanent refusal";
    EXPECT_FALSE(GameEngine::UIRegistration::ElementFactoryRegistry::Instance().HasFactory(tag))
        << "the sweep unregistered the owner's tag";
}

// A sweep from a job body is refused, whichever thread runs the body: the answer depends on what the call is,
// not on which thread dequeued it.
TEST_F(UiElementTypeThreadRuleTest, ASweepFromAJobBodyIsRefusedWhicheverThreadRunsIt)
{
    const std::string tag = FreshTag("bodysweep");
    ASSERT_NE(ClaimInAWindow(tag), 0u);
    const size_t refusedBefore = ManagedElementTypes::Instance().RefusedOffMainThreadCount();

    bool waiterJobOnMainThread = false;
    const auto sweep = [&] {
        uint64_t orphaned = 99;
        EXPECT_EQ(GE_UI_OrphanElementTypes(m_Owner, &orphaned), GE_Result_Ok);
        EXPECT_EQ(orphaned, 0u);
    };
    RunOnAWorker(sweep);
    RunAsAWaitersJob([&] {
        waiterJobOnMainThread = EngineCore::GetInstance().IsMainThread();
        sweep();
    });

    EXPECT_TRUE(waiterJobOnMainThread) << "the second arm ran on the main thread";
    EXPECT_EQ(ManagedElementTypes::Instance().RefusedOffMainThreadCount() - refusedBefore, 2u);
    EXPECT_TRUE(GameEngine::UIRegistration::ElementFactoryRegistry::Instance().HasFactory(tag))
        << "a refused sweep changes nothing";
}

// After Shutdown the marshaller drops a window rather than constructing a ScriptManager on the calling thread.
TEST_F(UiElementTypeThreadRuleTest, AWindowRequestAfterEngineShutdownConstructsNoManager)
{
    EngineCore::GetInstance().Shutdown();
    std::vector<std::string> lines;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&lines);
    bool windowRan = false;
    g_Window = [&] { windowRan = true; };

    ManagedElementTypes::Instance().RequestRegistrationWindow();
    Logger::Log::Flush();

    EXPECT_FALSE(windowRan);
    EXPECT_FALSE(EngineCore::GetInstance().HasScriptManagerForTests());
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(lines, "the engine has shut down, and nothing will run it"), 1u);
}
