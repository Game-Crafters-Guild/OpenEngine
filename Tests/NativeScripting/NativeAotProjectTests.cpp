// The AOT project a ship-optimized build publishes compiles the project scripts
// and runtime package modules with the language settings of the live compile and
// the generated hot-reload csprojs, so code that compiles in the editor also
// compiles in the export. A runtime package module that ships a prebuilt
// assembly and no sources is linked in by reference instead.

#include "Engine/Build/NativeAotProject.h"

#include "ShippedScriptSources.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;

TEST(NativeAotProject, CarriesTheLiveCompileLanguageSettings)
{
    NativeAotProjectDesc desc;
    desc.ScriptSources = "X:/game/Assets";
    const std::string xml = GenerateNativeAotCsprojXml(desc);
    EXPECT_NE(xml.find("<AllowUnsafeBlocks>true</AllowUnsafeBlocks>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<ImplicitUsings>enable</ImplicitUsings>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<Nullable>enable</Nullable>"), std::string::npos) << xml;
}

// A module with no sources has no glob to add: its declared assembly is
// referenced and rooted, so ILC keeps its types and module initializers.
TEST(NativeAotProject, PrebuiltModuleWithoutSourcesIsReferencedAndRooted)
{
    PackageCodeModule module;
    module.AssemblyName = "ClosedPack";
    module.Lang = PackageModuleRecord::ModuleLang::CSharp;
    module.Kind = PackageModuleRecord::ModuleKind::Runtime;
    module.PrebuiltDir = "X:/packages/closed-pack/Binaries";

    NativeAotProjectDesc desc;
    desc.ScriptSources = "X:/game/Assets";
    desc.PackageModules = {module};
    const std::string xml = GenerateNativeAotCsprojXml(desc);
    EXPECT_EQ(xml.find("<Compile Include=\"**"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<Reference Include=\"ClosedPack\">\n"
                       "      <HintPath>X:/packages/closed-pack/Binaries/ClosedPack.dll</HintPath>"),
              std::string::npos)
        << xml;
    EXPECT_NE(xml.find("<TrimmerRootAssembly Include=\"ClosedPack\" />"), std::string::npos) << xml;
}

// A project with no authored scripts has no script glob in the AOT project. A
// glob that matches nothing reached the compiler as its literal text (CS2021,
// CS2001), which failed native-only exports whose runtime packages carry C#.
TEST(NativeAotProject, NoAuthoredScriptsAddsNoScriptGlob)
{
    PackageCodeModule module;
    module.AssemblyName = "WaterPack";
    module.Lang = PackageModuleRecord::ModuleLang::CSharp;
    module.Kind = PackageModuleRecord::ModuleKind::Runtime;
    module.RootDir = "X:/packages/water-pack/Scripts";

    NativeAotProjectDesc desc;
    desc.PackageModules = {module};
    const std::string xml = GenerateNativeAotCsprojXml(desc);
    EXPECT_NE(xml.find("<Compile Include=\"X:/packages/water-pack/Scripts/**/*.cs\""), std::string::npos) << xml;
    EXPECT_EQ(xml.find("<Compile Include=\"**"), std::string::npos) << xml;
    EXPECT_EQ(xml.find("<Compile Include=\"/**"), std::string::npos) << xml;
}

namespace
{
void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
} // namespace

