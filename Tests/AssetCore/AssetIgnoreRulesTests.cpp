// The default ignore rules every asset root and every native source walk share
// (AssetIgnoreRules::CreateDefault): the directory-name and name-prefix rules
// that keep a build tree's own sources out of the scan, the digest and the
// compile.

#include "AssetCore/AssetIgnoreRules.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

using GameEngine::AssetIgnoreRules;
using GameEngine::TestUtils::MakeUniqueTempDirectory;
using GameEngine::TestUtils::ScopedTempDir;
namespace fs = std::filesystem;

TEST(AssetIgnoreRules, DefaultsSkipBuildTreesByDirectoryNameAndByNamePrefix)
{
    const AssetIgnoreRules rules = AssetIgnoreRules::CreateDefault();
    const fs::path root = fs::temp_directory_path();

    // Recursion is cut at the directory: a CMake build tree's own sources
    // (CMakeCXXCompilerId.cpp defines main) and a build tree named after its
    // configuration ("cmake-build-debug") are never entered.
    EXPECT_TRUE(rules.ShouldIgnoreDirectory(root / "CMakeFiles", root));
    EXPECT_TRUE(rules.ShouldIgnoreDirectory(root / "Tools" / "cmake-build-debug", root));
    EXPECT_TRUE(rules.ShouldIgnoreDirectory(root / "Tools" / "CMAKE-BUILD-Release", root));
    // The name prefix ends in the dash: a directory that merely starts with the
    // word is ordinary.
    EXPECT_FALSE(rules.ShouldIgnoreDirectory(root / "Tools" / "cmake-builder", root));
    EXPECT_FALSE(rules.ShouldIgnoreDirectory(root / "Source", root));

    // The same segments seen from a file's root-relative path.
    EXPECT_TRUE(rules.ShouldIgnoreCanonicalRelativePath(
        "Tools/CMakeFiles/4.0.0/CompilerIdCXX/CMakeCXXCompilerId.cpp"));
    EXPECT_TRUE(rules.ShouldIgnoreCanonicalRelativePath("Tools/cmake-build-debug/Widen.gen.cpp"));
    EXPECT_FALSE(rules.ShouldIgnoreCanonicalRelativePath("Tools/cmake-builder/Tool.cpp"));
    EXPECT_FALSE(rules.ShouldIgnoreCanonicalRelativePath("Source/Widen.cpp"));
}

// A terrain's page caches (<ProjectRoot>/.Cache/TerrainPages/*.gepage) share the extension of the
// authored height layer (AssetType::TerrainHeightLayer): the scan never enters .Cache, so only the
// layer beside a scene is an asset.
TEST(AssetIgnoreRules, DefaultsSkipTheDerivedCacheFolderSoAPageCacheIsNeverAnAsset)
{
    const AssetIgnoreRules rules = AssetIgnoreRules::CreateDefault();
    const fs::path root = fs::temp_directory_path();

    EXPECT_TRUE(rules.ShouldIgnoreDirectory(root / ".Cache", root));
    EXPECT_TRUE(rules.ShouldIgnoreCanonicalRelativePath(".Cache/TerrainPages/0123456789abcdef.gepage"));
    EXPECT_FALSE(rules.ShouldIgnoreCanonicalRelativePath("Scenes/World_Zones/layer.gepage"));
}

// A game names content folders "Generated", "Library" or "build". The tool output names
// apply directly under the root, and deeper only to a folder that holds a tool marker.
TEST(AssetIgnoreRules, ToolOutputNamesApplyAtTheRootOrWithAMarker)
{
    const ScopedTempDir temp(MakeUniqueTempDirectory("ge_ignore_rules_tool_output"));
    const fs::path root = temp.Path() / "Assets";
    fs::create_directories(root / "World" / "Generated");
    fs::create_directories(root / "Tools" / "build");
    const AssetIgnoreRules rules = AssetIgnoreRules::CreateDefault();

    EXPECT_FALSE(rules.ShouldIgnoreDirectory(root / "World" / "Generated", root));
    EXPECT_FALSE(rules.ShouldIgnoreCanonicalRelativePath("World/Generated/Height.r32"));
    EXPECT_FALSE(rules.FindIgnoredFolder("World/Generated/Height.r32", root).has_value());
    EXPECT_FALSE(rules.ShouldIgnoreDirectory(root / "Tools" / "build", root));

    EXPECT_TRUE(rules.ShouldIgnoreDirectory(root / "build", root));
    EXPECT_TRUE(rules.ShouldIgnoreCanonicalRelativePath("build/x.png"));
    const auto rootBuild = rules.FindIgnoredFolder("build/x.png", root);
    ASSERT_TRUE(rootBuild.has_value());
    EXPECT_EQ(rootBuild->Folder, root / "build");

    std::ofstream(root / "Tools" / "build" / "CMakeCache.txt") << "CMAKE_HOME_DIRECTORY:INTERNAL=x\n";
    EXPECT_TRUE(rules.ShouldIgnoreDirectory(root / "Tools" / "build", root));
    const auto markedBuild = rules.FindIgnoredFolder("Tools/build/x.png", root);
    ASSERT_TRUE(markedBuild.has_value());
    EXPECT_EQ(markedBuild->Folder, root / "Tools/build");
    EXPECT_NE(markedBuild->Reason.find("CMakeCache.txt"), std::string::npos) << markedBuild->Reason;
}

TEST(AssetIgnoreRules, SignatureCoversDirectoryNamePrefixes)
{
    // The warm-start snapshot keys on the signature: a rule set that ignores
    // more directories must not read as the same rule set.
    AssetIgnoreRules rules = AssetIgnoreRules::CreateDefault();
    const uint64_t withDefaults = rules.Signature();
    rules.ignoredDirNamePrefixesLower.clear();
    EXPECT_NE(rules.Signature(), withDefaults);
}

TEST(AssetIgnoreRules, SignatureCoversToolOutputMarkers)
{
    // A marker file name decides which deep tool output folders are ignored.
    AssetIgnoreRules rules = AssetIgnoreRules::CreateDefault();
    const uint64_t withDefaults = rules.Signature();
    rules.toolOutputMarkerFileNames.pop_back();
    EXPECT_NE(rules.Signature(), withDefaults);
}

TEST(AssetIgnoreRules, PrefixRulesMatchAPathUnderADirectoryLinkAsTheWalkSpellsIt)
{
    // A path under the root keeps the spelling of the walk. A directory link
    // under the root does not move its entries out of the root.
    const ScopedTempDir temp(MakeUniqueTempDirectory("ge_ignore_rules_link"));
    const fs::path root = temp.Path() / "Assets";
    fs::create_directories(root);
    fs::create_directories(temp.Path() / "Outside");
    std::error_code ec;
    fs::create_directory_symlink(temp.Path() / "Outside", root / "Generated", ec);
    if (ec)
        GTEST_SKIP() << "directory symlink not supported: " << ec.message();

    AssetIgnoreRules rules = AssetIgnoreRules::CreateDefault();
    rules.ignoredPrefixesLower.push_back("generated/");
    EXPECT_TRUE(rules.ShouldIgnoreFile(root / "Generated" / "Mesh.fbx", root));
    EXPECT_TRUE(rules.ShouldIgnoreDirectory(root / "Generated" / "Lods", root));
    EXPECT_FALSE(rules.ShouldIgnoreFile(root / "Source" / "Mesh.fbx", root));
}
