// Ship rules: a folder's .assetfolder file sets Always, Never or Inferred for what is under it,
// and the export's collection (every target's) applies them. A game that opens files by path
// marks their folder Always; a folder of content that must not ship is Never.

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/RuntimeHumanoidProfile.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/Build/ShipRules.h"
#include "Engine/GameUI/GameUIHost.h"
#include "TestTempDir.h"
#include "Types/PathUtils.h"
#include "UI/ControlAssetPaths.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{

void WriteTextFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string Prefab(const std::string& name, const std::string& texturePath)
{
    return "[prefab name=\"" + name + "\" version=1]\n"
           "[entity id=\"root\"]\n"
           "Name.value = \"" + name + "\"\n"
           "MeshRenderer.material = [path=\"" + texturePath + "\"]\n";
}

std::string SceneSpawning(const std::string& name, const std::string& prefabPath)
{
    return "[scene name=\"" + name + "\" version=1]\n"
           "[resource id=\"thing\" path=\"" + prefabPath + "\"]\n"
           "\n"
           "[prefab id=\"thing_0\" source=\"thing\"]\n"
           "Transform.position = (0, 0, 0)\n";
}

class ShipRulesCollection : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        WriteTextFile(m_EditorAssets / GameUIHost::kDefaultFontPath, "TTFDATA");
        WriteTextFile(m_EditorAssets / kRuntimeHumanoidProfilePath, "{}");
        for (std::string_view path : UI::kControlAssetPaths)
            WriteTextFile(m_EditorAssets / path, ".x { width: 1px; }\n");
        WriteTextFile(m_ProjectAssets / "Scenes" / "Main.scene", "[scene name=\"Main\" version=1]\n");
    }

    void TearDown() override { m_AssetManager.Shutdown(); }

    void Mount()
    {
        ASSERT_TRUE(m_AssetManager.Initialize(m_ProjectAssets, nullptr,
                                              m_Root.Path() / "project" / "AssetDatabase.assetdb",
                                              m_Root.Path() / "project" / ".Cache" / "AssetDatabase"));
        AssetSourceDesc editorSource{};
        editorSource.Alias = std::string(kAssetSourceAliasEditor);
        editorSource.Root = m_EditorAssets;
        editorSource.RegisterFileWatcher = false;
        ASSERT_TRUE(m_AssetManager.RegisterSource(editorSource));
        m_AssetManager.WaitForStartupScan(std::string(kAssetSourceAliasEditor));
        m_AssetManager.WaitForStartupScan();
    }

    // A fresh clone has Assets/ and AssetDatabase.assetdb, and no .Cache/.
    void RemountLikeAFreshClone()
    {
        m_AssetManager.Shutdown();
        fs::remove_all(m_Root.Path() / "project" / ".Cache");
        Mount();
    }

    // The staged paths of a collection, or the error when it fails.
    bool Collect(const std::vector<std::string>& scenes, std::set<std::string>& outShipped, std::string& outError)
    {
        AssetCollector collector(m_AssetManager);
        AssetManifest manifest;
        m_Warnings.clear();
        if (!collector.CollectGameContent(scenes, {}, m_ProjectAssets, manifest, m_Warnings, outError))
            return false;
        for (const AssetManifestEntry& entry : manifest.entries)
            outShipped.insert(PathToUtf8(entry.outputPath));
        return true;
    }

    std::set<std::string> WithEngineDefaults(std::set<std::string> paths) const
    {
        paths.insert(std::string("Assets/") + GameUIHost::kDefaultFontPath);
        paths.insert(std::string("Assets/") + kRuntimeHumanoidProfilePath);
        for (std::string_view path : UI::kControlAssetPaths)
            paths.insert("Assets/" + std::string(path));
        return paths;
    }

    const TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("ge_ship_rules")};
    const fs::path m_ProjectAssets = m_Root.Path() / "project" / "Assets";
    const fs::path m_EditorAssets = m_Root.Path() / "editor" / "Assets";
    AssetManager m_AssetManager;
    std::vector<std::string> m_Warnings; // The last Collect's warnings.
};

void SetOwnRule(AssetRegistry& registry, const fs::path& asset, std::string_view rule)
{
    ASSERT_TRUE(registry.SetMetaValue(asset, std::string(ShipRules::kAssetMetaKey), std::string(rule))) << asset;
}

} // namespace

