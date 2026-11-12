// EngineCore's module handlers revoke the terrain height functions of a module load: an aborted load
// loses only its own names, and a reload revokes the instances the older load built while its image
// is still mapped. The probe DLLs stand in for a user module (TerrainHeightFunctionProbe.cpp).
#include "Core/Application.h"
#include "Core/Engine.h"
#include "NativeScripting/BuildCacheRecord.h"
#include "NativeScripting/EngineBuildIdentity.h"
#include "NativeScripting/NativeScriptManager.h"
#include "Scripting/ScriptsConfig.h"
#include "TerrainECS/TerrainHeightFunction/RevocableTerrainHeightFunction.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainHeightFunction/TerrainHeightFunctionInstances.h"
#include "TerrainHeightFunction/TerrainHeightFunctionRegistry.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace
{
namespace fs = std::filesystem;
namespace ns = GameEngine::NativeScripting;
using namespace GameEngine;
using TerrainECS::RevocableTerrainHeightFunction;
using TerrainECS::TerrainHeightFunctionInstances;
using TerrainECS::TerrainHeightFunctionRegistry;

constexpr const char* kModuleId = "TerrainHeightFunctionProbe";
constexpr uint64 kSeed = 3u;
// TerrainHeightFunctionProbe.cpp: height = 0.5 * x + 2 * seed.
constexpr float32 kSampleX = 4.0f;
constexpr float32 kExpectedHeight = 0.5f * kSampleX + 2.0f * static_cast<float32>(kSeed);
constexpr auto kBuildTimeout = std::chrono::seconds(5);

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
                                    ns::BuildCacheRecord{"terrain-height-function-test", dll.generic_string(), "",
                                                         ns::EngineBuildIdentity()}),
          "could not write the build record");
    return EngineCore::GetInstance().GetNativeScriptManager()->LoadPrebuiltUserModule(root, kModuleId);
}

// Requests the ProbeSlope instance for kSeed and waits for its build on the job system.
std::shared_ptr<const RevocableTerrainHeightFunction> BuildProbeSlope()
{
    const auto registration = TerrainHeightFunctionRegistry::Get().Find("ProbeSlope"_sid);
    Check(registration.has_value(), "ProbeSlope is not registered");
    const TerrainECS::TerrainHeightFunctionKey key{"ProbeSlope"_sid, registration->Generation, {kSeed, 64.0f, 64.0f}};
    TerrainHeightFunctionInstances& instances = TerrainECS::TerrainService::Get().GetHeightFunctionInstances();
    const auto deadline = std::chrono::steady_clock::now() + kBuildTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto function = instances.Request(key, *registration))
            return function;
        std::this_thread::yield();
    }
    Check(false, "the ProbeSlope instance did not build");
    return nullptr;
}

bool ReadsTheProbeHeight(const RevocableTerrainHeightFunction& function)
{
    const float32 x[1] = {kSampleX};
    float32 out[1] = {-1.0f};
    return function.HeightRow(0.0f, x, 0.0f, out) && out[0] == kExpectedHeight;
}

void AbortAndReloadRevokeOnlyTheirLoadsFunctions()
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const fs::path root =
        fs::temp_directory_path() / ("ge_terrain_height_function_module_" + std::to_string(::GetCurrentProcessId()));
    const fs::path workspace = root / "Workspace";
    std::error_code error;
    fs::create_directories(workspace, error);
    const fs::path probe = ExecutableDirectory() / "TerrainHeightFunctionProbe.dll";
    const fs::path refused = ExecutableDirectory() / "TerrainHeightFunctionRefusedProbe.dll";
    // A reload maps a new file: the editor's build writes each version to its own path.
    const fs::path reloaded = root / "TerrainHeightFunctionProbeReloaded.dll";
    Check(fs::copy_file(probe, reloaded, fs::copy_options::overwrite_existing, error), "could not copy the probe");

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
    // EnableRenderingLoop starts the world services; this process has no rendering loop.
    TerrainECS::TerrainService::Initialize();
    const TerrainHeightFunctionRegistry& registry = TerrainHeightFunctionRegistry::Get();

    Check(LoadProbe(root, probe), "the probe did not load");
    const auto first = registry.Find("ProbeSlope"_sid);
    Check(first.has_value() && first->Owner.ModuleId == kModuleId && first->Owner.Generation == 1u,
          "the first load did not register ProbeSlope with its stamp");
    const auto firstFunction = BuildProbeSlope();
    Check(ReadsTheProbeHeight(*firstFunction), "the first instance did not answer the probe height");

    Check(!LoadProbe(root, refused), "the refused probe loaded");
    Check(!registry.Find("ProbeRefusedSlope"_sid).has_value(), "the aborted load's name survived its purge");
    Check(registry.Find("ProbeSlope"_sid).has_value() && registry.Find("ProbeSlope"_sid)->Owner.Generation == 1u,
          "the aborted load's purge removed an older load's name");
    Check(ReadsTheProbeHeight(*firstFunction), "the aborted load's purge revoked an older load's instance");

    Check(LoadProbe(root, reloaded), "the reloaded probe did not load");
    const auto second = registry.Find("ProbeSlope"_sid);
    Check(second.has_value() && second->Owner.Generation == 3u && second->Generation != first->Generation,
          "the reload did not register ProbeSlope with a new generation");
    const float32 x[1] = {kSampleX};
    float32 out[1] = {-1.0f};
    Check(!firstFunction->HeightRow(0.0f, x, 0.0f, out) && out[0] == -1.0f,
          "the reload did not revoke the instance the older load built");
    Check(ReadsTheProbeHeight(*BuildProbeSlope()), "the reloaded function did not build a new instance");

    engine.Shutdown();
    fs::remove_all(root, error);
    ::TerminateProcess(::GetCurrentProcess(), 0);
}

// EngineCore is a process singleton, so the scenario runs in a child process.
TEST(TerrainHeightFunctionModule, AnAbortedLoadLosesOnlyItsNamesAndAReloadRevokesTheOlderInstances)
{
    EXPECT_EXIT(AbortAndReloadRevokeOnlyTheirLoadsFunctions(), ::testing::ExitedWithCode(0), "");
}
} // namespace
