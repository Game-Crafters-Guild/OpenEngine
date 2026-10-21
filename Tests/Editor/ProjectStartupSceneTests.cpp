// The scene a project opens on: the first build scene, else project.startupScene, else the
// project's last-opened scene, else none; and the startup scene a created project is given.

#include <gtest/gtest.h>

#include "Editor/Settings/SettingsStore.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Projects/ProjectAcquisition.h"
#include "ScopedEditorUserDataRoot.h"
#include "Projects/ProjectCatalog.h"
#include "Projects/ProjectStartupScene.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using GameEngine::Editor::FindNewProjectStartupScene;
using GameEngine::Editor::kProjectStartupSceneKey;
using GameEngine::Editor::ProjectStartupSceneCandidates;
using GameEngine::Editor::ResolveProjectStartupScene;

namespace
{

fs::path MakeScratchRoot(const char* name)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / (std::string(name) + "_" + std::to_string(stamp));
    fs::create_directories(root);
    return root;
}

void WriteFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << text;
}

} // namespace

TEST(ProjectStartupScene, TheFirstBuildSceneWinsThenTheStartupSceneThenTheLastOpened)
{
    const fs::path project = MakeScratchRoot("ge_startup_order");
    WriteFile(project / "Assets" / "Scenes" / "Build.scene", "{}");
    WriteFile(project / "Assets" / "Scenes" / "Startup.scene", "{}");
    WriteFile(project / "Assets" / "Scenes" / "Recent.scene", "{}");

    ProjectStartupSceneCandidates candidates;
    candidates.GlobalBuildScenes = {"Scenes/Build.scene", "Scenes/Startup.scene"};
    candidates.StartupScene = "Scenes/Startup.scene";
    candidates.RecentScenes = {project / "Assets" / "Scenes" / "Recent.scene"};

    auto scene = ResolveProjectStartupScene(project, candidates);
    ASSERT_TRUE(scene.has_value());
    EXPECT_EQ(scene->filename(), "Build.scene");

    // A build scene that is not on disk falls through to the next rule.
    candidates.GlobalBuildScenes = {"Scenes/Missing.scene"};
    scene = ResolveProjectStartupScene(project, candidates);
    ASSERT_TRUE(scene.has_value());
    EXPECT_EQ(scene->filename(), "Startup.scene");

    candidates.GlobalBuildScenes.clear();
    candidates.StartupScene.clear();
    scene = ResolveProjectStartupScene(project, candidates);
    ASSERT_TRUE(scene.has_value());
    EXPECT_EQ(scene->filename(), "Recent.scene");

    std::error_code ec;
    fs::remove_all(project, ec);
}

