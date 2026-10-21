// The native module build and the fast-build toolchain detection wait on other
// processes (cmake, the compiler, vswhere and vcvars) for most of their run, so
// each runs as a job of its own cap-1 JobChannel on the pool's blocking threads.
// Pinned here on a pool with one compute worker: while the build (or the
// detection) is parked, a compute job still runs. Before the channels, the
// parked job held that worker and the compute job waited behind it.

#include "JobSystem/WorkStealingThreadPool.h"
#include "NativeScripting/CMakeInvoker.h"
#include "NativeScripting/MsvcToolchain.h"
#include "NativeScripting/NativeBuildConfig.h"
#include "NativeScripting/NativeScriptManager.h"

#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ns = GameEngine::NativeScripting;
using namespace std::chrono_literals;

namespace
{

constexpr auto kDeadline = 10s;
// A compute job on an idle worker finishes in microseconds; this bounds it on a
// loaded machine without letting a parked worker pass.
constexpr auto kComputeJobDeadline = 5s;
// The module build waits up to 20 s for the toolchain detection (vswhere and vcvars,
// real processes) before its first cmake call, and the detection itself runs those
// processes: on a loaded machine either took longer than 10 s. These bounds sit well
// above that, so only a build or a detection that waits on the parked worker misses
// them.
constexpr auto kReachCMakeDeadline = 60s;
constexpr auto kToolchainDetectionDeadline = 60s;

class Gate
{
public:
    void Open()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Open = true;
        m_Changed.notify_all();
    }
    void Wait()
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Changed.wait(lock, [this] { return m_Open; });
    }

private:
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    bool m_Open = false;
};

bool WaitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// Parks every cmake process the build starts on a gate, then reports a failed
// process. Restores the shell and opens the gate when it leaves scope; a manager
// whose build it parks is destroyed first, so OpenOnExit opens the gate before that.
class ParkedCMakeProcesses
{
public:
    ParkedCMakeProcesses()
    {
        ns::CMakeInvoker::SetProcessRunnerForTests([this](const std::string&, std::string& outOutput) {
            m_Entered.store(true);
            m_Gate.Wait();
            outOutput += "parked by the test\n";
            return 1;
        });
    }
    ~ParkedCMakeProcesses()
    {
        m_Gate.Open();
        ns::CMakeInvoker::SetProcessRunnerForTests({});
    }
    ParkedCMakeProcesses(const ParkedCMakeProcesses&) = delete;
    ParkedCMakeProcesses& operator=(const ParkedCMakeProcesses&) = delete;

    bool WaitUntilEntered() { return WaitUntil([this] { return m_Entered.load(); }, kReachCMakeDeadline); }
    void Open() { m_Gate.Open(); }

private:
    Gate m_Gate;
    std::atomic<bool> m_Entered{false};
};

// Opens the parked processes' gate when it leaves scope. Declared after the manager, it
// releases the parked build before the manager's destructor waits on it, on every exit
// path, a failed assertion included.
class OpenOnExit
{
public:
    explicit OpenOnExit(ParkedCMakeProcesses& processes) : m_Processes(processes) {}
    ~OpenOnExit() { m_Processes.Open(); }
    OpenOnExit(const OpenOnExit&) = delete;
    OpenOnExit& operator=(const OpenOnExit&) = delete;

private:
    ParkedCMakeProcesses& m_Processes;
};

bool ComputeJobCompletes(JobSystem::WorkStealingThreadPool& pool)
{
    JobSystem::TaskHandle job = pool.Submit([] {});
    return WaitUntil([&job] { return job.IsDone(); }, kComputeJobDeadline) && job.IsCompleted();
}

} // namespace

TEST(NativeBuildChannel, ParkedModuleBuildHoldsNoComputeWorker)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_nbc_parked_build"));
    const std::filesystem::path root = scratch.Path();
    std::error_code ec;
    std::filesystem::create_directories(root / "Source", ec);
    std::ofstream(root / "Source" / "ParkedModule.cpp") << "int GeParkedModuleProbe() { return 1; }\n";

    ns::NativeBuildConfig config;
    config.SourceDir = root / "Source";
    config.ProjectDir = root / "Project";
    config.BuildDir = root / "Cache" / "NativeScripts" / "build";
    config.ActiveDir = root / "Cache" / "NativeScripts" / "active";
    config.ModuleName = "ParkedModule";

    JobSystem::WorkStealingThreadPool pool(1);
    {
        ParkedCMakeProcesses cmake;
        ns::NativeScriptManager manager;
        OpenOnExit release(cmake);
        ASSERT_TRUE(manager.Initialize(&pool));
        manager.SetPackageModuleConfigs({config});
        manager.RequestRebuild();
        manager.Tick(); // dispatches the build

        ASSERT_TRUE(cmake.WaitUntilEntered()) << "the module build never reached cmake";
        EXPECT_TRUE(ComputeJobCompletes(pool)) << "the parked module build holds the only compute worker";
    }
}

TEST(NativeBuildChannel, ToolchainDetectionHoldsNoComputeWorker)
{
    GameEngine::TestUtils::ScopedTempDir toolchainCache(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_nbc_toolchain"));
    JobSystem::WorkStealingThreadPool pool(1);
    Gate workerGate;
    std::atomic<bool> workerParked{false};
    JobSystem::TaskHandle parked = pool.Submit([&] {
        workerParked.store(true);
        workerGate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return workerParked.load(); }, kDeadline));

    ns::MsvcToolchain toolchain;
    toolchain.WarmAsync(&pool, toolchainCache.Path());
    auto joined = std::async(std::launch::async, [&toolchain] { toolchain.JoinWarm(); });
    const bool detectedWhileWorkerParked = joined.wait_for(kToolchainDetectionDeadline) == std::future_status::ready;

    workerGate.Open();
    joined.wait();
    parked.Wait();
    EXPECT_TRUE(detectedWhileWorkerParked) << "toolchain detection waited for the only compute worker";
}
