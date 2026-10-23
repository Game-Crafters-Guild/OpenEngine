#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace GameEngine
{
class AssetManager;
struct DayCycleProfileData;
} // namespace GameEngine

namespace GameEngine::Editor
{

/// What Save does for a Day Cycle profile. A save writes only into the project's own assets: a
/// profile there saves in place; a profile the editor ships, or one a package holds, is never
/// written and offers a copy saved into the project instead.
enum class DayCycleSaveAction : std::uint8_t
{
    SaveInPlace,
    SaveCopyToProject,
};

/// The save a profile owned by the asset source `sourceAlias` offers.
DayCycleSaveAction DayCycleSaveActionForSource(std::string_view sourceAlias);

/// The save the profile at `profilePath` offers, from the alias of the source that owns it.
DayCycleSaveAction DayCycleSaveActionFor(const AssetManager& assets, const std::filesystem::path& profilePath);

/// Writes `data` over the profile at `profilePath` and drives the asset pipeline for the write.
/// Refused, with the fix in `outError`, when the profile is not the project's.
bool SaveDayCycleProfile(AssetManager& assets, const std::filesystem::path& profilePath,
                         const DayCycleProfileData& data, std::string& outError);

/// Writes `data` as a new profile named "<name> copy" in the project's DayCycles folder, the file named
/// after it and numbered when the name is taken. The path written, or empty with the reason in `outError`.
std::filesystem::path SaveDayCycleProfileCopy(AssetManager& assets, const DayCycleProfileData& data,
                                              std::string& outError);

} // namespace GameEngine::Editor