// Whether an export compiles C# follows the authored scripts and the runtime C#
// package modules, never the generated project: the editor writes that project
// for native-only projects too, at the project root or in a relocated directory.
TEST(ShippedScriptSources, FollowTheAuthoredScriptsAndRuntimePackagesNotTheGeneratedProject)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "ge_shipped_script_sources";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path assets = root / "Assets";
    const std::string generatedProject =
        "<Project Sdk=\"Microsoft.NET.Sdk\"><ItemGroup><Compile Include=\"Assets/**/*.cs\" /></ItemGroup>"
        "<Target Name=\"DumpCompileItems\" /></Project>";
    WriteText(root / "GameEngine.Scripts.csproj", generatedProject);
    WriteText(root / "Generated" / "GameEngine.Scripts.csproj", generatedProject);
    WriteText(assets / "Scenes" / "Main.scene", "{}");
    // Build litter and caches are not authored scripts.
    WriteText(assets / "obj" / "Debug" / "GameEngine.Scripts.AssemblyInfo.cs", "// generated");
    WriteText(root / ".Cache" / "Staging" / "Copied.cs", "class Copied {}");

    EXPECT_TRUE(GameEngine::CollectShippedScriptSources(assets, {}).IsEmpty())
        << "a native-only project with an empty generated project has no C# to compile";

    PackageCodeModule runtimeModule;
    runtimeModule.AssemblyName = "WaterPack";
    runtimeModule.Lang = PackageModuleRecord::ModuleLang::CSharp;
    runtimeModule.Kind = PackageModuleRecord::ModuleKind::Runtime;
    runtimeModule.RootDir = "X:/packages/water-pack/Scripts";
    PackageCodeModule editorModule = runtimeModule;
    editorModule.AssemblyName = "WaterPack.Editor";
    editorModule.Kind = PackageModuleRecord::ModuleKind::Editor;
    PackageCodeModule nativeModule = runtimeModule;
    nativeModule.AssemblyName = "WaterPackNative";
    nativeModule.Lang = PackageModuleRecord::ModuleLang::Cpp;
    const auto packageOnly =
        GameEngine::CollectShippedScriptSources(assets, {runtimeModule, editorModule, nativeModule});
    EXPECT_TRUE(packageOnly.AuthoredRoot.empty());
    ASSERT_EQ(packageOnly.PackageModules.size(), 1u);
    EXPECT_EQ(packageOnly.PackageModules[0].AssemblyName, "WaterPack");

    WriteText(assets / "Scripts" / "Player.cs", "class Player {}");
    const auto authored = GameEngine::CollectShippedScriptSources(assets, {});
    EXPECT_EQ(authored.AuthoredRoot, assets) << "the scripts root, not the generated project's directory";

    fs::remove_all(root, ec);
}

// The export's step per source set: nothing to compile skips; a NativeAOT build
// compiles the sources into its own project, so it needs no generated project;
// the managed build compiles the generated project, so without one it refuses
// rather than ship a game without its C#.
TEST(ShippedScriptSources, PackageOnlyCSharpCompilesWithoutAGeneratedProject)
{
    using GameEngine::DecideScriptCompile;
    using GameEngine::ScriptCompileStep;
    PackageCodeModule runtimeModule;
    runtimeModule.AssemblyName = "WaterPack";
    runtimeModule.Lang = PackageModuleRecord::ModuleLang::CSharp;
    runtimeModule.Kind = PackageModuleRecord::ModuleKind::Runtime;
    runtimeModule.RootDir = "X:/packages/water-pack/Scripts";
    GameEngine::ShippedScriptSources none;
    GameEngine::ShippedScriptSources packageOnly;
    packageOnly.PackageModules = {runtimeModule};
    GameEngine::ShippedScriptSources authored;
    authored.AuthoredRoot = "X:/game/Assets";

    for (const bool projectExists : {false, true})
        for (const bool nativeAot : {false, true})
            EXPECT_EQ(DecideScriptCompile(none, projectExists, nativeAot), ScriptCompileStep::Skip);
    for (const auto* sources : {&packageOnly, &authored})
    {
        EXPECT_EQ(DecideScriptCompile(*sources, /*generatedProjectExists=*/true, /*nativeAot=*/true),
                  ScriptCompileStep::Compile);
        EXPECT_EQ(DecideScriptCompile(*sources, true, false), ScriptCompileStep::Compile);
        EXPECT_EQ(DecideScriptCompile(*sources, false, true), ScriptCompileStep::Compile)
            << "a NativeAOT build compiles the sources without the generated project";
        EXPECT_EQ(DecideScriptCompile(*sources, false, false), ScriptCompileStep::MissingProject)
            << "the managed build cannot compile without the generated project";
    }
}
