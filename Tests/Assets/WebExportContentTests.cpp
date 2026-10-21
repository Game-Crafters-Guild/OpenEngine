// What a Web export ships from a project. The export script stages exactly the list the
// engine writes (StageWebExportContent), which is the desktop export's collection: the walk
// from the configured scenes plus the shippable-content filter, so a project's C# and C++
// sources and its Editor/ folders never reach a published web build (#2858); and its scenes
// ship as the desktop export ships them, without their editor-only components.

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/RuntimeHumanoidProfile.h"
#include "Engine/Build/BuildPipeline.h"
#include "Engine/GameUI/GameUIHost.h"
#include "TestTempDir.h"
#include "WebExportContent.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

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

} // namespace

TEST(WebExportContent, OnlyWhatTheScenesReachShipsWithoutSourcesEditorFoldersOrEditorOnlyComponents)
{
    const TestUtils::ScopedTempDir root{TestUtils::MakeUniqueTempDirectory("ge_web_export_content")};
    const fs::path projectAssets = root.Path() / "project" / "Assets";
    const fs::path editorAssets = root.Path() / "editor" / "Assets";
    WriteTextFile(projectAssets / "Textures" / "Rock.png", "PNGDATA");
    WriteTextFile(projectAssets / "Textures" / "Unused.png", "PNGDATA");
    WriteTextFile(projectAssets / "Scenes" / "Main.scene",
                  "[scene name=\"Main\" version=1]\n"
                  "[resource id=\"rock\" path=\"Textures/Rock.png\"]\n"
                  "\n"
                  "[entity id=\"e_1\"]\n"
                  "MeshRenderer.material = #rock\n"
                  "\n"
                  "[entity id=\"plan\"]\n"
                  "Markup.description = \"the team's private plan\"\n");
    WriteTextFile(projectAssets / "Scripts" / "Player.cs", "class Player {}");
    WriteTextFile(projectAssets / "Scripts" / "Native" / "AI.cpp", "int Think();");
    WriteTextFile(projectAssets / "Editor" / "MyTool.cs", "class MyTool {}");
    WriteTextFile(editorAssets / GameUIHost::kDefaultFontPath, "TTFDATA");
    WriteTextFile(editorAssets / kRuntimeHumanoidProfilePath, "{}");

    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(projectAssets, nullptr, root.Path() / "project" / "AssetDatabase.assetdb",
                                  root.Path() / "project" / ".Cache" / "AssetDatabase"));
    AssetSourceDesc editorSource{};
    editorSource.Alias = std::string(kAssetSourceAliasEditor);
    editorSource.Root = editorAssets;
    editorSource.RegisterFileWatcher = false;
    ASSERT_TRUE(assets.RegisterSource(editorSource));
    assets.WaitForStartupScan(std::string(kAssetSourceAliasEditor));
    assets.WaitForStartupScan();

    BuildSettings settings;
    settings.scenes = {"Scenes/Main.scene"};
    const fs::path scratch = root.Path() / "scratch";
    std::vector<std::string> warnings;
    std::string error;
    ASSERT_TRUE(StageWebExportContent(assets, settings, projectAssets, scratch, warnings, error)) << error;
    assets.Shutdown();

    std::ifstream in(scratch / kWebExportContentListName, std::ios::binary);
    const nlohmann::json list = nlohmann::json::parse(in);
    std::set<std::string> staged;
    fs::path sceneSource;
    for (const nlohmann::json& file : list.at("files"))
    {
        const fs::path source(file.at("source").get<std::string>());
        EXPECT_TRUE(fs::is_regular_file(source));
        staged.insert(file.at("path").get<std::string>());
        if (file.at("path") == "Assets/Scenes/Main.scene")
            sceneSource = source;
    }
    const std::set<std::string> expected = {
        "Assets/Scenes/Main.scene",
        "Assets/Textures/Rock.png",
        std::string("Assets/") + GameUIHost::kDefaultFontPath,
        std::string("Assets/") + kRuntimeHumanoidProfilePath,
    };
    EXPECT_EQ(staged, expected);

    // The scene ships stripped, not as authored: the mark-up's note stays in the project.
    std::ifstream sceneIn(sceneSource, std::ios::binary);
    const std::string shipped((std::istreambuf_iterator<char>(sceneIn)), std::istreambuf_iterator<char>());
    EXPECT_NE(shipped.find("MeshRenderer.material"), std::string::npos) << shipped;
    EXPECT_EQ(shipped.find("the team's private plan"), std::string::npos) << shipped;
}
