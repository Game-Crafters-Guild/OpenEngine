// The text edits of the user's scripts csproj and of a Directory.Packages.props: what the Script
// Packages settings page and the editor's import check change, and that they change nothing else. Then how the package restore
// reads its outputs: the restore's assets file and the project's evaluated references.
#include <gtest/gtest.h>

#include "Scripting/ScriptPackageRestore.h"
#include "Scripting/ScriptsProjectFile.h"

#include "../../Engine/Source/Scripting/ScriptPackageRestoreInternal.h"
#include "../../Engine/Source/Scripting/ScriptsProjectFileInternal.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace GameEngine;

namespace
{
const std::string kProject = "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
                             "  <Import Project=\"GameEngine.Scripts.Engine.props\" />\n"
                             "  <!-- <PackageReference Include=\"Commented.Out\" Version=\"1.0.0\" /> -->\n"
                             "  <ItemGroup>\n"
                             "    <PackageReference Include=\"Humanizer.Core\" Version=\"2.14.1\" />\n"
                             "    <PackageReference Include=\"Newtonsoft.Json\">\n"
                             "      <Version>13.0.3</Version>\n"
                             "    </PackageReference>\n"
                             "  </ItemGroup>\n"
                             "  <Import Project=\"GameEngine.Scripts.Engine.targets\" />\n"
                             "</Project>\n";
} // namespace

TEST(ScriptsProjectFile, ReadsPackageReferencesAndSkipsComments)
{
    const std::vector<ScriptsProjectFile::PackageReference> packages = ScriptsProjectFile::ReadPackageReferences(kProject);
    ASSERT_EQ(packages.size(), 2u);
    EXPECT_EQ(packages[0].Id, "Humanizer.Core");
    EXPECT_EQ(packages[0].Version, "2.14.1");
    EXPECT_EQ(packages[1].Id, "Newtonsoft.Json");
    EXPECT_EQ(packages[1].Version, "13.0.3");
}

TEST(ScriptsProjectFile, SetChangesOnlyTheVersionOfAnExistingPackage)
{
    std::string text = kProject;
    ScriptsProjectFile::SetPackageReference(text, {"humanizer.core", "3.0.0"});
    std::string expected = kProject;
    expected.replace(expected.find("2.14.1"), 6, "3.0.0");
    EXPECT_EQ(text, expected);

    ScriptsProjectFile::SetPackageReference(text, {"Newtonsoft.Json", "13.0.4"});
    expected.replace(expected.find("13.0.3"), 6, "13.0.4");
    EXPECT_EQ(text, expected);
}

TEST(ScriptsProjectFile, SetAddsAfterTheLastPackageOrInANewItemGroup)
{
    std::string text = kProject;
    ScriptsProjectFile::SetPackageReference(text, {"Serilog", "4.0.0"});
    std::string expected = kProject;
    const std::string last = "    </PackageReference>";
    expected.insert(expected.find(last) + last.size(), "\n    <PackageReference Include=\"Serilog\" Version=\"4.0.0\" />");
    EXPECT_EQ(text, expected);

    std::string fresh = ScriptsProjectFile::NewProjectText();
    ScriptsProjectFile::SetPackageReference(fresh, {"Serilog", "4.0.0"});
    const std::size_t group = fresh.find("  <ItemGroup>\n    <PackageReference Include=\"Serilog\" Version=\"4.0.0\" />\n"
                                         "  </ItemGroup>\n  <Import Project=\"GameEngine.Scripts.Engine.targets\" />");
    EXPECT_NE(group, std::string::npos) << fresh;
    EXPECT_EQ(ScriptsProjectFile::ReadPackageReferences(fresh).size(), 1u);
}

namespace
{
const std::string kPackagesProps = "<Project>\n"
                                   "  <!-- <PackageVersion Include=\"Commented.Out\" Version=\"1.0.0\" /> -->\n"
                                   "  <ItemGroup>\n"
                                   "    <PackageVersion Include=\"Humanizer.Core\" Version=\"2.14.1\" />\n"
                                   "  </ItemGroup>\n"
                                   "</Project>\n";
} // namespace

TEST(ScriptsProjectFile, ReadsPackageVersionsAndSkipsComments)
{
    const std::vector<ScriptsProjectFile::PackageVersion> versions = ScriptsProjectFile::ReadPackageVersions(kPackagesProps);
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_EQ(versions[0].Id, "Humanizer.Core");
    EXPECT_EQ(versions[0].Version, "2.14.1");
    EXPECT_TRUE(ScriptsProjectFile::ReadPackageVersions(kProject).empty()) << "a PackageReference is not a PackageVersion";
}