TEST(ShipRules, TheNearestFolderFileWins)
{
    const TestUtils::ScopedTempDir root{TestUtils::MakeUniqueTempDirectory("ge_ship_rules_resolve")};
    WriteTextFile(root.Path() / "Data" / ".assetfolder", R"({"ship": "always"})");
    WriteTextFile(root.Path() / "Data" / "drafts" / ".assetfolder", R"({"ship": "inferred"})");
    WriteTextFile(root.Path() / "Textures" / "Old" / ".assetfolder", R"({"ship": "never"})");
    // An ignored folder holds no registered asset, so its folder file sets nothing.
    WriteTextFile(root.Path() / "Generated" / ".assetfolder", R"({"ship": "never"})");

    std::vector<std::string> errors;
    const ShipRules rules = ShipRules::Load(root.Path(), errors);
    EXPECT_TRUE(errors.empty()) << errors.front();

    const ShipRules::Resolved maps = rules.Resolve("Data/maps/a.json");
    EXPECT_EQ(maps.Rule, ShipRule::Always);
    EXPECT_EQ(maps.Folder, "Data/");
    EXPECT_EQ(rules.Resolve("data/Maps/A.json").Rule, ShipRule::Always);
    const ShipRules::Resolved drafts = rules.Resolve("Data/drafts/b.json");
    EXPECT_EQ(drafts.Rule, ShipRule::Inferred);
    EXPECT_EQ(drafts.Folder, "Data/drafts/");
    const ShipRules::Resolved old = rules.Resolve("Textures/Old/Wall.png");
    EXPECT_EQ(old.Rule, ShipRule::Never);
    EXPECT_EQ(old.Folder, "Textures/Old/");
    EXPECT_EQ(rules.Resolve("Textures/Rock.png").Rule, ShipRule::Inferred);
    EXPECT_EQ(rules.Resolve("Generated/x.json").Rule, ShipRule::Inferred);
}

TEST(ShipRules, AFolderFileThatIsNotValidFailsWithASentenceThatNamesIt)
{
    const TestUtils::ScopedTempDir root{TestUtils::MakeUniqueTempDirectory("ge_ship_rules_invalid")};
    WriteTextFile(root.Path() / "Data" / ".assetfolder", R"({"ship": "yes"})");
    WriteTextFile(root.Path() / "Audio" / ".assetfolder", "always");
    WriteTextFile(root.Path() / "UI" / ".assetfolder", R"({"ship": "always", "group": "menus"})");

    std::vector<std::string> errors;
    const ShipRules rules = ShipRules::Load(root.Path(), errors);
    ASSERT_EQ(errors.size(), 3u);
    const std::set<std::string> sentences(errors.begin(), errors.end());
    EXPECT_EQ(sentences.count("Data/.assetfolder is not valid: \"ship\" must be \"always\", \"never\" or "
                              "\"inferred\" (found \"yes\"). Fix the file or delete it."),
              1u);
    EXPECT_EQ(sentences.count("Audio/.assetfolder is not valid: it must be a JSON object such as "
                              "{\"ship\": \"always\"}. Fix the file or delete it."),
              1u);
    EXPECT_EQ(sentences.count("UI/.assetfolder is not valid: \"group\" is not a folder setting; only \"ship\" "
                              "is. Fix the file or delete it."),
              1u);
    EXPECT_TRUE(rules.IsEmpty());
}

#if !defined(_WIN32)
// Permission bits cannot make a file unreadable on Windows, so POSIX only.
TEST(ShipRules, AFolderFileThatCannotBeReadFailsWithASentenceThatNamesIt)
{
    if (geteuid() == 0)
        GTEST_SKIP() << "root reads a file with no permission bits";
    const TestUtils::ScopedTempDir root{TestUtils::MakeUniqueTempDirectory("ge_ship_rules_unreadable")};
    const fs::path file = root.Path() / "Data" / ".assetfolder";
    WriteTextFile(file, R"({"ship": "always"})");
    fs::permissions(file, fs::perms::none);

    std::vector<std::string> errors;
    const ShipRules rules = ShipRules::Load(root.Path(), errors);
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write);
    EXPECT_EQ(errors, std::vector<std::string>{"Data/.assetfolder cannot be read. Check that no other program holds it."});
    EXPECT_TRUE(rules.IsEmpty());
}

