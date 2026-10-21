// The project picker's Location field opens at the parent folder of the last project created,
// not at the opened project's own folder, and at the fallback when that folder is gone.

#include <gtest/gtest.h>

#include "Editor/Settings/SettingsStore.h"
#include "Projects/NewProjectLocation.h"
#include "ScopedEditorUserDataRoot.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using GameEngine::Editor::LoadNewProjectLocation;
using GameEngine::Editor::SaveNewProjectLocation;

TEST(NewProjectLocation, OpensAtTheLastParentFolderAndFallsBackWhenItIsGone)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("ge_new_project_location_" + std::to_string(stamp));
    fs::create_directories(root / "user");
    fs::create_directories(root / "Games" / "Pong");
    const ScopedEditorUserDataRoot userDataRoot(root / "user");
    const fs::path fallback = root / "home";

    {
        // The opened project's own path is not where the next project is created.
        GameEngine::Editor::SettingsStore prefs = GameEngine::Editor::OpenEditorPreferences();
        (void)prefs.Load();
        prefs.SetString("lastProjectPath", (root / "Games" / "Pong").string());
        ASSERT_TRUE(prefs.Save());
    }
    EXPECT_EQ(LoadNewProjectLocation(fallback), fallback) << "the first run opens at the fallback";

    SaveNewProjectLocation(root / "Games");
    EXPECT_EQ(LoadNewProjectLocation(fallback), root / "Games");

    std::error_code ec;
    fs::remove_all(root / "Games", ec);
    EXPECT_EQ(LoadNewProjectLocation(fallback), fallback);

    fs::remove_all(root, ec);
}
