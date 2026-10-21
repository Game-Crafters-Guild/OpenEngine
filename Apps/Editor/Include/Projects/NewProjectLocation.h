#pragma once

#include <filesystem>

namespace GameEngine::Editor
{

// Editor preferences key: the parent folder the last project was created or imported in. Distinct from
// lastProjectPath, which is the opened project's own folder.
inline constexpr const char* kNewProjectLocationPrefKey = "newProjectLocation";

// The folder the project picker's Location field opens at: the parent folder of the last project
// created or imported, while that folder exists; `fallback` otherwise.
std::filesystem::path LoadNewProjectLocation(const std::filesystem::path& fallback);

// Remembers `parent` as the folder the next new project starts in.
void SaveNewProjectLocation(const std::filesystem::path& parent);

} // namespace GameEngine::Editor
