// The GameSDK's AI macros register a user module's names in the engine's AiRegistry through Engine.dll, stamped
// with the module's load, so that a reload retires a name the newer code does not register. The probe DLLs stand
// in for a user module (AiNamesProbe.cpp).
#include "AI/StateTree/StateTreeCompileError.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/Assets/AiOptionSetData.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeSource.h"
#include "AIECS/Components/AiAgent.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "NativeScripting/BuildCacheRecord.h"
#include "NativeScripting/EngineBuildIdentity.h"
#include "NativeScripting/NativeScriptManager.h"
#include "Scripting/ScriptsConfig.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
namespace fs = std::filesystem;
namespace ns = GameEngine::NativeScripting;
using namespace GameEngine;

constexpr const char* kModuleId = "AiNamesProbe";

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
        std::fflush(stderr);
        ::TerminateProcess(::GetCurrentProcess(), 17);
    }
}

fs::path ExecutableDirectory()
{
    wchar_t executable[32768]{};
    Check(::GetModuleFileNameW(nullptr, executable, 32768) != 0, "missing executable path");
    return fs::path(executable).parent_path();
}

// Points the build record at `dll` and loads it as the next generation of kModuleId.
bool LoadProbe(const fs::path& root, const fs::path& dll)
{
    Check(ns::WriteBuildCacheRecord(root / "NativeScripts" / "build",
                                    ns::BuildCacheRecord{"ai-names-test", dll.generic_string(), "",
                                                         ns::EngineBuildIdentity()}),
          "could not write the build record");
    return EngineCore::GetInstance().GetNativeScriptManager()->LoadPrebuiltUserModule(root, kModuleId);
}

// The engine with no CLR and no hot reload, in a scratch workspace under `root`.
void InitializeEngine(const fs::path& root)
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const fs::path workspace = root / "Workspace";
    std::error_code error;
    fs::create_directories(workspace, error);
    ApplicationConfig config{};
    config.WorkspaceDirectory = workspace.string();
    config.AssetDirectory = "Assets";
    ScriptsConfig scriptsConfig{};
    scriptsConfig.disableClr = true;
    scriptsConfig.enableHotReload = false;
    scriptsConfig.enableAutoProjectGeneration = false;
    EngineCore& engine = EngineCore::GetInstance();
    engine.SetScriptsConfig(scriptsConfig);
    Check(engine.Initialize(config), "EngineCore did not initialize");
}

fs::path ScratchRoot(const char* name)
{
    return fs::temp_directory_path() / (std::string(name) + std::to_string(::GetCurrentProcessId()));
}

void Finish(const fs::path& root)
{
    EngineCore::GetInstance().Shutdown();
    std::error_code error;
    fs::remove_all(root, error);
    ::TerminateProcess(::GetCurrentProcess(), 0);
}

// A program file that names the probe's input, task, condition and event.
AIECS::AiStateTreeSource ProbeNames()
{
    AIECS::AiStateTreeSource source;
    source.InputNames = {"ProbeThreat"};
    source.TaskNames = {"ProbeWalk"};
    source.ConditionNames = {"ProbeSeesEnemy"};
    source.EventNames = {"ProbeAlarm"};
    return source;
}

bool ListsInput(const AIECS::AiRegistry& registry, const std::string& name)
{
    const std::vector<AIECS::AiRegistry::NamedInput> inputs = registry.GetNamedInputs();
    return std::ranges::find(inputs, name, &AIECS::AiRegistry::NamedInput::Name) != inputs.end();
}

// A utility program with one option that runs the task ProbeWalk.
uint16 CompileWalkProgram(AIECS::AiRegistry& registry)
{
    AIECS::AiOptionSetData data;
    data.TaskNames = {"ProbeWalk"};
    data.Options.Options.push_back({.Weight = 1.0f, .Action = 0});
    return registry.CompileProgram(data);
}

void LoadRegistersEachNameWithItsCaptionAndUnit()
{
    const fs::path root = ScratchRoot("ge_ai_names_load_");
    InitializeEngine(root);
    const AIECS::AiRegistry& registry = AIECS::AiRegistry::Get();

    Check(LoadProbe(root, ExecutableDirectory() / "AiNamesProbe.dll"), "the probe did not load");

    Check(registry.CheckStateTreeNames(ProbeNames()).empty(), "a name of the probe does not resolve after its load");
    Check(registry.FindEventType("ProbeAlarm") != AIECS::AiRegistry::kNoId, "the probe's event has no id");
    const std::vector<AIECS::AiRegistry::NamedInput> inputs = registry.GetNamedInputs();
    const auto threat = std::ranges::find(inputs, std::string("ProbeThreat"), &AIECS::AiRegistry::NamedInput::Name);
    Check(threat != inputs.end() && threat->Caption == "Threat level" && threat->Unit == "m",
          "the probe's input does not list its caption and unit");
    Finish(root);
}

void ReloadRetiresTheNamesTheNewerCodeDropped()
{
    const fs::path root = ScratchRoot("ge_ai_names_reload_");
    InitializeEngine(root);
    AIECS::AiRegistry& registry = AIECS::AiRegistry::Get();
    Check(LoadProbe(root, ExecutableDirectory() / "AiNamesProbe.dll"), "the probe did not load");
    const uint16 walk = CompileWalkProgram(registry);
    Check(walk != Components::AiAgent::kNoProgram && registry.IsProgramRunnable(walk),
          "the program of the probe's task did not compile");

    Check(LoadProbe(root, ExecutableDirectory() / "AiNamesDroppedProbe.dll"), "the newer probe did not load");

    Check(!ListsInput(registry, "ProbeThreat"), "the reload kept the input that the newer code dropped");
    Check(!registry.IsProgramRunnable(walk), "the reload kept the task that the newer code dropped");
    const std::vector<std::string> events = registry.GetEventTypeNames();
    Check(std::ranges::find(events, std::string("ProbeAlarm")) == events.end(),
          "the reload kept the event that the newer code dropped");
    const std::vector<std::string> conditions = registry.GetConditionNames();
    Check(std::ranges::find(conditions, std::string("ProbeSeesEnemy")) != conditions.end(),
          "the reload retired the condition that the newer code keeps");
    Finish(root);
}

// EngineCore is a process singleton, so each scenario runs in a child process.
TEST(AiNamesModule, ALoadRegistersEachNameOfTheMacrosWithItsCaptionAndUnit)
{
    EXPECT_EXIT(LoadRegistersEachNameWithItsCaptionAndUnit(), ::testing::ExitedWithCode(0), "");
}

TEST(AiNamesModule, AReloadRetiresTheNamesThatTheNewerCodeDropped)
{
    EXPECT_EXIT(ReloadRetiresTheNamesTheNewerCodeDropped(), ::testing::ExitedWithCode(0), "");
}
} // namespace
