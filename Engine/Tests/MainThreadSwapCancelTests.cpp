// Pins how a hot-reload run reaches its swap: the preparation job's completion
// queues it on the main thread, Cancel makes a queued swap do nothing, with no
// frame loop the swap runs inline, and a swap that did not happen fails the run.
//
// The engine is initialized headless with scripting fully disabled, so the
// swap takes its main-thread path against a ScriptManager whose queue these
// tests own and pump only when they say so.

#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/Engine.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Jobs/HotReloadTasks.h"
#include "Jobs/MockHotReloadDependencies.h" // Tests/Jobs
#include "Scripting/ScriptManager.h"
#include "Scripting/ScriptingABI.h"
#include "Scripting/ScriptsConfig.h"
#include "TestTempDir.h" // Tests/

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

using namespace GameEngine;
using namespace std::chrono_literals;

namespace
{

constexpr auto kDeadline = 10s;
// The time the worker that queued the swap gets to leave its job.
constexpr auto kPromptRelease = 1s;

// A compile that blocks until the test releases it.
class GatedCompiler : public Jobs::ICompiler
{
  public:
    struct Gate
    {
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Released = false;
        std::atomic<bool> Started{false};
    };

    explicit GatedCompiler(std::shared_ptr<Gate> gate) : m_Gate(std::move(gate)) {}

    CompilationResult compile(const String& /*assemblyPath*/, const std::atomic<bool>* /*cancelRequested*/) override
    {
        m_Gate->Started.store(true);
        std::unique_lock<std::mutex> lock(m_Gate->Mutex);
        m_Gate->Cv.wait(lock, [this] { return m_Gate->Released; });
        CompilationResult result;
        result.success = true;
        return result;
    }

  private:
    std::shared_ptr<Gate> m_Gate;
};

void Release(GatedCompiler::Gate& gate)
{
    {
        std::lock_guard<std::mutex> lock(gate.Mutex);
        gate.Released = true;
    }
    gate.Cv.notify_all();
}

GE_Result GE_CDECL SwapReportsAFailure()
{
    return GE_Result_Fail;
}

GE_Result GE_CDECL SwapFindsAnotherSwapRunning()
{
    return GE_Result_Busy;
}

// Makes GE_SwapPreloadedContext call the given function in place of the managed swap.
void InstallManagedSwap(GE_SwapPreloadedContext_Fn swap)
{
    GE_HotReloadCallbacks callbacks{};
    callbacks.SwapPreloadedContext = swap;
    GE_RegisterHotReloadCallbacks(&callbacks);
}

template <typename Predicate>
bool WaitUntil(Predicate done, std::chrono::steady_clock::duration timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    return done();
}

class MainThreadSwapCancel : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_OriginalWorkingDirectory = std::filesystem::current_path();
        m_Workspace = TestUtils::MakeUniqueTempDirectory("MainThreadSwapCancel");

        ApplicationConfig config{};
        config.WorkspaceDirectory = m_Workspace.string();
        config.AssetDirectory = "Assets";
        config.StandaloneHosted = m_StandaloneHosted;
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAsyncHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);
        ASSERT_TRUE(EngineCore::GetInstance().Initialize(config));

        m_Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(1);
        m_Compiles = std::make_unique<JobSystem::JobChannel>(
            *m_Pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
        m_Pipeline = std::make_unique<HotReloadPipeline>(*m_Pool, *m_Compiles);
        m_Pipeline->SetCompilerFactory([] { return std::make_unique<Tests::MockCompiler>(true, 0); });
        m_Pipeline->SetAssemblyLoaderFactory([] { return std::make_unique<Tests::MockAssemblyLoader>(); });
    }

    void TearDown() override
    {
        GE_RegisterHotReloadCallbacks(nullptr);
        m_Pipeline.reset();
        m_Compiles.reset();
        m_Pool.reset();
        EngineCore::GetInstance().Shutdown();
        std::error_code ec;
        std::filesystem::current_path(m_OriginalWorkingDirectory, ec);
        std::filesystem::remove_all(m_Workspace, ec);
    }

    ScriptManager& Scripts() { return EngineCore::GetInstance().GetScriptManager(); }

    bool WaitForQueuedSwap()
    {
        return WaitUntil([this] { return Scripts().GetMainThreadTaskCount() != 0; }, kDeadline);
    }

    std::filesystem::path m_OriginalWorkingDirectory;
    std::filesystem::path m_Workspace;
    bool m_StandaloneHosted = false;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_Pool;
    std::unique_ptr<JobSystem::JobChannel> m_Compiles;
    std::unique_ptr<HotReloadPipeline> m_Pipeline;
};

