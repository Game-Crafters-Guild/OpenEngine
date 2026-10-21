#pragma once
#include <cstdint>
#include <string>

// Test-side lifetime management for real CompileServerHost processes
// (issue #357):
//
// 1. Every host spawned through the engine's launch path (explicitly via
//    StartTestCompileServer or implicitly via pipeline/ScriptManager
//    compiles) dies with the test process, even when the run crashes or is
//    killed: the launch path binds it to its launcher (a Windows
//    kill-on-close job object, a Linux parent-death signal, the host's
//    --parent-pid watch).
// 2. Fixture teardown (a global gtest Environment) reaps registered pipes
//    on orderly exit: __shutdown__, bounded grace, then kill by PID.
namespace CompileServerTestHost {

// Spawns a host for the pipe through the engine's real launch path
// (CompileServerClient::StartServerForPipe) and registers the pipe for
// teardown reaping. Returns false when the host DLL cannot be found or the
// process failed to start.
bool StartTestCompileServer(const std::string& pipeName);

// PID self-reported (via __version__) by a host currently listening on the
// pipe; 0 when none is reachable.
uint32_t QueryHostPid(const std::string& pipeName, int connectWaitMs = 100);

// Shuts down every host listening on the pipe: __shutdown__ per instance,
// ~2s grace for orderly exit, then kill by PID.
void ReapHostsOnPipe(const std::string& pipeName);

// Reaps every pipe registered by StartTestCompileServer.
void ReapAllTestHosts();

// "GE_CompileServer_Test_<tag>_<pid>" — unique per run so tests never talk
// to a stale host from a previous (crashed) run.
std::string UniquePipeName(const char* tag);

} // namespace CompileServerTestHost
