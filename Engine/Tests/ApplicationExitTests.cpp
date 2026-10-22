#include "Core/Application.h"
#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scripting/ApplicationABI.h"
#include "Scripting/ScriptsConfig.h"

#include <thread>

#include <gtest/gtest.h>

namespace
{

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
        RequestResult = GE_Application_RequestExit(RequestedExitCode);
    }

    void Render() override { ++RenderCalls; }
    void OnShutdown() override { ++ShutdownCalls; }
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

}

TEST_F(ApplicationExit, NoApplicationDoesNotInitializeTheEngine)
{
    ASSERT_EQ(GameEngine::Application::Get(), nullptr);
    ASSERT_FALSE(GameEngine::EngineCore::GetInstance().IsInitialized());

    EXPECT_EQ(GE_Application_RequestExit(23), GE_Result_NotInitialized);
    EXPECT_FALSE(GameEngine::EngineCore::GetInstance().IsInitialized());
}

TEST_F(ApplicationExit, RequestPreservesTheExitCodeAndPreventsTheNextFrame)
{
    ExitRequestApplication application(Config());
    ASSERT_TRUE(application.Initialize());

    EXPECT_EQ(GE_Application_RequestExit(23), GE_Result_Ok);
    EXPECT_TRUE(application.ShouldExit());
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

    EXPECT_EQ(GE_Application_RequestExit(23), GE_Result_Ok);
    EXPECT_EQ(GE_Application_RequestExit(0), GE_Result_Ok);
    EXPECT_TRUE(application.ShouldExit());
    EXPECT_EQ(application.Run(), 0);
    application.Shutdown();
}

TEST_F(ApplicationExit, AnotherThreadCannotRequestExit)
{
    ExitRequestApplication application(Config());
    ASSERT_TRUE(application.Initialize());

    GE_Result result = GE_Result_Ok;
    std::thread worker([&result] { result = GE_Application_RequestExit(23); });
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
    jobs.Run([&result] { result = GE_Application_RequestExit(23); }, counter);
    jobs.Wait(counter);

    EXPECT_EQ(result, GE_Result_Fail);
    EXPECT_FALSE(application.ShouldExit());
    EXPECT_EQ(application.GetExitCode(), 0);
    application.Shutdown();
}