// What the scripting ABI's bootstrap initializes for dotnet test: no frame loop, no pump.
class MainThreadSwapStandalone : public MainThreadSwapCancel
{
  protected:
    MainThreadSwapStandalone() { m_StandaloneHosted = true; }
};

} // namespace

TEST_F(MainThreadSwapCancel, NoWorkerIsHeldWhileTheSwapWaitsForTheMainThread)
{
    // This thread is the marked main thread and does not pump until the end,
    // so the run's swap stays queued for it.
    ASSERT_TRUE(EngineCore::GetInstance().IsMainThread());
    ASSERT_EQ(Scripts().GetMainThreadTaskCount(), 0u);

    auto run = m_Pipeline->ExecuteAsync("GameEngine.Scripts.dll");
    ASSERT_TRUE(WaitForQueuedSwap()) << "the run never queued its swap for the main thread";

    // The worker that queued the swap leaves its job at once.
    WaitUntil([this] { return m_Pool->GetStatistics().Normal.Running == 0; }, kPromptRelease);
    const JobSystem::JobSystemStatistics stats = m_Pool->GetStatistics();
    EXPECT_EQ(Scripts().GetMainThreadTaskCount(), 1u);
    EXPECT_EQ(stats.Normal.Running, 0u) << "a compute worker is held while the swap waits for the main thread";
    for (const auto& channel : stats.Channels)
    {
        EXPECT_EQ(channel.Running, 0u) << channel.Name << " holds a thread while the swap waits for the main thread";
    }
    EXPECT_NE(run.wait_for(0s), std::future_status::ready);

    // The main thread pumps: the swap runs and the run ends.
    EXPECT_EQ(Scripts().ProcessMainThreadTasks(), 1u);
    ASSERT_EQ(run.wait_for(0s), std::future_status::ready);
    EXPECT_TRUE(run.get().success);
    EXPECT_FALSE(m_Pipeline->IsExecuting());
}

TEST_F(MainThreadSwapCancel, TheSwapIsQueuedOnlyAfterTheCompileAndPreparationEnd)
{
    auto gate = std::make_shared<GatedCompiler::Gate>();
    m_Pipeline->SetCompilerFactory([gate] { return std::make_unique<GatedCompiler>(gate); });

    auto run = m_Pipeline->ExecuteAsync("GameEngine.Scripts.dll");
    ASSERT_TRUE(WaitUntil([&gate] { return gate->Started.load(); }, kDeadline)) << "the compile never started";
    EXPECT_EQ(Scripts().GetMainThreadTaskCount(), 0u) << "the swap was queued while the compile ran";
    EXPECT_EQ(Scripts().ProcessMainThreadTasks(), 0u);
    EXPECT_NE(run.wait_for(0s), std::future_status::ready);

    Release(*gate);
    ASSERT_TRUE(WaitForQueuedSwap()) << "the run never queued its swap after the compile ended";
    EXPECT_EQ(Scripts().ProcessMainThreadTasks(), 1u);
    ASSERT_EQ(run.wait_for(0s), std::future_status::ready);
    EXPECT_TRUE(run.get().success);
}

TEST_F(MainThreadSwapCancel, CancelEndsTheRunAndItsQueuedSwapDoesNothing)
{
    auto run = m_Pipeline->ExecuteAsync("GameEngine.Scripts.dll");
    ASSERT_TRUE(WaitForQueuedSwap());

    m_Pipeline->Cancel();
    ASSERT_EQ(run.wait_for(0s), std::future_status::ready) << "Cancel returned before the run ended";
    EXPECT_EQ(run.get().result, HotReloadResult::CancelledByNewOperation);
    EXPECT_FALSE(m_Pipeline->IsExecuting());

    // The queued swap runs and finds its run cancelled.
    EXPECT_EQ(Scripts().ProcessMainThreadTasks(), 1u);
    EXPECT_FALSE(m_Pipeline->IsExecuting());
}

