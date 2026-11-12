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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace ns = GameEngine::NativeScripting;
using namespace std::chrono_literals;

namespace
{

constexpr auto kDeadline = 10s;
// A canceled probe ends at the next 15 ms cancel poll; vcvars alone runs for seconds.
constexpr auto kCanceledDetectionJoinDeadline = 1s;
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

// A latch the test opens. Declare it before the pool: a job can still be
// inside Wait() after the gate opens, and only the pool's destructor joins it.
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

#if defined(_WIN32)
// Adds the id of each running child process of this process to `seen`, once.
void CollectChildProcessIds(std::vector<DWORD>& seen)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    const DWORD self = GetCurrentProcessId();
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
    {
        if (entry.th32ParentProcessID == self && std::find(seen.begin(), seen.end(), entry.th32ProcessID) == seen.end())
            seen.push_back(entry.th32ProcessID);
    }
    CloseHandle(snapshot);
}

// The detection starts one child process per probe: where ninja, vswhere, then vcvars. True once
// the third one started: the vcvars probe, which runs for seconds.
bool WaitUntilTheVcvarsProbeRuns()
{
    std::vector<DWORD> seen;
    return WaitUntil(
        [&seen] {
            CollectChildProcessIds(seen);
            return seen.size() >= 3;
        },
        kToolchainDetectionDeadline);
}

bool HasVswhere()
{
    return std::filesystem::exists("C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe");
}
#endif

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
    Gate workerGate;
    JobSystem::WorkStealingThreadPool pool(1);
    std::atomic<bool> workerParked{false};
    JobSystem::TaskHandle parked = pool.Submit([&] {
        workerParked.store(true);
        workerGate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return workerParked.load(); }, kDeadline));

    ns::MsvcToolchain toolchain;
    toolchain.WarmAsync(&pool, toolchainCache.Path());
    // Wait before JoinWarm: JoinWarm cancels the probes, and this test needs a full detection.
    const auto waitStart = std::chrono::steady_clock::now();
    toolchain.WaitUntilReady(kToolchainDetectionDeadline);
    const bool detectedWhileWorkerParked = std::chrono::steady_clock::now() - waitStart < kToolchainDetectionDeadline;

    workerGate.Open();
    toolchain.JoinWarm();
    parked.Wait();
    EXPECT_TRUE(detectedWhileWorkerParked) << "toolchain detection waited for the only compute worker";
}