TEST(ScriptsProjectFile, SetPackageVersionChangesAddsAndStartsAnItemGroup)
{
    std::string text = kPackagesProps;
    ScriptsProjectFile::SetPackageVersion(text, {"humanizer.core", "3.0.0"});
    std::string expected = kPackagesProps;
    expected.replace(expected.find("2.14.1"), 6, "3.0.0");
    EXPECT_EQ(text, expected);

    ScriptsProjectFile::SetPackageVersion(text, {"Serilog", "4.0.0"});
    const std::string last = "Version=\"3.0.0\" />";
    expected.insert(expected.find(last) + last.size(), "\n    <PackageVersion Include=\"Serilog\" Version=\"4.0.0\" />");
    EXPECT_EQ(text, expected);

    std::string bare = "<Project>\n</Project>\n";
    ScriptsProjectFile::SetPackageVersion(bare, {"Serilog", "4.0.0"});
    EXPECT_EQ(bare, "<Project>\n  <ItemGroup>\n    <PackageVersion Include=\"Serilog\" Version=\"4.0.0\" />\n"
                    "  </ItemGroup>\n</Project>\n");
}

TEST(ScriptsProjectFile, SetKeepsWindowsLineEndings)
{
    std::string text = "<Project Sdk=\"Microsoft.NET.Sdk\">\r\n"
                       "  <ItemGroup>\r\n"
                       "    <PackageReference Include=\"A\" Version=\"1.0.0\" />\r\n"
                       "  </ItemGroup>\r\n"
                       "</Project>\r\n";
    ScriptsProjectFile::SetPackageReference(text, {"B", "2.0.0"});
    EXPECT_NE(text.find("Version=\"1.0.0\" />\r\n    <PackageReference Include=\"B\" Version=\"2.0.0\" />\r\n"),
              std::string::npos)
        << text;
}

TEST(ScriptsProjectFile, RemoveTakesTheWholeLine)
{
    std::string text = kProject;
    EXPECT_TRUE(ScriptsProjectFile::RemovePackageReference(text, "Humanizer.Core"));
    std::string expected = kProject;
    const std::string line = "    <PackageReference Include=\"Humanizer.Core\" Version=\"2.14.1\" />\n";
    expected.erase(expected.find(line), line.size());
    EXPECT_EQ(text, expected);

    EXPECT_TRUE(ScriptsProjectFile::RemovePackageReference(text, "Newtonsoft.Json"));
    EXPECT_TRUE(ScriptsProjectFile::ReadPackageReferences(text).empty());
    EXPECT_FALSE(ScriptsProjectFile::RemovePackageReference(text, "Commented.Out"));
}

TEST(ScriptsProjectFile, ReconcilePutsBackOnlyTheMissingImports)
{
    std::string untouched = kProject;
    const ScriptsProjectFile::ReconcileResult none = ScriptsProjectFile::ReconcileEngineImports(untouched);
    EXPECT_FALSE(none.PropsImportAdded || none.TargetsImportAdded || none.SdkMissing);
    EXPECT_EQ(untouched, kProject);

    std::string bare = "<Project Sdk=\"Microsoft.NET.Sdk\">\n  <PropertyGroup />\n</Project>\n";
    const ScriptsProjectFile::ReconcileResult both = ScriptsProjectFile::ReconcileEngineImports(bare);
    EXPECT_TRUE(both.PropsImportAdded);
    EXPECT_TRUE(both.TargetsImportAdded);
    EXPECT_EQ(bare, "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
                    "  <Import Project=\"GameEngine.Scripts.Engine.props\" />\n"
                    "  <PropertyGroup />\n"
                    "  <Import Project=\"GameEngine.Scripts.Engine.targets\" />\n"
                    "</Project>\n");

    std::string noSdk = "<Project>\n  <Import Project=\"GameEngine.Scripts.Engine.props\" />\n"
                        "  <Import Project=\"GameEngine.Scripts.Engine.targets\" />\n</Project>\n";
    const std::string before = noSdk;
    EXPECT_TRUE(ScriptsProjectFile::ReconcileEngineImports(noSdk).SdkMissing);
    EXPECT_EQ(noSdk, before);
}