#if defined(_LIBCPP_VERSION)
// libc++ opens each folder of the walk by its full path, so a folder path longer than PATH_MAX stops
// the walk with an error.
TEST(ShipRules, AFolderWalkThatStopsOnAnErrorFailsWithASentenceThatNamesWhere)
{
    const TestUtils::ScopedTempDir root{TestUtils::MakeUniqueTempDirectory("ge_ship_rules_walk_error")};
    const std::string name(200, 'd');
    const std::string tooLong = std::make_error_code(std::errc::filename_too_long).message();
    std::vector<std::string> expected;
    fs::path folder = root.Path();
    // Each folder is made relative to its parent: a path past PATH_MAX cannot be given whole.
    int parent = open(root.Path().c_str(), O_RDONLY | O_DIRECTORY);
    while (parent >= 0 && folder.native().size() <= PATH_MAX)
    {
        EXPECT_EQ(mkdirat(parent, name.c_str(), 0755), 0);
        const int child = openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY);
        close(parent);
        parent = child;
        folder /= name;
        expected.push_back("The folder files under " + PathToUtf8(root.Path()) +
                           " cannot all be read: the read stopped after " + PathToUtf8(folder) + " (" + tooLong +
                           "). Check the folder and export again.");
    }
    if (parent >= 0)
        close(parent);

    std::vector<std::string> errors;
    ShipRules::Load(root.Path(), errors);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(std::find(expected.begin(), expected.end(), errors[0]), expected.end()) << errors[0];
}
#endif
#endif

TEST_F(ShipRulesCollection, AnAlwaysFolderShipsWhatNothingUsesWithWhatItNames)
{
    WriteTextFile(m_ProjectAssets / "Data" / ".assetfolder", R"({"ship": "always"})");
    WriteTextFile(m_ProjectAssets / "Data" / "units.json", R"({"model": "Textures/Unit.png"})");
    WriteTextFile(m_ProjectAssets / "Data" / "maps" / "lakeside.json", "{}");
    WriteTextFile(m_ProjectAssets / "Data" / "drafts" / ".assetfolder", R"({"ship": "inferred"})");
    WriteTextFile(m_ProjectAssets / "Data" / "drafts" / "next.json", "{}");
    WriteTextFile(m_ProjectAssets / "Data" / "Loader.cpp", "int Load();");
    WriteTextFile(m_ProjectAssets / "Data" / "Menu.backup.scene", "[scene name=\"Menu\" version=1]\n");
    WriteTextFile(m_ProjectAssets / "Textures" / "Unit.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Textures" / "Unused.png", "PNGDATA");
    Mount();

    std::set<std::string> shipped;
    std::string error;
    ASSERT_TRUE(Collect({"Scenes/Main.scene"}, shipped, error)) << error;
    EXPECT_EQ(shipped, WithEngineDefaults({"Assets/Scenes/Main.scene", "Assets/Data/units.json",
                                           "Assets/Data/maps/lakeside.json", "Assets/Textures/Unit.png"}));
}

TEST_F(ShipRulesCollection, AnAlwaysClosureShipsByPathReferencesTheEdgeCacheDropped)
{
    // The nested reference is by path: the prefab names its texture, and nothing names it by GUID.
    WriteTextFile(m_ProjectAssets / "Data" / ".assetfolder", R"({"ship": "always"})");
    WriteTextFile(m_ProjectAssets / "Data" / "units.json", R"({"prefab": "Prefabs/Unit.prefab"})");
    WriteTextFile(m_ProjectAssets / "Prefabs" / "Unit.prefab", Prefab("Unit", "Textures/Unit.png"));
    WriteTextFile(m_ProjectAssets / "Textures" / "Unit.png", "PNGDATA");
    Mount();
    // A second session: the edge cache opens only when the asset database existed at mount.
    m_AssetManager.Shutdown();
    Mount();
    // The editor refreshes a prefab's edges when it saves it; the edge cache then drops the by-path edge (#2844).
    AssetRegistry& registry = m_AssetManager.GetRegistry();
    ASSERT_EQ(registry.RefreshDependencies(registry.GetAssetGUID(m_ProjectAssets / "Prefabs" / "Unit.prefab")).size(), 1u);

    const std::set<std::string> expected = WithEngineDefaults(
        {"Assets/Scenes/Main.scene", "Assets/Data/units.json", "Assets/Prefabs/Unit.prefab", "Assets/Textures/Unit.png"});
    for (int exportIndex = 0; exportIndex < 2; ++exportIndex)
    {
        std::set<std::string> shipped;
        std::string error;
        ASSERT_TRUE(Collect({"Scenes/Main.scene"}, shipped, error)) << error;
        EXPECT_EQ(shipped, expected) << "export " << exportIndex;
    }
}

