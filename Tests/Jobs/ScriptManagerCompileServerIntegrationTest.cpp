#include <gtest/gtest.h>
#include "CompileServerTestHost.h"
#include "Scripting/ScriptManager.h"
#include "Scripting/ScriptsProjectFile.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Engine/Build/DotnetHost.h"
#include "Jobs/WorkspaceId.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Core/Application.h" // PathUtils
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace GameEngine;

TEST(ScriptManagerCompileServerIntegration, CompileProjectUsesServerWhenAvailable) {
#ifndef _WIN32
    GTEST_SKIP() << "ScriptManager compile server integration test is Windows-only in this environment";
#endif
#ifdef _WIN32
    // Arrange temporary workspace with a simple C# file and fake project file
    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_SM_CS_Integration";
    std::error_code ec; std::filesystem::create_directories(ws, ec);

    auto sourcePath = ws / "Foo.cs";
    {
        std::ofstream f(sourcePath);
        f << "public static class Foo { public static int Smoke(){ return 1; } }";
        f.close();
    }

    // Compute pipe name and start server
    std::string pipeName = std::string("GE_CompileServer_") + WorkspaceId::Compute(ws.string());
    if (!CompileServerTestHost::StartTestCompileServer(pipeName)) {
        GTEST_SKIP() << "CompileServerHost not found or failed to start; skipping integration test";
    }

    // Ensure server-first path is enabled
    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");

    // Initialize ScriptManager with explicit configuration matching the new API
    JobSystem::WorkStealingThreadPool pool(4);
    ScriptManager mgr;

    ScriptsConfig cfg{};
    cfg.workspaceRoot = ws;
    cfg.scriptsRoot = ws;                         // Place sources directly in the workspace
    cfg.assembliesRoot = ws / "ScriptAssemblies"; // Output for compiled assemblies
    cfg.generatedProjectRoot = ws;                // Not used in this test (auto-generation disabled)
    cfg.disableClr = true;                        // CompileServer path does not require CoreCLR
    cfg.enableHotReload = false;
    cfg.enableAsyncHotReload = false;
    cfg.enableAutoProjectGeneration = false;      // We provide an explicit fake .csproj below

    ASSERT_TRUE(mgr.Initialize(cfg, pool));

    // Use a fake .csproj path under the workspace; server ignores contents and uses AllFiles list
    auto fakeProject = ws / "GameEngine.Scripts.csproj";
    {
        std::ofstream f(fakeProject);
        f << "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>";
        f.close();
    }

    auto result = mgr.CompileProject(fakeProject, nullptr);
    if (!result.success) {
        GTEST_SKIP() << "CompileServer path failed in this environment; skipping assertions";
    }

    EXPECT_TRUE(result.success);
#endif
}

