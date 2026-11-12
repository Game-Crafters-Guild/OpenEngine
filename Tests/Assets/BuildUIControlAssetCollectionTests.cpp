// The UI module's control assets a build ships. A control requests its stylesheet by path in its
// constructor, so no scene or pipeline references the file, and the dependency walk never finds
// it. The collector ships every file the UI module stages, from the editor mount, under Assets/.

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/RuntimeHumanoidProfile.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/GameUI/GameUIHost.h"
#include "UI/ControlAssetPaths.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{

constexpr const char* kControlIconPath = "Icons/control-icon.svg";

void WriteTextFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// An empty project and an editor mount that holds what every export ships: the humanoid
// profile, the default game UI font and the UI module's control assets.
class BuildUIControlAssetCollection : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fs::create_directories(m_ProjectAssets);
        WriteTextFile(m_EditorAssets / kRuntimeHumanoidProfilePath, "{}");
        WriteTextFile(m_EditorAssets / GameUIHost::kDefaultFontPath, "TTFDATA");
        for (std::string_view path : UI::kControlAssetPaths)
            WriteTextFile(m_EditorAssets / path, ".x { width: 1px; }\n");
        WriteTextFile(m_EditorAssets / kControlIconPath, "<svg/>");
        WriteTextFile(m_EditorAssets / "UI/controls/Button.css",
                      ".x { background-image: url(\"editor:Icons/control-icon.svg\"); }\n");

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

    void TearDown() override { m_AssetManager.Shutdown(); }

    const TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("ge_build_ui_control_collect")};
    const fs::path m_ProjectAssets = m_Root.Path() / "project" / "Assets";
    const fs::path m_EditorAssets = m_Root.Path() / "editor" / "Assets";
    AssetManager m_AssetManager;
};

} // namespace

TEST_F(BuildUIControlAssetCollection, GameContentShipsEveryControlAssetFromTheEditorMount)
{
    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    std::vector<std::string> warnings;
    std::string error;
    ASSERT_TRUE(collector.CollectGameContent({}, {}, m_ProjectAssets, manifest, warnings, error)) << error;

    ASSERT_FALSE(UI::kControlAssetPaths.empty());
    for (std::string_view path : UI::kControlAssetPaths)
    {
        const GUID guid = m_AssetManager.GetRegistry().GetAssetGUID(m_EditorAssets / path);
        ASSERT_FALSE(guid.IsNull()) << path;
        const AssetManifestEntry* entry = manifest.FindByGuid(guid);
        ASSERT_NE(entry, nullptr) << "the export does not ship " << path;
        EXPECT_EQ(entry->outputPath.generic_string(), "Assets/" + std::string(path));
        EXPECT_EQ(entry->sourceAlias, std::string(kAssetSourceAliasEditor));
    }

    const GUID icon = m_AssetManager.GetRegistry().GetAssetGUID(m_EditorAssets / kControlIconPath);
    ASSERT_FALSE(icon.IsNull());
    EXPECT_NE(manifest.FindByGuid(icon), nullptr) << "the export does not ship an icon a control sheet references";
}