TEST_F(ShipRulesCollection, ANeverAssetAShippedAssetUsesFailsTheExportAndNamesTheChain)
{
    WriteTextFile(m_ProjectAssets / "Scenes" / "Castle.scene", SceneSpawning("Castle", "Prefabs/Wall.prefab"));
    WriteTextFile(m_ProjectAssets / "Prefabs" / "Wall.prefab", Prefab("Wall", "Textures/Old/Wall.png"));
    WriteTextFile(m_ProjectAssets / "Textures" / "Old" / ".assetfolder", R"({"ship": "never"})");
    WriteTextFile(m_ProjectAssets / "Textures" / "Old" / "Wall.png", "PNGDATA");
    Mount();

    std::set<std::string> shipped;
    std::string error;
    EXPECT_FALSE(Collect({"Scenes/Castle.scene"}, shipped, error));
    EXPECT_EQ(error, "Textures/Old/Wall.png is set to Never ship (from its folder Textures/Old/), but a shipped "
                     "asset uses it: Scenes/Castle.scene → Prefabs/Wall.prefab → Textures/Old/Wall.png. Remove the "
                     "reference, or change the rule.");
}

TEST_F(ShipRulesCollection, AnAssetsOwnNeverRuleAShippedAssetUsesFailsTheExportAndNamesTheChain)
{
    WriteTextFile(m_ProjectAssets / "Scenes" / "Castle.scene", SceneSpawning("Castle", "Prefabs/Wall.prefab"));
    WriteTextFile(m_ProjectAssets / "Prefabs" / "Wall.prefab", Prefab("Wall", "Textures/Wall.png"));
    WriteTextFile(m_ProjectAssets / "Textures" / "Wall.png", "PNGDATA");
    Mount();
    SetOwnRule(m_AssetManager.GetRegistry(), m_ProjectAssets / "Textures" / "Wall.png", "never");

    std::set<std::string> shipped;
    std::string error;
    EXPECT_FALSE(Collect({"Scenes/Castle.scene"}, shipped, error));
    EXPECT_EQ(error, "Textures/Wall.png is set to Never ship (from its own rule), but a shipped asset uses it: "
                     "Scenes/Castle.scene → Prefabs/Wall.prefab → Textures/Wall.png. Remove the reference, or "
                     "change the rule.");
}

// The rule keys and the chain sentence are UTF-8 on every platform, so a folder name that is not
// ASCII matches its rule.
TEST_F(ShipRulesCollection, ANeverFolderWithANameThatIsNotAsciiFailsTheExportAndNamesTheChain)
{
#if defined(_WIN32)
    GTEST_SKIP() << "the asset scan converts paths through the code page (#4246)";
#else
    const fs::path folder(u8"\u4E2D");
    const std::string folderName = PathToUtf8(folder);
    WriteTextFile(m_ProjectAssets / "Scenes" / "Castle.scene", SceneSpawning("Castle", "Prefabs/Wall.prefab"));
    WriteTextFile(m_ProjectAssets / "Prefabs" / "Wall.prefab", Prefab("Wall", folderName + "/Wall.png"));
    WriteTextFile(m_ProjectAssets / folder / ".assetfolder", R"({"ship": "never"})");
    WriteTextFile(m_ProjectAssets / folder / "Wall.png", "PNGDATA");
    Mount();

    std::set<std::string> shipped;
    std::string error;
    EXPECT_FALSE(Collect({"Scenes/Castle.scene"}, shipped, error));
    EXPECT_EQ(error, folderName + "/Wall.png is set to Never ship (from its folder " + folderName +
                         "/), but a shipped asset uses it: Scenes/Castle.scene → Prefabs/Wall.prefab → " + folderName +
                         "/Wall.png. Remove the reference, or change the rule.");
#endif
}

TEST_F(ShipRulesCollection, ANeverSceneInTheSceneListFailsTheExport)
{
    WriteTextFile(m_ProjectAssets / "Scenes" / "Tests" / ".assetfolder", R"({"ship": "never"})");
    WriteTextFile(m_ProjectAssets / "Scenes" / "Tests" / "Probe.scene", "[scene name=\"Probe\" version=1]\n");
    Mount();

    std::set<std::string> shipped;
    std::string error;
    EXPECT_FALSE(Collect({"Scenes/Main.scene", "Scenes/Tests/Probe.scene"}, shipped, error));
    EXPECT_EQ(error, "Scenes/Tests/Probe.scene is set to Never ship (from its folder Scenes/Tests/), but it is in "
                     "the build's scene list. Remove it from the list, or change the rule.");
}

