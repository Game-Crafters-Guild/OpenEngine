// Pins that cancelling a hot-reload run ends the swap stage's wait for the main
// thread at once. The swap stage hands the swap to the main thread and waits
// for it; a Shutdown on the main thread cannot pump that queue, so a wait that
// ignored the run's cancellation stalled Shutdown for the full timeout.
//
// Also pins the other end of that wait: in an engine with no frame loop (a
// standalone-hosted one) the stage pumps the queue itself once the wait times
// out, so the swap runs instead of timing out.
//
// The engine is initialized headless with scripting fully disabled, so the
// swap stage takes its main-thread path against a ScriptManager whose queue
// these tests own and never pump.

#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Jobs/HotReloadCancellation.h"
#include "Jobs/HotReloadTasks.h"
#include "Scripting/ScriptManager.h"
#include "Scripting/ScriptsConfig.h"
#include "TestTempDir.h" // Tests/

#include <chrono>
#include <filesystem>
#include <memory>
#include <system_error>
#include <thread>

using namespace GameEngine;
using namespace std::chrono_literals;

namespace
{

constexpr auto kDeadline = 10s;
// Well under the swap stage's 5 s main-thread timeout.
constexpr auto kPromptCancel = 1s;

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
    }

    void TearDown() override
    {
        EngineCore::GetInstance().Shutdown();
        std::error_code ec;
        std::filesystem::current_path(m_OriginalWorkingDirectory, ec);
        std::filesystem::remove_all(m_Workspace, ec);
    }

    std::filesystem::path m_OriginalWorkingDirectory;
    std::filesystem::path m_Workspace;
    bool m_StandaloneHosted = false;
};

// What the scripting ABI's bootstrap initializes for dotnet test: no frame loop, no pump.
class MainThreadSwapStandalone : public MainThreadSwapCancel
{
  protected:
    MainThreadSwapStandalone() { m_StandaloneHosted = true; }
};

} // namespace

TEST_F(MainThreadSwapCancel, CancelEndsTheWaitForTheMainThread)
{
    // This thread plays the editor's main thread: Initialize marked it, so the
    // swap stage does not pump the queue inline, and it is busy, so it never
    // pumps it either.
    ASSERT_TRUE(EngineCore::GetInstance().IsMainThread());
    auto& scriptManager = EngineCore::GetInstance().GetScriptManager();
    ASSERT_EQ(scriptManager.GetMainThreadTaskCount(), 0u);

    JobSystem::WorkStealingThreadPool pool(1);
    auto cancellation = std::make_shared<HotReloadCancellation>();
    auto swap = pool.Submit(std::make_unique<MainThreadSwapTask>(&pool, "GameEngine.Scripts.dll",
                                                                 std::make_shared<PreloadSync>(), nullptr, cancellation));
    ASSERT_TRUE(swap.IsValid());

    // The swap is queued for the main thread just before the stage starts waiting.
    const auto queueDeadline = std::chrono::steady_clock::now() + kDeadline;
    while (scriptManager.GetMainThreadTaskCount() == 0 && std::chrono::steady_clock::now() < queueDeadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(scriptManager.GetMainThreadTaskCount(), 1u) << "the swap stage never queued its main-thread swap";

    const auto cancelled = std::chrono::steady_clock::now();
    cancellation->Cancel();
    swap.Wait();
    const auto waited = std::chrono::steady_clock::now() - cancelled;

    EXPECT_LT(waited, kPromptCancel) << "the swap stage waited "
                                     << std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()
                                     << " ms for the main thread after its run was cancelled";
    std::chrono::milliseconds swapDuration{};
    EXPECT_FALSE(swap.TryGetResult(swapDuration)) << "a cancelled swap must not publish a swap duration";
}

TEST_F(MainThreadSwapStandalone, TheSwapStagePumpsTheQueueItselfWhenNoFrameLoopRuns)
{
    ASSERT_TRUE(EngineCore::GetInstance().IsStandaloneHosted());
    auto& scriptManager = EngineCore::GetInstance().GetScriptManager();
    ASSERT_EQ(scriptManager.GetMainThreadTaskCount(), 0u);

    JobSystem::WorkStealingThreadPool pool(1);
    auto swap = pool.Submit(std::make_unique<MainThreadSwapTask>(&pool, "GameEngine.Scripts.dll",
                                                                 std::make_shared<PreloadSync>(), nullptr,
                                                                 std::make_shared<HotReloadCancellation>()));
    ASSERT_TRUE(swap.IsValid());
    // Returns after the stage's 5 s main-thread timeout, when it pumps the queue itself.
    swap.Wait();

    EXPECT_EQ(scriptManager.GetMainThreadTaskCount(), 0u)
        << "the swap stage left its swap queued for a main thread that never pumps";
}
