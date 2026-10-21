#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

// The script build as the play-mode entry gate sees it.
struct ScriptBuildSnapshot
{
    // A build the game's code depends on is requested or running: a shipped native module
    // (the project module or a runtime package module) or the managed scripts.
    bool Building = false;
    // One line naming what is being built now ("Eztree: compiling (3 of 9 built)").
    std::string Activity;
    // Modules built out of the batch, 0 to 1; negative when the build gives no measure (a native
    // build of a single module reports its stage, not how far it is).
    float Fraction = -1.0f;
    // What failed in the latest builds: shipped native modules by name, and "C# scripts".
    std::vector<std::string> FailedModules;
    // The first error the failed build reported, without its directory
    // ("BreakoutSystem.cpp(296): error C2059: syntax error: 'this'"); empty when none failed.
    std::string FirstError;
};

// The editor's scripts as they are now: the native script manager's shipped-module builds and the
// managed hot reload.
ScriptBuildSnapshot ReadScriptBuildSnapshot();

// Holds a play-mode entry while the game's scripts build: the entry runs once the build finishes
// without errors and is dropped when it fails or by Cancel. Play is also refused, with no build in
// flight, while the latest build of a module has failed: the game would run without that module's
// current code. Otherwise play enters at once.
class PlayModeEntryGate
{
  public:
    enum class Phase : std::uint8_t
    {
        Idle,
        Waiting, // an entry is held until the build ends
        Failed,  // the latest build failed; the entry was dropped
    };

    using BuildProbe = std::function<ScriptBuildSnapshot()>;
    explicit PlayModeEntryGate(BuildProbe probe);

    // Returns true when play must not enter now: while scripts are building it keeps `enter`
    // (replacing an entry already held) to run when the build succeeds; after a failed build it
    // reports the failure and keeps nothing. Returns false, keeping nothing, when the caller
    // enters now.
    bool Hold(std::function<void()> enter);

    // Drops the held entry, or clears a reported failure.
    void Cancel();

    // Per frame: refreshes the snapshot and, once the build has ended, runs the held entry or
    // reports the failure.
    void Update();

    Phase GetPhase() const { return m_Phase; }
    const ScriptBuildSnapshot& GetSnapshot() const { return m_Snapshot; }

  private:
    BuildProbe m_Probe;
    std::function<void()> m_Enter;
    ScriptBuildSnapshot m_Snapshot;
    Phase m_Phase = Phase::Idle;
};

} // namespace GameEngine::Editor