TEST(ScriptsProjectFile, PackageIdsAndVersionsAreValidated)
{
    EXPECT_TRUE(ScriptsProjectFile::IsValidPackageId("Newtonsoft.Json"));
    EXPECT_TRUE(ScriptsProjectFile::IsValidPackageId("_Private-Pkg.2"));
    EXPECT_FALSE(ScriptsProjectFile::IsValidPackageId(""));
    EXPECT_FALSE(ScriptsProjectFile::IsValidPackageId("Bad\"Id"));
    EXPECT_FALSE(ScriptsProjectFile::IsValidPackageId(".Leading"));
    EXPECT_TRUE(ScriptsProjectFile::IsValidPackageVersion("13.0.3"));
    EXPECT_TRUE(ScriptsProjectFile::IsValidPackageVersion("[1.0, 2.0)"));
    EXPECT_TRUE(ScriptsProjectFile::IsValidPackageVersion("2.0.0-beta.1+sha"));
    EXPECT_FALSE(ScriptsProjectFile::IsValidPackageVersion(""));
    EXPECT_FALSE(ScriptsProjectFile::IsValidPackageVersion("1.0\" />"));
}

TEST(ScriptPackageRestore, ReadsTheRuntimeAssembliesOfTheTarget)
{
    const std::string assets = R"({
      "version": 3,
      "targets": {
        "net10.0": {
          "Humanizer.Core/2.14.1": { "type": "package", "runtime": { "lib/net6.0/Humanizer.dll": {} } },
          "Empty.Shim/1.0.0": { "type": "package", "runtime": { "lib/net10.0/_._": {} } },
          "Other.Project/1.0.0": { "type": "project", "runtime": { "bin/placeholder/Other.Project.dll": {} } }
        }
      },
      "libraries": {
        "Humanizer.Core/2.14.1": { "type": "package", "path": "humanizer.core/2.14.1" },
        "Empty.Shim/1.0.0": { "type": "package", "path": "empty.shim/1.0.0" },
        "Other.Project/1.0.0": { "type": "project", "path": "../Other/Other.csproj" }
      },
      "packageFolders": { "C:\\Users\\dev\\.nuget\\packages\\": {} }
    })";
    std::vector<std::filesystem::path> assemblies;
    std::vector<std::string> warnings;
    std::string error;
    ASSERT_TRUE(ReadRestoredRuntimeAssemblies(assets, "net10.0", assemblies, warnings, error)) << error;
    EXPECT_TRUE(warnings.empty());
    ASSERT_EQ(assemblies.size(), 1u);
    EXPECT_EQ(assemblies[0],
              (std::filesystem::path("C:\\Users\\dev\\.nuget\\packages\\") / "humanizer.core/2.14.1/lib/net6.0/Humanizer.dll")
                  .lexically_normal());

    assemblies.clear();
    EXPECT_FALSE(ReadRestoredRuntimeAssemblies(assets, "net8.0", assemblies, warnings, error));
    EXPECT_NE(error.find("net8.0"), std::string::npos);
    EXPECT_FALSE(ReadRestoredRuntimeAssemblies("not json", "net10.0", assemblies, warnings, error));
}

// A package with native or platform-specific assets stages its assemblies and is named once in a
// warning with the asset it cannot load.
TEST(ScriptPackageRestore, NativeAssetsAreNamedInAWarning)
{
    const std::string assets = R"({
      "targets": {
        "net10.0": {
          "Data.Sqlite/8.0.0": {
            "type": "package",
            "runtime": { "lib/net8.0/Data.Sqlite.dll": {} },
            "runtimeTargets": {
              "runtimes/win-x64/native/e_sqlite3.dll": { "assetType": "native", "rid": "win-x64" },
              "runtimes/linux-x64/native/libe_sqlite3.so": { "assetType": "native", "rid": "linux-x64" }
            }
          },
          "Placeholder.Native/1.0.0": { "type": "package", "native": { "lib/net10.0/_._": {} } }
        }
      },
      "libraries": {
        "Data.Sqlite/8.0.0": { "type": "package", "path": "data.sqlite/8.0.0" },
        "Placeholder.Native/1.0.0": { "type": "package", "path": "placeholder.native/1.0.0" }
      },
      "packageFolders": { "C:/packages/": {} }
    })";
    std::vector<std::filesystem::path> assemblies;
    std::vector<std::string> warnings;
    std::string error;
    ASSERT_TRUE(ReadRestoredRuntimeAssemblies(assets, "net10.0", assemblies, warnings, error)) << error;
    EXPECT_EQ(assemblies.size(), 1u);
    ASSERT_EQ(warnings.size(), 1u) << "one warning per package; an empty placeholder is not an asset";
    EXPECT_NE(warnings[0].find("Data.Sqlite 8.0.0"), std::string::npos) << warnings[0];
    EXPECT_NE(warnings[0].find("runtimes/win-x64/native/e_sqlite3.dll"), std::string::npos) << warnings[0];
    EXPECT_NE(warnings[0].find("not supported: assemblies load from bytes"), std::string::npos) << warnings[0];
}

