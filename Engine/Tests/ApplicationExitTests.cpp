// Application.RequestExit through its generated table, the call a C# script makes
// (Managed/Scripting.Bindings/Generated/Application.g.cs): the frame finishes, the host decides
// what a game's exit means, and a call off the main thread or from a job is refused.
#include "Core/Application.h"
#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scripting/ScriptingABI.h"
#include "Scripting/ScriptsConfig.h"
#include "scripting/Application.script.gen.h"

#include <thread>

#include <gtest/gtest.h>

namespace
{

GE_Result ScriptRequestExit(int32_t exitCode)
{
    const void* table = nullptr;
    uint32_t size = 0;
    EXPECT_EQ(GE_Application_GetInterface(GE_Application_Hash, &table, &size), GE_Result_Ok);
    EXPECT_EQ(size, sizeof(GE_Application_Table_v1));
    return static_cast<const GE_Application_Table_v1*>(table)->RequestExit(exitCode);
}

class ExitRequestApplication : public GameEngine::Application
{
public:
    using GameEngine::Application::Application;

    int UpdateCalls = 0;
    int RenderCalls = 0;
    int ShutdownCalls = 0;
    int RequestedExitCode = 0;
    GE_Result RequestResult = GE_Result_Fail;

protected:
    void Update(GameEngine::float64) override
    {
        ++UpdateCalls;
        RequestResult = ScriptRequestExit(RequestedExitCode);
    }

    void Render() override { ++RenderCalls; }
    void OnShutdown() override { ++ShutdownCalls; }
};

class EmbeddedGameApplication : public ExitRequestApplication
{
public:
    using ExitRequestApplication::ExitRequestApplication;

    int GameExitCalls = 0;
    int GameExitCode = 0;

    void RequestGameExit(int exitCode) override
    {
        ++GameExitCalls;
        GameExitCode = exitCode;
    }
};

class ApplicationExit : public ::testing::Test
{
protected:
    void SetUp() override
    {
        GameEngine::ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAsyncHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        GameEngine::EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);
    }

    GameEngine::ApplicationConfig Config()
    {
        GameEngine::ApplicationConfig config{};
        config.Name = "ApplicationExitTests";
        config.Headless = true;
        return config;
    }
};

} // namespace

TEST_F(ApplicationExit, RequestPreservesTheExitCodeAndPreventsTheNextFrame)
{
    ExitRequestApplication application(Config());
    ASSERT_TRUE(application.Initialize());

    EXPECT_EQ(ScriptRequestExit(23), GE_Result_Ok);
    // Run() loops until an exit is requested: without the request it never returns.
    ASSERT_TRUE(application.ShouldExit());
    EXPECT_EQ(application.GetExitCode(), 23);
    EXPECT_EQ(application.Run(), 23);
    EXPECT_EQ(application.UpdateCalls, 0);
    application.Shutdown();
    EXPECT_EQ(application.ShutdownCalls, 1);
}

TEST_F(ApplicationExit, RequestInsideAFrameFinishesTheFrameAndShutsDownNormally)
{
    ExitRequestApplication application(Config());
    application.RequestedExitCode = -7;
    ASSERT_TRUE(application.Initialize());

    EXPECT_FALSE(application.Tick());
    EXPECT_EQ(application.RequestResult, GE_Result_Ok);
    EXPECT_EQ(application.UpdateCalls, 1);
    EXPECT_EQ(application.RenderCalls, 1);
    EXPECT_EQ(application.GetFrameCount(), 1u);
    EXPECT_EQ(application.GetExitCode(), -7);
    EXPECT_FALSE(application.PollEventsAndTick());
    EXPECT_EQ(application.UpdateCalls, 1);
    application.Shutdown();
    EXPECT_EQ(application.ShutdownCalls, 1);
}

TEST_F(ApplicationExit, RepeatedRequestsKeepTheLastExitCode)
{
    ExitRequestApplication application(Config());
    ASSERT_TRUE(application.Initialize());

    EXPECT_EQ(ScriptRequestExit(23), GE_Result_Ok);
    EXPECT_EQ(ScriptRequestExit(0), GE_Result_Ok);
    ASSERT_TRUE(application.ShouldExit());
    EXPECT_EQ(application.Run(), 0);
    application.Shutdown();
}

TEST_F(ApplicationExit, GameExitUsesTheHostPolicyWithoutClosingTheHost)
{
    EmbeddedGameApplication application(Config());
    application.RequestedExitCode = 23;
    ASSERT_TRUE(application.Initialize());

    EXPECT_TRUE(application.Tick());
    EXPECT_EQ(application.RequestResult, GE_Result_Ok);
    EXPECT_EQ(application.GameExitCalls, 1);
    EXPECT_EQ(application.GameExitCode, 23);
    EXPECT_EQ(application.RenderCalls, 1);
    EXPECT_FALSE(application.ShouldExit());
    EXPECT_EQ(application.GetExitCode(), 0);
    application.Shutdown();
}

TEST_F(ApplicationExit, AnotherThreadCannotRequestExit)
{
    ExitRequestApplication application(Config());
    ASSERT_TRUE(application.Initialize());

    GE_Result result = GE_Result_Ok;
    std::thread worker([&result] { result = ScriptRequestExit(23); });
    worker.join();

    EXPECT_EQ(result, GE_Result_Fail);
    EXPECT_FALSE(application.ShouldExit());
    EXPECT_EQ(application.GetExitCode(), 0);
    application.Shutdown();
}

TEST_F(ApplicationExit, AnEngineJobCannotRequestExit)
{
    ExitRequestApplication application(Config());
    ASSERT_TRUE(application.Initialize());

    GE_Result result = GE_Result_Ok;
    auto& jobs = GameEngine::EngineCore::GetInstance().GetJobSystem();
    JobSystem::JobCounter counter;
    jobs.Run([&result] { result = ScriptRequestExit(23); }, counter);
    jobs.Wait(counter);

    EXPECT_EQ(result, GE_Result_Fail);
    EXPECT_FALSE(application.ShouldExit());
    EXPECT_EQ(application.GetExitCode(), 0);
    application.Shutdown();
}
