#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

// Project settings key naming the scene the editor opens with the project; the
// value is asset-relative ("Scenes/Main.scene"), the form build.globalScenes uses.
inline constexpr const char* kProjectStartupSceneKey = "project.startupScene";

// What a project says about the scene it opens on, gathered by the caller.
struct ProjectStartupSceneCandidates
{
    // build.globalScenes, asset-relative, in build order.
    std::vector<std::string> GlobalBuildScenes;
    // project.startupScene, asset-relative; empty when unset.
    std::string StartupScene;
    // The editor's recent scenes, absolute, most recent first; only those inside
    // the project count as the project's last-opened scene.
    std::vector<std::filesystem::path> RecentScenes;
};

// The scene opening `projectRoot` loads: the first build scene, else the
// project's startupScene, else its last-opened scene, else none (the caller
// opens an empty scene). A named scene that is missing or outside the
// project's Assets folder is skipped with a warning and the next rule applies.
std::optional<std::filesystem::path> ResolveProjectStartupScene(
    const std::filesystem::path& projectRoot, const ProjectStartupSceneCandidates& candidates);

// The startup scene a freshly created project names, asset-relative: the
// startupScene of its game.config when that scene exists, else its only scene
// file. Empty when neither applies.
std::string FindNewProjectStartupScene(const std::filesystem::path& projectRoot);

} // namespace GameEngine::Editor