TEST_F(ShipRulesCollection, AnAlwaysFolderStopsTheFallbackAndTheFallbackSkipsNever)
{
    // The scene list names no scene the registry holds, so only the Always folder adds content.
    WriteTextFile(m_ProjectAssets / "Data" / ".assetfolder", R"({"ship": "always"})");
    WriteTextFile(m_ProjectAssets / "Data" / "units.json", "{}");
    WriteTextFile(m_ProjectAssets / "Old" / ".assetfolder", R"({"ship": "never"})");
    WriteTextFile(m_ProjectAssets / "Old" / "units.json", "{}");
    WriteTextFile(m_ProjectAssets / "Textures" / "Unused.png", "PNGDATA");
    Mount();

    std::set<std::string> shipped;
    std::string error;
    ASSERT_TRUE(Collect({"Scenes/Missing.scene"}, shipped, error)) << error;
    EXPECT_EQ(shipped, WithEngineDefaults({"Assets/Data/units.json"}));

    // With no Always folder, the fallback ships the project's content, except what is Never.
    WriteTextFile(m_ProjectAssets / "Data" / ".assetfolder", R"({"ship": "inferred"})");
    shipped.clear();
    ASSERT_TRUE(Collect({"Scenes/Missing.scene"}, shipped, error)) << error;
    EXPECT_EQ(shipped, WithEngineDefaults({"Assets/Scenes/Main.scene", "Assets/Data/units.json",
                                           "Assets/Textures/Unused.png"}));
}

TEST_F(ShipRulesCollection, AnAssetsOwnRuleBeatsItsFolderAndAFolderBeatsItsParent)
{
    WriteTextFile(m_ProjectAssets / "Data" / ".assetfolder", R"({"ship": "always"})");
    WriteTextFile(m_ProjectAssets / "Data" / "units.json", "{}");
    WriteTextFile(m_ProjectAssets / "Data" / "old.json", "{}");
    WriteTextFile(m_ProjectAssets / "Data" / "drafts" / ".assetfolder", R"({"ship": "inferred"})");
    WriteTextFile(m_ProjectAssets / "Data" / "drafts" / "next.json", "{}");
    WriteTextFile(m_ProjectAssets / "Data" / "drafts" / "later.json", "{}");
    WriteTextFile(m_ProjectAssets / "Loose" / "credits.json", "{}");
    WriteTextFile(m_ProjectAssets / "Loose" / "draft.json", "{}");
    Mount();
    AssetRegistry& registry = m_AssetManager.GetRegistry();
    SetOwnRule(registry, m_ProjectAssets / "Data" / "old.json", "never");
    SetOwnRule(registry, m_ProjectAssets / "Data" / "drafts" / "next.json", "always");
    SetOwnRule(registry, m_ProjectAssets / "Loose" / "credits.json", "always");
    // An own Never outside an Always folder keeps nothing out, so it has no warning.
    SetOwnRule(registry, m_ProjectAssets / "Loose" / "draft.json", "never");

    std::set<std::string> shipped;
    std::string error;
    ASSERT_TRUE(Collect({"Scenes/Main.scene"}, shipped, error)) << error;
    EXPECT_EQ(shipped, WithEngineDefaults({"Assets/Scenes/Main.scene", "Assets/Data/units.json",
                                           "Assets/Data/drafts/next.json", "Assets/Loose/credits.json"}));
    EXPECT_EQ(m_Warnings, std::vector<std::string>{"Data/old.json is set to Never ship (from its own rule), so it does "
                                                   "not ship, although its folder Data/ is Always."});
}

// An asset's own rule is read from its UTF-8 path, so it applies and is named under a folder name that is not ASCII.
TEST_F(ShipRulesCollection, AnAssetsOwnRuleUnderAFolderWithANameThatIsNotAsciiAppliesAndIsNamed)
{
#if defined(_WIN32)
    GTEST_SKIP() << "the asset scan converts paths through the code page (#4246)";
#else
    const fs::path folder(u8"\u4E2D");
    const std::string folderName = PathToUtf8(folder);
    WriteTextFile(m_ProjectAssets / folder / ".assetfolder", R"({"ship": "always"})");
    WriteTextFile(m_ProjectAssets / folder / "units.json", "{}");
    WriteTextFile(m_ProjectAssets / folder / "old.json", "{}");
    WriteTextFile(m_ProjectAssets / folder / "Unit.cpp", "int unit;\n");
    Mount();
    AssetRegistry& registry = m_AssetManager.GetRegistry();
    SetOwnRule(registry, m_ProjectAssets / folder / "old.json", "never");

    std::set<std::string> shipped;
    std::string error;
    ASSERT_TRUE(Collect({"Scenes/Main.scene"}, shipped, error)) << error;
    EXPECT_EQ(shipped, WithEngineDefaults({"Assets/Scenes/Main.scene", "Assets/" + folderName + "/units.json"}));
    EXPECT_EQ(m_Warnings, std::vector<std::string>{folderName + "/old.json is set to Never ship (from its own rule), so "
                                                                "it does not ship, although its folder " +
                                                   folderName + "/ is Always."});

    SetOwnRule(registry, m_ProjectAssets / folder / "Unit.cpp", "always");
    EXPECT_FALSE(Collect({"Scenes/Main.scene"}, shipped, error));
    EXPECT_EQ(error, folderName + "/Unit.cpp is set to Always ship, but source files, Editor/ folders, build output "
                                  "and scene backups never ship. Clear the rule.");
#endif
}