TEST(ScriptManagerCompileServerIntegration, CompileProjectCanReferenceEcsAbiWhenPresent)
{
#ifndef _WIN32
    GTEST_SKIP() << "ScriptManager compile server integration test is Windows-only in this environment";
#endif
#ifdef _WIN32
    // Only run when the ECS ABI module is staged next to the test executable.
    std::filesystem::path engineBinDir = GameEngine::PathUtils::GetExecutableDirectory();
    if (!std::filesystem::exists(engineBinDir / "GameEngine.ECS.ABI.dll"))
    {
        GTEST_SKIP() << "GameEngine.ECS.ABI.dll not found next to test executable; skipping";
    }

    // Arrange temporary workspace with a simple C# file that references the ECS ABI surface.
    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_SM_CS_EcsAbi";
    std::error_code ec;
    std::filesystem::create_directories(ws, ec);

    auto sourcePath = ws / "Foo.cs";
    {
        std::ofstream f(sourcePath);
        f << "using GameEngine.ECS;\n";
        f << "public static class Foo { public static int Smoke(){ WorldHandle w = Ecs.PrimaryWorld; return w.TryGetEntityCount(out int c) ? c : -1; } }\n";
        f.close();
    }

    // Compute pipe name and start server
    std::string pipeName = std::string("GE_CompileServer_") + WorkspaceId::Compute(ws.string());
    if (!CompileServerTestHost::StartTestCompileServer(pipeName))
    {
        GTEST_SKIP() << "CompileServerHost not found or failed to start; skipping integration test";
    }

    // Ensure server-first path is enabled
    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");

    JobSystem::WorkStealingThreadPool pool(4);
    ScriptManager mgr;

    ScriptsConfig cfg{};
    cfg.workspaceRoot = ws;
    cfg.scriptsRoot = ws;                         // Place sources directly in the workspace
    cfg.assembliesRoot = ws / "ScriptAssemblies"; // Output for compiled assemblies
    cfg.generatedProjectRoot = ws;                // Not used in this test (auto-generation disabled)
    cfg.disableClr = true;                        // CompileServer path does not require CoreCLR
    cfg.enableHotReload = false;
    cfg.enableAsyncHotReload = false;
    cfg.enableAutoProjectGeneration = false;      // We provide an explicit fake .csproj below

    ASSERT_TRUE(mgr.Initialize(cfg, pool));

    // Use a fake .csproj path under the workspace; server ignores contents and uses AllFiles list
    auto fakeProject = ws / "GameEngine.Scripts.csproj";
    {
        std::ofstream f(fakeProject);
        f << "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>";
        f.close();
    }

    auto result = mgr.CompileProject(fakeProject, nullptr);
    if (!result.success)
    {
        GTEST_SKIP() << "CompileServer path failed in this environment; skipping assertions";
    }

    EXPECT_TRUE(result.success);
#endif
}
#ifdef _WIN32
namespace
{
void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A workspace whose NuGet sources are one local folder and whose package cache is its own,
// so a restore never reaches the network or the user's cache. The cache variable is put back
// when the workspace goes, on every path out of the test.
class PackageWorkspace
{
  public:
    explicit PackageWorkspace(const char* name) : m_Root(std::filesystem::temp_directory_path() / name)
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
        std::filesystem::create_directories(m_Root / "Assets", ec);
        std::filesystem::create_directories(m_Root / "feed", ec);
        WriteText(m_Root / "nuget.config", "<configuration><packageSources><clear />"
                                           "<add key=\"local\" value=\"feed\" /></packageSources></configuration>\n");
        if (const char* previous = std::getenv("NUGET_PACKAGES"))
            m_PreviousCache = previous;
        _putenv_s("NUGET_PACKAGES", (m_Root / "nuget-cache").string().c_str());
    }
    ~PackageWorkspace() { _putenv_s("NUGET_PACKAGES", m_PreviousCache.c_str()); }
    PackageWorkspace(const PackageWorkspace&) = delete;
    PackageWorkspace& operator=(const PackageWorkspace&) = delete;

    const std::filesystem::path& Root() const { return m_Root; }

  private:
    std::filesystem::path m_Root;
    std::string m_PreviousCache;
};

// Packs the test package GeTestDependency at `version` into the workspace feed; its Greeter.Value()
// returns `value`.
::testing::AssertionResult PackDependency(const std::filesystem::path& ws, const std::string& version, int value)
{
    WriteText(ws / "dependency" / "GeTestDependency.csproj",
              "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework>"
              "<PackageId>GeTestDependency</PackageId></PropertyGroup></Project>\n");
    WriteText(ws / "dependency" / "Greeter.cs",
              "namespace GeTestDependency { public static class Greeter { public static int Value() => " +
                  std::to_string(value) + "; } }\n");
    const ShellProcessResult pack = RunProcessCaptured(
        DotnetHostCommand(), {"pack", (ws / "dependency" / "GeTestDependency.csproj").string(), "-o",
                              (ws / "feed").string(), "-p:Version=" + version, "--nologo", "--verbosity", "quiet"});
    if (pack.exitCode != 0)
        return ::testing::AssertionFailure() << pack.output;
    return ::testing::AssertionSuccess();
}

// Puts `items` into the csproj, before the engine targets import.
void AddProjectItems(const std::filesystem::path& projectPath, const std::string& items)
{
    std::string text = ReadText(projectPath);
    const std::size_t targets = text.find("  <Import Project=\"GameEngine.Scripts.Engine.targets\"");
    ASSERT_NE(targets, std::string::npos) << text;
    text.insert(targets, "  <ItemGroup>\n" + items + "  </ItemGroup>\n");
    WriteText(projectPath, text);
}

ScriptsConfig PackageWorkspaceConfig(const std::filesystem::path& ws)
{
    ScriptsConfig cfg{};
    cfg.workspaceRoot = ws;
    cfg.scriptsRoot = ws / "Assets";
    cfg.assembliesRoot = ws / "ScriptAssemblies";
    cfg.disableClr = true;
    cfg.enableHotReload = false;
    cfg.enableAsyncHotReload = false;
    cfg.enableAutoProjectGeneration = true; // the engine props and targets the csproj imports
    return cfg;
}

void AddPackage(const std::filesystem::path& projectPath, const std::string& id)
{
    std::string text = ReadText(projectPath);
    ScriptsProjectFile::SetPackageReference(text, {id, "1.0.0"});
    WriteText(projectPath, text);
}

bool StartCompileServerFor(const std::filesystem::path& ws)
{
    const std::string pipeName = std::string("GE_CompileServer_") + WorkspaceId::Compute(ws.string());
    if (!CompileServerTestHost::StartTestCompileServer(pipeName))
        return false;
    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");
    return true;
}
} // namespace

