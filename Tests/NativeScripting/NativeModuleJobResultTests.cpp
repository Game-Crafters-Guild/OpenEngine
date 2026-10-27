#include "Core/Application.h"
#include "Core/Engine.h"
#include "Core/EngineLoggerBridge.h"
#include "EngineLogCapture.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "NativeScripting/BuildCacheRecord.h"
#include "NativeScripting/EngineBuildIdentity.h"
#include "NativeScripting/NativeScriptManager.h"
#include "Scripting/ScriptsConfig.h"
#include <gtest/gtest.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using ProbeExport = void (*)(JobSystem::WorkStealingThreadPool*, std::atomic<int>*);

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
        std::fflush(stderr);
        ::TerminateProcess(::GetCurrentProcess(), 17);
    }
}

// Yields until the job's envelope is dead on the worker, or until the timeout.
void AwaitIdlePool(const JobSystem::WorkStealingThreadPool& pool)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pool.GetPendingTasksApprox() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
}

// Writes a build record that points the loader at the probe DLL; returns the project root.
std::filesystem::path WriteProbeBuildRecord()
{
    namespace ns = GameEngine::NativeScripting;
    namespace fs = std::filesystem;
    wchar_t executable[32768]{};
    Check(::GetModuleFileNameW(nullptr, executable, 32768) != 0, "missing executable path");
    const auto dll = fs::path(executable).parent_path() / "JobResultModuleProbe.dll";
    const auto root =
        fs::temp_directory_path() / ("ge_job_result_module_" + std::to_string(::GetCurrentProcessId()));
    Check(ns::WriteBuildCacheRecord(root / "NativeScripts" / "build",
                                    ns::BuildCacheRecord{"job-result-test", dll.generic_string(), "",
                                                         ns::EngineBuildIdentity()}),
          "could not create isolated build record");
    return root;
}

// Calls the probe export in the mapped image, then waits until the pool is idle. holdWorker keeps the
// one worker busy while the export runs, so the export's job stays Pending.
void RunProbe(std::uint64_t imageBase, const char* probeExport, bool holdWorker,
              JobSystem::WorkStealingThreadPool& pool, std::atomic<int>& live)
{
    const auto probe =
        reinterpret_cast<ProbeExport>(::GetProcAddress(reinterpret_cast<HMODULE>(imageBase), probeExport));
    Check(probe != nullptr, "missing job result probe export");
    std::atomic<bool> holding{false};
    std::atomic<bool> released{!holdWorker};
    JobSystem::TaskHandle hold;
    if (holdWorker)
    {
        hold = pool.Submit([&holding, &released] {
            holding.store(true);
            while (!released.load())
                std::this_thread::yield();
        });
        while (!holding.load())
            std::this_thread::yield();
    }
    probe(&pool, &live);
    released.store(true);
    hold = {};
    AwaitIdlePool(pool);
}

// Runs the probe export on a one-worker pool while the image is mapped, lets the loader reject and unmap
// the image, then destroys the pool. A value the pool keeps past the unload has its destructor in the
// unmapped image.
void UnloadModuleAfterProbe(const char* probeExport, bool holdWorker)
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    namespace ns = GameEngine::NativeScripting;
    namespace fs = std::filesystem;
    const auto root = WriteProbeBuildRecord();
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    Logger::Log::Initialize({});

    std::atomic<int> live{0};
    auto pool = std::make_unique<JobSystem::WorkStealingThreadPool>(1);
    std::uint64_t mappedBase = 0;
    ns::NativeScriptManager manager;
    ns::NativeScriptManager::ModuleImageObserver observer;
    observer.ImageMapEnd = [&mappedBase, &pool, &live, probeExport, holdWorker](std::uint64_t base, std::uint64_t)
    {
        mappedBase = base;
        RunProbe(base, probeExport, holdWorker, *pool, live);
    };
    // The engine's C12 observer does the same (Engine.cpp).
    observer.ImageUnmapping = [&pool](std::uint64_t, std::uint64_t) { pool->ReleaseFinishedTasks(); };
    manager.SetModuleImageObserver(std::move(observer));
    Check(!manager.LoadPrebuiltUserModule(root, "JobResultProbe"), "module without an ABI export was accepted");
    HMODULE owner = nullptr;
    Check(mappedBase && !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                              reinterpret_cast<LPCWSTR>(mappedBase), &owner),
          "rejected image is still mapped");
    Check(live.load() == 0, "the pool kept a value of the module past the unload");
    pool.reset();
    Logger::Log::Shutdown();
    std::error_code error;
    fs::remove_all(root, error);
    ::TerminateProcess(::GetCurrentProcess(), 0);
}

// The probe completes a job on EngineCore's pool from its static initializer, and EngineCore's loader
// rejects and unmaps it. EngineCore's unload hook must destroy the result while the image is mapped.
void UnloadModuleFromEngineCoreAfterItsJobCompleted()
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    namespace fs = std::filesystem;
    std::vector<std::string> lines;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&lines);
    const auto root = WriteProbeBuildRecord();
    const fs::path workspace = root / "Workspace";
    std::error_code error;
    fs::create_directories(workspace, error);
    GameEngine::ApplicationConfig config{};
    config.WorkspaceDirectory = workspace.string();
    config.AssetDirectory = "Assets";
    GameEngine::ScriptsConfig scriptsConfig{};
    scriptsConfig.disableClr = true;
    scriptsConfig.enableHotReload = false;
    scriptsConfig.enableAutoProjectGeneration = false;
    auto& engine = GameEngine::EngineCore::GetInstance();
    engine.SetScriptsConfig(scriptsConfig);
    Check(engine.Initialize(config), "EngineCore did not initialize");
    Check(!engine.GetNativeScriptManager()->LoadPrebuiltUserModule(root, "JobResultProbe"),
          "module without an ABI export was accepted");
    Logger::Log::Flush();
    Check(GameEngine::TestLog::CountLinesContaining(lines, "JobResultProbe: engine job completed") == 1,
          "the probe did not complete its job on the engine's pool");
    Check(GameEngine::TestLog::CountLinesContaining(lines, "JobResultProbe: engine job result destroyed") == 1,
          "EngineCore's unload kept the module's job result past the unmap");
    engine.Shutdown();
    fs::remove_all(root, error);
    ::TerminateProcess(::GetCurrentProcess(), 0);
}

TEST(NativeModuleJobResult, PoolOutlivesTheModuleWithoutCallingIntoIt)
{
    EXPECT_EXIT(UnloadModuleAfterProbe("GE_JobResultProbe_SubmitAndWait", false), ::testing::ExitedWithCode(0), "");
}

TEST(NativeModuleJobResult, CancelledTaskDropsTheModulesContinuations)
{
    EXPECT_EXIT(UnloadModuleAfterProbe("GE_JobResultProbe_CancelWithContinuations", true),
                ::testing::ExitedWithCode(0), "");
}

TEST(NativeModuleJobResult, EngineCoreUnloadReleasesTheModulesJobResult)
{
    EXPECT_EXIT(UnloadModuleFromEngineCoreAfterItsJobCompleted(), ::testing::ExitedWithCode(0), "");
}
} // namespace