// The evaluated references keep the csproj's own Reference files, skip the engine's and the
// framework's, and turn a ProjectReference into a warning that names it and the fix.
TEST(ScriptPackageRestore, EvaluatedReferencesKeepOnlyTheProjectsOwnFiles)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "GE_ScriptPackageRestore_References";
    std::error_code ec;
    std::filesystem::create_directories(dir / "lib", ec);
    {
        std::ofstream(dir / "lib" / "Local.dll", std::ios::binary) << "MZ";
    }
    const std::string projectDir = (dir / "").string();
    const nlohmann::json evaluation = {
        {"Items",
         {{"Reference",
           {{{"Identity", "Local"},
             {"HintPath", "lib/Local.dll"},
             {"DefiningProjectFullPath", (dir / "GameEngine.Scripts.csproj").string()},
             {"DefiningProjectDirectory", projectDir}},
            {{"Identity", "GameEngine.Core"},
             {"HintPath", "C:/Engine/GameEngine.Core.dll"},
             {"DefiningProjectFullPath", (dir / "GameEngine.Scripts.Engine.props").string()},
             {"DefiningProjectDirectory", projectDir}},
            {{"Identity", "System.Xml"},
             {"DefiningProjectFullPath", (dir / "GameEngine.Scripts.csproj").string()},
             {"DefiningProjectDirectory", projectDir}}}},
          {"ProjectReference", {{{"Identity", "../Tools/Tools.csproj"}, {"FullPath", "C:/Tools/Tools.csproj"}}}}}}};
    std::vector<std::filesystem::path> references;
    std::vector<std::string> warnings;
    std::string error;
    ASSERT_TRUE(ReadEvaluatedReferences(evaluation.dump(), references, warnings, error)) << error;
    ASSERT_EQ(references.size(), 1u);
    EXPECT_EQ(references[0], (dir / "lib" / "Local.dll").lexically_normal());
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("C:/Tools/Tools.csproj"), std::string::npos) << warnings[0];
    EXPECT_NE(warnings[0].find("add a Reference to its output assembly"), std::string::npos) << warnings[0];

    std::filesystem::remove(dir / "lib" / "Local.dll", ec);
    references.clear();
    EXPECT_FALSE(ReadEvaluatedReferences(evaluation.dump(), references, warnings, error));
    EXPECT_NE(error.find("Local.dll"), std::string::npos) << error;
    std::filesystem::remove_all(dir, ec);
}

// A restore the compile cancels stops, and the compile error says why.
TEST(ScriptPackageRestore, ACancelledRestoreStopsWithItsMessage)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "GE_ScriptPackageRestore_Cancel";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::string text = ScriptsProjectFile::NewProjectText();
    ScriptsProjectFile::SetPackageReference(text, {"GeTestCancelled", "1.0.0"});
    {
        std::ofstream(dir / "GameEngine.Scripts.csproj", std::ios::binary) << text;
        // One empty local source, so a restore that is not stopped never reaches the network.
        std::ofstream(dir / "nuget.config", std::ios::binary)
            << "<configuration><packageSources><clear /><add key=\"local\" value=\"feed\" /></packageSources>"
               "</configuration>\n";
    }
    const std::atomic<bool> cancelled{true};
    const ScriptPackageRestoreResult result =
        RestoreScriptPackages(dir / "GameEngine.Scripts.csproj", dir / "Dependencies", "", &cancelled);
    EXPECT_FALSE(result.Success);
    EXPECT_TRUE(result.Stopped) << "a stop by a newer compile is logged as information, not as an error";
    EXPECT_NE(result.Error.find("was stopped: a newer compile replaced this one"), std::string::npos) << result.Error;
    std::filesystem::remove_all(dir, ec);
}