TEST_F(ShipRulesCollection, AnAssetRuleSurvivesAFreshCloneAndRemoveMetaValueClearsIt)
{
    WriteTextFile(m_ProjectAssets / "Loose" / "credits.json", "{}");
    const fs::path credits = m_ProjectAssets / "Loose" / "credits.json";
    Mount();
    SetOwnRule(m_AssetManager.GetRegistry(), credits, "always");
    RemountLikeAFreshClone();

    std::set<std::string> shipped;
    std::string error;
    ASSERT_TRUE(Collect({"Scenes/Main.scene"}, shipped, error)) << error;
    EXPECT_EQ(shipped, WithEngineDefaults({"Assets/Scenes/Main.scene", "Assets/Loose/credits.json"}));

    ASSERT_TRUE(m_AssetManager.GetRegistry().RemoveMetaValue(credits, std::string(ShipRules::kAssetMetaKey)));
    RemountLikeAFreshClone();
    std::string stored;
    EXPECT_FALSE(m_AssetManager.GetRegistry().TryGetMetaValue(credits, std::string(ShipRules::kAssetMetaKey), stored))
        << stored;
    shipped.clear();
    ASSERT_TRUE(Collect({"Scenes/Main.scene"}, shipped, error)) << error;
    EXPECT_EQ(shipped, WithEngineDefaults({"Assets/Scenes/Main.scene"}));
}

TEST_F(ShipRulesCollection, AStoredRuleThatIsNotARuleFailsTheExport)
{
    WriteTextFile(m_ProjectAssets / "Data" / "units.json", "{}");
    Mount();
    SetOwnRule(m_AssetManager.GetRegistry(), m_ProjectAssets / "Data" / "units.json", "sometimes");

    std::set<std::string> shipped;
    std::string error;
    EXPECT_FALSE(Collect({"Scenes/Main.scene"}, shipped, error));
    EXPECT_EQ(error, "The ship rule stored for Data/units.json is \"sometimes\". Set it again in the inspector.");
}

TEST_F(ShipRulesCollection, AnAssetsOwnAlwaysOnContentThatNeverShipsFailsTheExport)
{
    WriteTextFile(m_ProjectAssets / "Scripts" / "Unit.cpp", "int unit;\n");
    Mount();
    SetOwnRule(m_AssetManager.GetRegistry(), m_ProjectAssets / "Scripts" / "Unit.cpp", "always");

    std::set<std::string> shipped;
    std::string error;
    EXPECT_FALSE(Collect({"Scenes/Main.scene"}, shipped, error));
    EXPECT_EQ(error, "Scripts/Unit.cpp is set to Always ship, but source files, Editor/ folders, build output and "
                     "scene backups never ship. Clear the rule.");
}

TEST_F(ShipRulesCollection, FolderRulesUnderAnotherRootThanTheRegistrysFailTheExport)
{
    const fs::path otherAssets = m_Root.Path() / "other" / "Assets";
    WriteTextFile(otherAssets / "Data" / ".assetfolder", R"({"ship": "always"})");
    Mount();

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    std::vector<std::string> warnings;
    std::string error;
    EXPECT_FALSE(collector.CollectGameContent({"Scenes/Main.scene"}, {}, otherAssets, manifest, warnings, error));
    EXPECT_EQ(error, "The export reads folder rules under " + PathToUtf8(otherAssets) +
                         ", but the asset registry's root is " +
                         PathToUtf8(m_AssetManager.GetRegistry().GetAssetRoot()) +
                         ", so no rule can apply. Open the project again and export again.");
}