// The reported case: the last scene the editor opened belongs to the previous project, which
// must not count as this project's.
TEST(ProjectStartupScene, AnotherProjectsRecentSceneIsNotThisProjectsStartupScene)
{
    const fs::path root = MakeScratchRoot("ge_startup_other");
    WriteFile(root / "Breakout" / "Assets" / "Scenes" / "Breakout.scene", "{}");
    fs::create_directories(root / "Pong" / "Assets");

    ProjectStartupSceneCandidates candidates;
    candidates.RecentScenes = {root / "Breakout" / "Assets" / "Scenes" / "Breakout.scene"};
    EXPECT_FALSE(ResolveProjectStartupScene(root / "Pong", candidates).has_value());

    // Nor does a named scene that walks out of the project's Assets folder.
    candidates.StartupScene = "../../Breakout/Assets/Scenes/Breakout.scene";
    EXPECT_FALSE(ResolveProjectStartupScene(root / "Pong", candidates).has_value());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(ProjectStartupScene, ANewProjectNamesItsGameConfigSceneOverItsOtherScenes)
{
    const fs::path project = MakeScratchRoot("ge_startup_gameconfig");
    WriteFile(project / "Assets" / "Scenes" / "Menu.scene", "{}");
    WriteFile(project / "Assets" / "Scenes" / "Game.scene", "{}");
    EXPECT_EQ(FindNewProjectStartupScene(project), "") << "two scenes and no game.config name none";

    WriteFile(project / "game.config", R"({"gameName": "Demo", "startupScene": "Scenes/Game.scene"})");
    EXPECT_EQ(FindNewProjectStartupScene(project), "Scenes/Game.scene");

    std::error_code ec;
    fs::remove_all(project, ec);
}

// Creating a project from a template whose only scene is not named anywhere stamps that scene as
// project.startupScene, so the new project opens on it.
TEST(ProjectStartupScene, CreatingAProjectFromATemplateStampsItsStartupScene)
{
    const fs::path root = MakeScratchRoot("ge_startup_create");
    WriteFile(root / "templates" / "basic" / "Assets" / "Scenes" / "Main.scene", "{}");
    WriteFile(root / "templates" / "basic" / "Assets" / "Scenes" / "Main.backup.scene", "{}");
    fs::create_directories(root / "user");
    const ScopedEditorUserDataRoot userDataRoot(root / "user");

    JobSystem::WorkStealingThreadPool pool(1);
    GameEngine::Editor::ProjectAcquireStatus status;
    const fs::path destination = root / "projects" / "Basic";
    {
        GameEngine::Editor::ProjectAcquisition acquisition(pool);
        GameEngine::Editor::ProjectAcquireRequest request;
        request.Entry.Id = "basic";
        request.Entry.Name = "Basic";
        request.Entry.Source.Type = GameEngine::Editor::ProjectSourceType::Local;
        request.Entry.Source.Path = "basic";
        request.CatalogRoot = root / "templates";
        request.Destination = destination;
        ASSERT_TRUE(acquisition.Start(request));

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (acquisition.Poll(status) && status.State != GameEngine::Editor::ProjectAcquireState::Running)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ASSERT_EQ(status.State, GameEngine::Editor::ProjectAcquireState::Succeeded) << status.Error;

    GameEngine::Editor::SettingsStore settings = GameEngine::Editor::OpenProjectSettings(destination);
    ASSERT_TRUE(settings.Load());
    std::string startupScene;
    EXPECT_TRUE(settings.TryGetString(kProjectStartupSceneKey, startupScene));
    EXPECT_EQ(startupScene, "Scenes/Main.scene");

    std::error_code ec;
    fs::remove_all(root, ec);
}

// Every built-in template ships a thumbnail and a payload with one scene, so a project created
// from it opens on that scene instead of whatever world the editor had loaded. Reads the source
// manifest: the templates are source data and this suite does not stage the editor.
TEST(ProjectStartupScene, EveryBuiltInTemplateStampsItsStartupScene)
{
    const fs::path catalogRoot = fs::path(GE_EDITOR_SOURCE_DIR) / "ProjectTemplates";
    std::ifstream manifestFile(catalogRoot / "manifest.json");
    ASSERT_TRUE(manifestFile.good()) << (catalogRoot / "manifest.json").string();
    const std::string manifestText((std::istreambuf_iterator<char>(manifestFile)), std::istreambuf_iterator<char>());
    GameEngine::Editor::ProjectCatalog catalog;
    std::string error;
    ASSERT_TRUE(GameEngine::Editor::ParseProjectCatalog(manifestText, catalogRoot.string(), catalog, &error)) << error;
    ASSERT_FALSE(catalog.Entries.empty());

    const fs::path root = MakeScratchRoot("ge_startup_builtin");
    fs::create_directories(root / "user");
    const ScopedEditorUserDataRoot userDataRoot(root / "user");
    JobSystem::WorkStealingThreadPool pool(1);
    for (const GameEngine::Editor::ProjectCatalogEntry& entry : catalog.Entries)
    {
        SCOPED_TRACE(entry.Id);
        EXPECT_EQ(entry.Source.Type, GameEngine::Editor::ProjectSourceType::Local) << "no payload";
        if (entry.Source.Type != GameEngine::Editor::ProjectSourceType::Local)
            continue;
        EXPECT_TRUE(fs::is_directory(catalogRoot / entry.Source.Path));
        EXPECT_TRUE(fs::is_regular_file(entry.ThumbnailRef)) << "thumbnail: '" << entry.ThumbnailRef << "'";

        GameEngine::Editor::ProjectAcquireStatus status;
        const fs::path destination = root / "projects" / entry.Id;
        {
            GameEngine::Editor::ProjectAcquisition acquisition(pool);
            GameEngine::Editor::ProjectAcquireRequest request;
            request.Entry = entry;
            request.CatalogRoot = catalogRoot;
            request.Destination = destination;
            ASSERT_TRUE(acquisition.Start(request));

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (acquisition.Poll(status) && status.State != GameEngine::Editor::ProjectAcquireState::Running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ASSERT_EQ(status.State, GameEngine::Editor::ProjectAcquireState::Succeeded) << status.Error;

        GameEngine::Editor::SettingsStore settings = GameEngine::Editor::OpenProjectSettings(destination);
        ASSERT_TRUE(settings.Load());
        std::string startupScene;
        EXPECT_TRUE(settings.TryGetString(kProjectStartupSceneKey, startupScene));
        EXPECT_TRUE(fs::is_regular_file(destination / "Assets" / startupScene)) << startupScene;
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}
