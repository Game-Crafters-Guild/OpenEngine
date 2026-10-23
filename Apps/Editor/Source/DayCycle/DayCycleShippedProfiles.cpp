#include "DayCycle/DayCycleShippedProfiles.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/DayCycleProfile.h"

#include <algorithm>
#include <string>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kShippedFolder = "DayCycles";
constexpr const char* kDefaultFile = "Default.daycycle.json";
constexpr std::string_view kExtension = ".daycycle.json";

std::filesystem::path ShippedFolder(const AssetManager& assets)
{
    const std::filesystem::path root = assets.GetRegistry().GetSourceRoot(kAssetSourceAliasEditor);
    return root.empty() ? std::filesystem::path{} : root / kShippedFolder;
}
} // namespace

std::vector<std::filesystem::path> ShippedDayCycleProfiles(const AssetManager& assets)
{
    std::vector<std::filesystem::path> profiles;
    const std::filesystem::path folder = ShippedFolder(assets);
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec))
        return profiles;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
    {
        const std::string name = entry.path().filename().string();
        if (name.size() > kExtension.size() && name.ends_with(kExtension))
            profiles.push_back(entry.path());
    }
    std::sort(profiles.begin(), profiles.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        const bool aDefault = a.filename() == kDefaultFile;
        const bool bDefault = b.filename() == kDefaultFile;
        return aDefault != bDefault ? aDefault : a.filename() < b.filename();
    });
    return profiles;
}

std::filesystem::path DefaultDayCycleProfilePath(const AssetManager& assets)
{
    const std::filesystem::path folder = ShippedFolder(assets);
    return folder.empty() ? folder : folder / kDefaultFile;
}

std::shared_ptr<const DayCycleProfileData> ReadDefaultDayCycleProfile(const AssetManager& assets)
{
    const std::filesystem::path path = DefaultDayCycleProfilePath(assets);
    std::string text;
    auto data = std::make_shared<DayCycleProfileData>();
    std::string error;
    if (path.empty() || !ReadFileTextShared(path, text) || !ParseDayCycleProfile(text, *data, error))
        return nullptr;
    return data;
}

} // namespace GameEngine::Editor