#if defined(_WIN32)
// A quit right after start-up must not wait for the vswhere and vcvars probes (seconds): JoinWarm
// terminates the running probe, so the join is short and the canceled detection finds nothing.
TEST(NativeBuildChannel, JoinWarmCancelsTheToolchainProbes)
{
    if (!HasVswhere())
        GTEST_SKIP() << "no vswhere: detection starts no Visual Studio probe to cancel";
    GameEngine::TestUtils::ScopedTempDir toolchainCache(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_nbc_toolchain"));
    JobSystem::WorkStealingThreadPool pool(1);
    ns::MsvcToolchain toolchain;
    toolchain.WarmAsync(&pool, toolchainCache.Path());
    EXPECT_TRUE(WaitUntilTheVcvarsProbeRuns()) << "the detection started no vcvars probe";

    const auto joinStart = std::chrono::steady_clock::now();
    toolchain.JoinWarm();
    const auto joinTime = std::chrono::steady_clock::now() - joinStart;

    EXPECT_LT(joinTime, kCanceledDetectionJoinDeadline) << "JoinWarm waited for the probes instead of canceling them";
    EXPECT_FALSE(toolchain.NinjaReady()) << "the detection ran to completion after JoinWarm";
}

// A quit while a module build waits for the toolchain detection does not wait for the probes either.
TEST(NativeBuildChannel, ShutdownDuringABuildThatWaitsForTheToolchainCancelsTheProbes)
{
    if (!HasVswhere())
        GTEST_SKIP() << "no vswhere: detection starts no Visual Studio probe to cancel";
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_nbc_quit_build"));
    const std::filesystem::path root = scratch.Path();
    std::error_code ec;
    std::filesystem::create_directories(root / "Source", ec);
    std::ofstream(root / "Source" / "QuitModule.cpp") << "int GeQuitModuleProbe() { return 1; }\n";

    ns::NativeBuildConfig config;
    config.SourceDir = root / "Source";
    config.ProjectDir = root / "Project";
    config.BuildDir = root / "Cache" / "NativeScripts" / "build";
    config.ActiveDir = root / "Cache" / "NativeScripts" / "active";
    config.ModuleName = "QuitModule";

    JobSystem::WorkStealingThreadPool pool(1);
    ParkedCMakeProcesses cmake;
    ns::NativeScriptManager manager;
    ASSERT_TRUE(manager.Initialize(&pool));
    manager.SetPackageModuleConfigs({config});
    manager.RequestRebuild();
    manager.Tick(); // dispatches the build, which waits for the toolchain detection
    EXPECT_TRUE(WaitUntilTheVcvarsProbeRuns()) << "the detection started no vcvars probe";
    cmake.Open(); // the build fails at once when it reaches cmake

    const auto shutdownStart = std::chrono::steady_clock::now();
    manager.Shutdown();
    const auto shutdownTime = std::chrono::steady_clock::now() - shutdownStart;

    EXPECT_LT(shutdownTime, kCanceledDetectionJoinDeadline) << "Shutdown waited for the build's toolchain wait";
}

// A warm without a job system after a joined warm does not wait for a detection that never starts.
TEST(NativeBuildChannel, WarmAsyncWithoutAJobSystemAfterJoinWarmDoesNotWait)
{
    GameEngine::TestUtils::ScopedTempDir toolchainCache(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_nbc_toolchain"));
    JobSystem::WorkStealingThreadPool pool(1);
    ns::MsvcToolchain toolchain;
    toolchain.WarmAsync(&pool, toolchainCache.Path());
    toolchain.JoinWarm();
    toolchain.WarmAsync(nullptr, toolchainCache.Path());

    const auto waitStart = std::chrono::steady_clock::now();
    toolchain.WaitUntilReady(kDeadline);
    const auto waitTime = std::chrono::steady_clock::now() - waitStart;

    EXPECT_LT(waitTime, kCanceledDetectionJoinDeadline) << "WaitUntilReady waited for a detection that never starts";
}

// A warm after JoinWarm runs a full detection: the cancel of the previous detection does not carry over.
TEST(NativeBuildChannel, WarmAsyncAfterJoinWarmDetectsAgain)
{
    GameEngine::TestUtils::ScopedTempDir toolchainCache(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_nbc_toolchain"));
    JobSystem::WorkStealingThreadPool pool(1);
    ns::MsvcToolchain toolchain;
    toolchain.WarmAsync(&pool, toolchainCache.Path());
    toolchain.JoinWarm();
    toolchain.WarmAsync(&pool, toolchainCache.Path());
    const bool readyAfterRewarm = toolchain.WaitUntilReady(kToolchainDetectionDeadline);
    toolchain.JoinWarm();
    if (readyAfterRewarm)
        return;

    ns::MsvcToolchain reference;
    reference.WarmAsync(&pool, toolchainCache.Path());
    const bool hostHasToolchain = reference.WaitUntilReady(kToolchainDetectionDeadline);
    reference.JoinWarm();
    if (!hostHasToolchain)
        GTEST_SKIP() << "no Ninja and MSVC environment on this host: a full detection finds nothing either";
    ADD_FAILURE() << "the warm after JoinWarm found no toolchain, but a fresh detection does";
}
#endif

#if !defined(_WIN32)
// POSIX detection is a PATH scan, so a host without a job system builds with Ninja, as the editor
// does. The macOS default generator (make 3.81) misses an edit saved in the same second as a build.
TEST(NativeBuildChannel, ToolchainDetectsNinjaWithoutAJobSystemOnPosix)
{
    GameEngine::TestUtils::ScopedTempDir toolchainCache(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_nbc_toolchain"));
    ns::MsvcToolchain toolchain;
    toolchain.WarmAsync(nullptr, toolchainCache.Path());
    EXPECT_TRUE(toolchain.WaitUntilReady(0ms)) << "no Ninja detected without a job system (BUILD.md requires ninja on PATH)";
    EXPECT_FALSE(toolchain.NinjaExe().empty());
}
#endif