TEST_F(MainThreadSwapCancel, ASwapQueuedWhenThePipelineIsDestroyedDoesNothing)
{
    auto run = m_Pipeline->ExecuteAsync("GameEngine.Scripts.dll");
    ASSERT_TRUE(WaitForQueuedSwap());

    m_Pipeline.reset();
    ASSERT_EQ(run.wait_for(0s), std::future_status::ready);
    EXPECT_EQ(run.get().result, HotReloadResult::CancelledByNewOperation);
    EXPECT_EQ(Scripts().ProcessMainThreadTasks(), 1u);
}

TEST_F(MainThreadSwapCancel, AnOverdueRunIsEndedAsATimeout)
{
    auto gate = std::make_shared<GatedCompiler::Gate>();
    m_Pipeline->SetCompilerFactory([gate] { return std::make_unique<GatedCompiler>(gate); });
    auto run = m_Pipeline->ExecuteAsync("GameEngine.Scripts.dll");
    ASSERT_TRUE(WaitUntil([&gate] { return gate->Started.load(); }, kDeadline));

    m_Pipeline->CancelOverdueRun(std::chrono::steady_clock::now());
    EXPECT_NE(run.wait_for(0s), std::future_status::ready) << "a run inside its time bound was ended";

    m_Pipeline->CancelOverdueRun(std::chrono::steady_clock::now() + std::chrono::minutes(10));
    ASSERT_EQ(run.wait_for(0s), std::future_status::ready);
    const auto stats = run.get();
    EXPECT_EQ(stats.result, HotReloadResult::UnknownError);
    EXPECT_EQ(stats.errorMessage, "Pipeline execution timeout");
    EXPECT_FALSE(m_Pipeline->IsExecuting());
    Release(*gate);
}

TEST_F(MainThreadSwapCancel, FailedSwapEndsTheRunAsFailedWithItsReason)
{
    InstallManagedSwap(&SwapReportsAFailure);
    auto run = m_Pipeline->ExecuteAsync("GameEngine.Scripts.dll");
    ASSERT_TRUE(WaitForQueuedSwap());
    EXPECT_EQ(Scripts().ProcessMainThreadTasks(), 1u);
    ASSERT_EQ(run.wait_for(0s), std::future_status::ready);
    const auto stats = run.get();

    EXPECT_FALSE(stats.success) << "a run whose swap failed must not report success";
    EXPECT_EQ(stats.result, HotReloadResult::SwapFailed);
    EXPECT_NE(stats.errorMessage.find("code -1"), std::string::npos) << stats.errorMessage;
    EXPECT_NE(stats.errorMessage.find("Save a script again to retry"), std::string::npos) << stats.errorMessage;
    EXPECT_FALSE(m_Pipeline->IsExecuting());
}

TEST_F(MainThreadSwapCancel, BusySwapEndsTheRunAsNotSwapped)
{
    InstallManagedSwap(&SwapFindsAnotherSwapRunning);
    auto run = m_Pipeline->ExecuteAsync("GameEngine.Scripts.dll");
    ASSERT_TRUE(WaitForQueuedSwap());
    EXPECT_EQ(Scripts().ProcessMainThreadTasks(), 1u);
    ASSERT_EQ(run.wait_for(0s), std::future_status::ready);
    const auto stats = run.get();

    EXPECT_FALSE(stats.success) << "a run whose swap did not start must not report success";
    EXPECT_EQ(stats.result, HotReloadResult::SwapBusy);
    EXPECT_FALSE(IsCancellation(stats.result)) << "the edit is not live, so the editor must show a failure";
    EXPECT_NE(stats.errorMessage.find("another script reload was running"), std::string::npos) << stats.errorMessage;
    EXPECT_NE(stats.errorMessage.find("Save a script again to retry"), std::string::npos) << stats.errorMessage;
}

TEST_F(MainThreadSwapStandalone, TheSwapRunsInlineWhenNoFrameLoopRuns)
{
    ASSERT_TRUE(EngineCore::GetInstance().IsStandaloneHosted());
    ASSERT_EQ(Scripts().GetMainThreadTaskCount(), 0u);

    auto run = m_Pipeline->ExecuteAsync("GameEngine.Scripts.dll");
    // Nothing pumps the queue here.
    ASSERT_EQ(run.wait_for(kDeadline), std::future_status::ready);
    EXPECT_TRUE(run.get().success);
    EXPECT_EQ(Scripts().GetMainThreadTaskCount(), 0u) << "the swap was queued for a main thread that never pumps";
}
