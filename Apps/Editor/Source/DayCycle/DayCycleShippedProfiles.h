#pragma once

#include <filesystem>
#include <memory>
#include <vector>

namespace GameEngine
{
class AssetManager;
struct DayCycleProfileData;
}

namespace GameEngine::Editor
{

/// The Day Cycle profiles the engine ships, in the editor source's DayCycles folder: Default first,
/// then the others by name. Empty when the editor source is not mounted.
std::vector<std::filesystem::path> ShippedDayCycleProfiles(const AssetManager& assets);

/// The shipped Default profile: the day every volume stack starts from.
std::filesystem::path DefaultDayCycleProfilePath(const AssetManager& assets);

/// The shipped Default profile's data, as its file reads now: the values a track a profile leaves out
/// takes. Null when the editor source is not mounted or the file does not parse.
std::shared_ptr<const DayCycleProfileData> ReadDefaultDayCycleProfile(const AssetManager& assets);

} // namespace GameEngine::Editor