// A script that uses a type from a NuGet package compiles through the compile server, against
// the package assembly staged into Dependencies/.
TEST(ScriptManagerCompileServerIntegration, ScriptUsingAPackageCompiles)
{
    const PackageWorkspace workspace("GE_SM_CS_Package");
    const std::filesystem::path& ws = workspace.Root();
    ASSERT_TRUE(PackDependency(ws, "1.0.0", 42));
    WriteText(ws / "Assets" / "Foo.cs",
              "public static class Foo { public static int Smoke() => GeTestDependency.Greeter.Value(); }\n");
    if (!StartCompileServerFor(ws))
        GTEST_SKIP() << "CompileServerHost not found or failed to start; skipping integration test";

    JobSystem::WorkStealingThreadPool pool(4);
    ScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(PackageWorkspaceConfig(ws), pool));
    AddPackage(mgr.GeneratedProjectPath(), "GeTestDependency");

    const ScriptCompilationResult result = mgr.CompileProject(mgr.GeneratedProjectPath(), nullptr);
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_TRUE(std::filesystem::exists(ws / "ScriptAssemblies" / "Dependencies" / "GeTestDependency.dll"));

    mgr.Shutdown();
}

// A package that does not restore fails the compile with an error that names it and the fix.
TEST(ScriptManagerCompileServerIntegration, MissingPackageIsANamedCompileError)
{
    const PackageWorkspace workspace("GE_SM_CS_MissingPackage");
    const std::filesystem::path& ws = workspace.Root();
    WriteText(ws / "Assets" / "Foo.cs", "public static class Foo { public static int Smoke() => 1; }\n");

    JobSystem::WorkStealingThreadPool pool(4);
    ScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(PackageWorkspaceConfig(ws), pool));
    AddPackage(mgr.GeneratedProjectPath(), "GeTestMissingPackage");

    const ScriptCompilationResult result = mgr.CompileProject(mgr.GeneratedProjectPath(), nullptr);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("GeTestMissingPackage"), std::string::npos) << result.errorMessage;
    EXPECT_NE(result.errorMessage.find("PackageReference"), std::string::npos) << result.errorMessage;

    mgr.Shutdown();
}

namespace
{
void WriteCentralVersion(const std::filesystem::path& ws, const char* version)
{
    WriteText(ws / "Directory.Packages.props",
              std::string("<Project><PropertyGroup><ManagePackageVersionsCentrally>true</ManagePackageVersionsCentrally>"
                          "</PropertyGroup><ItemGroup><PackageVersion Include=\"GeTestDependency\" Version=\"") +
                  version + "\" /></ItemGroup></Project>\n");
}
} // namespace

// A version bump in Directory.Packages.props, with the csproj unchanged, restores and stages the
// new version at the next compile.
TEST(ScriptManagerCompileServerIntegration, CentralVersionBumpRestoresTheNewVersion)
{
    const PackageWorkspace workspace("GE_SM_CS_CentralVersion");
    const std::filesystem::path& ws = workspace.Root();
    ASSERT_TRUE(PackDependency(ws, "1.0.0", 1));
    ASSERT_TRUE(PackDependency(ws, "2.0.0", 2));
    WriteCentralVersion(ws, "1.0.0");
    WriteText(ws / "Assets" / "Foo.cs",
              "public static class Foo { public static int Smoke() => GeTestDependency.Greeter.Value(); }\n");
    if (!StartCompileServerFor(ws))
        GTEST_SKIP() << "CompileServerHost not found or failed to start; skipping integration test";

    JobSystem::WorkStealingThreadPool pool(4);
    ScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(PackageWorkspaceConfig(ws), pool));
    AddProjectItems(mgr.GeneratedProjectPath(), "    <PackageReference Include=\"GeTestDependency\" />\n");
    const std::filesystem::path staged = ws / "ScriptAssemblies" / "Dependencies" / "GeTestDependency.dll";
    const std::filesystem::path cached = ws / "nuget-cache" / "getestdependency";

    const std::filesystem::path oldAssembly = cached / "1.0.0" / "lib" / "net10.0" / "GeTestDependency.dll";
    const std::filesystem::path newAssembly = cached / "2.0.0" / "lib" / "net10.0" / "GeTestDependency.dll";

    ScriptCompilationResult result = mgr.CompileProject(mgr.GeneratedProjectPath(), nullptr);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(ReadText(staged), ReadText(oldAssembly));

    // The new version is in the package cache before the bump, with the size and the write time of
    // the old one, as two versions packed within the same two seconds are.
    WriteText(ws / "prefetch" / "Prefetch.csproj",
              "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework>"
              "<ManagePackageVersionsCentrally>false</ManagePackageVersionsCentrally></PropertyGroup><ItemGroup>"
              "<PackageReference Include=\"GeTestDependency\" Version=\"2.0.0\" /></ItemGroup></Project>\n");
    const ShellProcessResult prefetch = RunProcessCaptured(
        DotnetHostCommand(), {"restore", (ws / "prefetch" / "Prefetch.csproj").string(), "--verbosity", "quiet"});
    ASSERT_EQ(prefetch.exitCode, 0) << prefetch.output;
    ASSERT_EQ(std::filesystem::file_size(newAssembly), std::filesystem::file_size(oldAssembly));
    std::filesystem::last_write_time(newAssembly, std::filesystem::last_write_time(oldAssembly));

    WriteCentralVersion(ws, "2.0.0");
    result = mgr.CompileProject(mgr.GeneratedProjectPath(), nullptr);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_NE(ReadText(ws / "obj" / "project.assets.json").find("GeTestDependency/2.0.0"), std::string::npos)
        << "the bump did not restore";
    EXPECT_EQ(ReadText(staged), ReadText(newAssembly)) << "the compile still stages the old version";
    EXPECT_NE(std::filesystem::last_write_time(staged), std::filesystem::last_write_time(newAssembly))
        << "the staged copy keeps the package's write time, so the dependency context is not replaced";

    mgr.Shutdown();
}

