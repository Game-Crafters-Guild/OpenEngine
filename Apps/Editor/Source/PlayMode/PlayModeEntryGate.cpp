#include "PlayMode/PlayModeEntryGate.h"

#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "NativeScripting/NativeScriptManager.h"
#include "Scripting/ScriptManager.h"

#include <string_view>
#include <utility>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kManagedScriptsName = "C# scripts";

// "C:\Game\Assets\Scripts\Ball.cpp(296): error C2059: ..." -> "Ball.cpp(296): error C2059: ...".
// The location ends at the first ": " (a drive letter's colon is followed by a separator).
std::string WithoutDirectory(const std::string& line)
{
    const std::size_t locationEnd = line.find(": ");
    const std::size_t lastSeparator =
        line.find_last_of("/\\", locationEnd == std::string::npos ? std::string::npos : locationEnd);
    return lastSeparator == std::string::npos ? line : line.substr(lastSeparator + 1);
}

std::string FirstLine(const std::string& text)
{
    return text.substr(0, text.find('\n'));
}

// "Eztree: compiling (3 of 9 built)", or "UserScripts: compiling" for a batch of one.
std::string DescribeNativeBuild(const NativeScripting::NativeScriptManager::BuildProgress& progress)
{
    if (progress.Module.empty() || progress.Count == 0)
        return "Waiting for the native script build to start";
    std::string activity = progress.Module + ": " + (progress.Stage.empty() ? "building" : progress.Stage);
    if (progress.Count > 1)
        activity += " (" + std::to_string(progress.Index - 1) + " of " + std::to_string(progress.Count) + " built)";
    return activity;
}

} // namespace

ScriptBuildSnapshot ReadScriptBuildSnapshot()
{
    ScriptBuildSnapshot snapshot;
    EngineCore& engine = EngineCore::GetInstance();
    if (const NativeScripting::NativeScriptManager* native = engine.GetNativeScriptManager())
    {
        snapshot.FailedModules = native->FailedShippedModules();
        if (!snapshot.FailedModules.empty())
            snapshot.FirstError = WithoutDirectory(native->FirstBuildErrorLine(snapshot.FailedModules.front()));
        if (native->AreShippedModuleBuildsPending())
        {
            snapshot.Building = true;
            const NativeScripting::NativeScriptManager::BuildProgress progress = native->CurrentBuildProgress();
            snapshot.Activity = DescribeNativeBuild(progress);
            if (progress.Count > 1 && progress.Index > 0)
                snapshot.Fraction = static_cast<float>(progress.Index - 1) / static_cast<float>(progress.Count);
            return snapshot;
        }
    }
    ScriptManager& managed = engine.GetScriptManager();
    if (managed.IsAsyncHotReloadInProgress())
    {
        snapshot.Building = true;
        snapshot.Activity = std::string(kManagedScriptsName) + ": " + managed.GetAsyncHotReloadStatus();
        snapshot.Fraction = managed.GetAsyncHotReloadProgress();
        return snapshot;
    }
    const std::string managedError = managed.GetLastAsyncHotReloadError();
    if (!managedError.empty())
    {
        snapshot.FailedModules.push_back(kManagedScriptsName);
        if (snapshot.FirstError.empty())
            snapshot.FirstError = FirstLine(managedError);
    }
    return snapshot;
}

PlayModeEntryGate::PlayModeEntryGate(BuildProbe probe)
    : m_Probe(std::move(probe))
{
}

bool PlayModeEntryGate::Hold(std::function<void()> enter)
{
    m_Snapshot = m_Probe();
    if (m_Snapshot.Building)
    {
        if (m_Phase != Phase::Waiting)
            Logger::Log::Info("PlayMode: scripts are building; play mode starts when the build finishes");
        m_Enter = std::move(enter);
        m_Phase = Phase::Waiting;
        return true;
    }
    m_Enter = nullptr;
    if (!m_Snapshot.FailedModules.empty())
    {
        Logger::Log::Warning("PlayMode: not entering play mode; the latest script build failed");
        m_Phase = Phase::Failed;
        return true;
    }
    m_Phase = Phase::Idle;
    return false;
}

void PlayModeEntryGate::Cancel()
{
    if (m_Phase == Phase::Waiting)
        Logger::Log::Info("PlayMode: entering play mode canceled while scripts were building");
    m_Enter = nullptr;
    m_Phase = Phase::Idle;
}

void PlayModeEntryGate::Update()
{
    if (m_Phase != Phase::Waiting)
        return;
    m_Snapshot = m_Probe();
    if (m_Snapshot.Building)
        return;
    std::function<void()> enter = std::move(m_Enter);
    m_Enter = nullptr;
    if (!m_Snapshot.FailedModules.empty())
    {
        Logger::Log::Warning("PlayMode: not entering play mode; the script build failed");
        m_Phase = Phase::Failed;
        return;
    }
    m_Phase = Phase::Idle;
    if (enter)
        enter();
}

} // namespace GameEngine::Editor
