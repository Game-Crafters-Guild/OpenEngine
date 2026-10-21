// A CompileServerHost belongs to the process that launched it (issues #357,
// #3777):
//   1. Every host spawned through the engine's real launch path lands in the
//      kill-on-close job object, so it dies when its launcher exits for any
//      reason (a crash, a forced stop).
//   2. CompileServerClient::StopStartedServers stops the hosts a compile
//      launched, which is what an orderly Editor shutdown runs, and returns
//      within its bound even when a host has stopped responding.
// Fixture teardown also reaps test hosts on orderly exit (__shutdown__,
// bounded grace, then kill), so cleanup does not depend on the job alone.
#include <gtest/gtest.h>
#include "CompileServerTestHost.h"
#include "Jobs/CompileServerClient.h"
#include "Jobs/HotReloadTestHooks.h"
#include "Jobs/IHotReloadTransport.h"
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace {

// Cold start (JIT, AV scanning) can take several seconds; polls for the host
// to come up and self-report its PID over __version__.
uint32_t WaitForHostPid(const std::string& pipeName)
{
    uint32_t pid = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (pid == 0 && std::chrono::steady_clock::now() < deadline)
    {
        pid = CompileServerTestHost::QueryHostPid(pipeName);
        if (pid == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return pid;
}

// Makes the client launch a host itself, as the Editor's first script compile
// does: an empty-workspace compile on a pipe nobody serves. Returns the host's
// pid, or 0 when no host could be started.
uint32_t LaunchHostThroughCompile(const std::string& pipeName)
{
    const std::string requestJson =
        "{\"ProjectRoot\":\".\",\"ChangedFiles\":[],\"AffectedFiles\":[],\"AllFiles\":[],"
        "\"PreferredStrategy\":\"Incremental\",\"ForceFull\":false}";
    GameEngine::CompileServerClient client(std::unique_ptr<GameEngine::IHotReloadTransport>{}, pipeName);
    GameEngine::CompileServerResponse response;
    (void)client.Compile(requestJson, response);
    return WaitForHostPid(pipeName);
}

std::filesystem::path HostPidFile(const std::string& pipeName)
{
    return std::filesystem::temp_directory_path() / "GE_CompileServer" / (pipeName + ".pid");
}

bool IsProcessGone(uint32_t pid)
{
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process)
        return true;
    const bool gone = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    return gone;
#else
    return ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
#endif
}

// Stops every thread of the process, so it neither reads its pipe nor exits on
// its own: a host that has stopped responding.
void FreezeProcess(uint32_t pid)
{
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    ASSERT_NE(snapshot, INVALID_HANDLE_VALUE);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != pid)
            continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
        if (thread)
        {
            SuspendThread(thread);
            CloseHandle(thread);
        }
    }
    CloseHandle(snapshot);
#else
    ASSERT_EQ(::kill(static_cast<pid_t>(pid), SIGSTOP), 0);
#endif
}

} // namespace

TEST(CompileServerHostLifetime, TestSpawnedHostIsJobBoundAndReapedByTeardown)
{
#ifdef _WIN32
    const std::string pipeName = CompileServerTestHost::UniquePipeName("Lifetime");

    if (!CompileServerTestHost::StartTestCompileServer(pipeName))
    {
        GTEST_SKIP() << "CompileServerHost not found or failed to start; skipping lifetime test";
    }

    const uint32_t pid = WaitForHostPid(pipeName);
    ASSERT_NE(pid, 0u) << "Host never became reachable on pipe '" << pipeName << "'";

    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    ASSERT_NE(process, nullptr) << "OpenProcess failed for host pid=" << pid;

    // Layer 1: the host must be inside OUR job object, not merely "in some
    // job" (test runners often wrap the test exe in their own job), and that
    // job must kill its processes when its last handle closes.
    HANDLE job = static_cast<HANDLE>(GameEngine::GetCompileServerHostJobHandleForTests());
    ASSERT_NE(job, nullptr) << "The host job object was never created";
    BOOL inJob = FALSE;
    ASSERT_TRUE(IsProcessInJob(process, job, &inJob));
    EXPECT_TRUE(inJob) << "Host pid=" << pid << " is not in the kill-on-close job object";
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    ASSERT_TRUE(QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr));
    EXPECT_NE(limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, 0u);

    // Layer 2: teardown reaping. __shutdown__ + bounded grace must remove the
    // process; the kill fallback caps the wait.
    CompileServerTestHost::ReapHostsOnPipe(pipeName);
    EXPECT_EQ(WaitForSingleObject(process, 5000), WAIT_OBJECT_0)
        << "Host pid=" << pid << " still alive 5s after teardown reap";
    CloseHandle(process);

    // The pipe must be dead too — no other instance left behind.
    EXPECT_EQ(CompileServerTestHost::QueryHostPid(pipeName), 0u);
#else
    GTEST_SKIP() << "Job-object lifetime is Windows-only; POSIX uses PR_SET_PDEATHSIG (Linux) and the host's --parent-pid watch";
#endif
}

TEST(CompileServerHostLifetime, StopStartedServersStopsTheHostACompileLaunched)
{
    const std::string pipeName = CompileServerTestHost::UniquePipeName("StopStarted");
    const uint32_t pid = LaunchHostThroughCompile(pipeName);
    if (pid == 0)
    {
        GTEST_SKIP() << "CompileServerHost not found or failed to start; skipping stop test";
    }
    ASSERT_TRUE(std::filesystem::exists(HostPidFile(pipeName))) << "Host pid=" << pid << " wrote no pid file";

    GameEngine::CompileServerClient::StopStartedServers();

    // StopStartedServers waits for the exit itself: the host is gone on return,
    // and so is the pid file a terminated host cannot delete.
    EXPECT_TRUE(IsProcessGone(pid)) << "Host pid=" << pid << " still alive after StopStartedServers";
    EXPECT_FALSE(std::filesystem::exists(HostPidFile(pipeName)));
    EXPECT_EQ(CompileServerTestHost::QueryHostPid(pipeName), 0u);
}

TEST(CompileServerHostLifetime, StopStartedServersReturnsWhileTheHostIsFrozen)
{
    const std::string pipeName = CompileServerTestHost::UniquePipeName("StopFrozen");
    const uint32_t pid = LaunchHostThroughCompile(pipeName);
    if (pid == 0)
    {
        GTEST_SKIP() << "CompileServerHost not found or failed to start; skipping frozen-host test";
    }
    FreezeProcess(pid);

    const auto start = std::chrono::steady_clock::now();
    GameEngine::CompileServerClient::StopStartedServers();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // The stop waits at most 2 s per host; the margin covers a loaded machine.
    EXPECT_LT(elapsed, std::chrono::seconds(5));
    EXPECT_TRUE(IsProcessGone(pid)) << "Frozen host pid=" << pid << " still alive after StopStartedServers";
}