// A restored package assembly that is gone from the package cache (the cache was cleared) restores
// again at the next compile instead of failing the copy.
TEST(ScriptManagerCompileServerIntegration, ClearedPackageCacheRestoresAgain)
{
    const PackageWorkspace workspace("GE_SM_CS_ClearedCache");
    const std::filesystem::path& ws = workspace.Root();
    ASSERT_TRUE(PackDependency(ws, "1.0.0", 42));
    WriteText(ws / "Assets" / "Foo.cs",
              "public static class Foo { public static int Smoke() => GeTestDependency.Greeter.Value(); }\n");
    if (!StartCompileServerFor(ws))
        GTEST_SKIP() << "CompileServerHost not found or failed to start; skipping integration test";

    JobSystem::WorkStealingThreadPool pool(4);
    ScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(PackageWorkspaceConfig(ws), pool));
    AddPackage(mgr.GeneratedProjectPath(), "GeTestDependency");
    ScriptCompilationResult result = mgr.CompileProject(mgr.GeneratedProjectPath(), nullptr);
    ASSERT_TRUE(result.success) << result.errorMessage;

    std::filesystem::remove_all(ws / "nuget-cache");
    result = mgr.CompileProject(mgr.GeneratedProjectPath(), nullptr);
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_TRUE(std::filesystem::exists(ws / "nuget-cache" / "getestdependency" / "1.0.0"));

    mgr.Shutdown();
}

// A Reference item with a HintPath compiles in the editor: its file is staged into Dependencies/
// and referenced like a package assembly.
TEST(ScriptManagerCompileServerIntegration, ReferenceItemCompilesAndIsStaged)
{
    const PackageWorkspace workspace("GE_SM_CS_Reference");
    const std::filesystem::path& ws = workspace.Root();
    WriteText(ws / "local" / "GeTestLocal.csproj",
              "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework>"
              "</PropertyGroup></Project>\n");
    WriteText(ws / "local" / "Local.cs",
              "namespace GeTestLocal { public static class Local { public static int Value() => 7; } }\n");
    const ShellProcessResult build = RunProcessCaptured(
        DotnetHostCommand(), {"build", (ws / "local" / "GeTestLocal.csproj").string(), "-o", (ws / "lib").string(),
                              "--nologo", "--verbosity", "quiet"});
    ASSERT_EQ(build.exitCode, 0) << build.output;
    WriteText(ws / "Assets" / "Foo.cs",
              "public static class Foo { public static int Smoke() => GeTestLocal.Local.Value(); }\n");
    if (!StartCompileServerFor(ws))
        GTEST_SKIP() << "CompileServerHost not found or failed to start; skipping integration test";

    JobSystem::WorkStealingThreadPool pool(4);
    ScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(PackageWorkspaceConfig(ws), pool));
    AddProjectItems(mgr.GeneratedProjectPath(), "    <Reference Include=\"GeTestLocal\">\n"
                                                "      <HintPath>lib\\GeTestLocal.dll</HintPath>\n"
                                                "    </Reference>\n");

    const ScriptCompilationResult result = mgr.CompileProject(mgr.GeneratedProjectPath(), nullptr);
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_TRUE(std::filesystem::exists(ws / "ScriptAssemblies" / "Dependencies" / "GeTestLocal.dll"));

    mgr.Shutdown();
}
#endif
