#include "DayCycle/DayCycleProfileSave.h"

#include "Assets/AssetCreation.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/DayCycleProfile.h"

#include <fstream>

namespace GameEngine::Editor
{
namespace
{
constexpr std::string_view kDayCycleProfileExtension = ".daycycle.json";
constexpr std::string_view kProjectDayCycleFolder = "DayCycles";

bool WriteProfileFile(AssetManager& assets, const std::filesystem::path& path, const DayCycleProfileData& data,
                      std::string& outError)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    ExpectedAssetWrite write = assets.ExpectWrite(path);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << WriteDayCycleProfile(data);
        if (!out.good())
        {
            outError = "Could not write " + path.string() + "; check that the file is not read-only or open elsewhere.";
            return false;
        }
    }
    write.Report(path);
    return true;
}

// A profile name as a file name: the characters a file name cannot hold become spaces.
std::string FileStemFor(const std::string& name)
{
    std::string stem;
    for (const char c : name)
        stem += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                    ? ' '
                    : c;
    return stem.empty() ? std::string("Day Cycle") : stem;
}
} // namespace

DayCycleSaveAction DayCycleSaveActionForSource(std::string_view sourceAlias)
{
    return sourceAlias == kAssetSourceAliasProject ? DayCycleSaveAction::SaveInPlace
                                                   : DayCycleSaveAction::SaveCopyToProject;
}

DayCycleSaveAction DayCycleSaveActionFor(const AssetManager& assets, const std::filesystem::path& profilePath)
{
    return DayCycleSaveActionForSource(assets.GetRegistry().GetImportSettingsOrigin(profilePath).SourceAlias);
}

bool SaveDayCycleProfile(AssetManager& assets, const std::filesystem::path& profilePath,
                         const DayCycleProfileData& data, std::string& outError)
{
    if (DayCycleSaveActionFor(assets, profilePath) != DayCycleSaveAction::SaveInPlace)
    {
        outError = "This profile ships with the editor or a package and is not saved in place; use Save a copy to the "
                   "project, then edit the copy.";
        return false;
    }
    return WriteProfileFile(assets, profilePath, data, outError);
}

std::filesystem::path SaveDayCycleProfileCopy(AssetManager& assets, const DayCycleProfileData& data,
                                              std::string& outError)
{
    const std::filesystem::path projectRoot = assets.GetRegistry().GetSourceRoot(kAssetSourceAliasProject);
    if (projectRoot.empty())
    {
        outError = "No project is open; open a project to save a copy of the profile into it.";
        return {};
    }
    // The copy is named apart from the profile it copies, so the two never read the same in a list.
    DayCycleProfileData copy = data;
    copy.Name = (data.Name.empty() ? std::string("Day Cycle") : data.Name) + " copy";
    const std::filesystem::path path = MakeUniqueFilePath(projectRoot / kProjectDayCycleFolder, FileStemFor(copy.Name),
                                                          std::string(kDayCycleProfileExtension));
    if (path.empty())
    {
        outError = "No free file name for the copy in " + (projectRoot / kProjectDayCycleFolder).string() + ".";
        return {};
    }
    if (!WriteProfileFile(assets, path, copy, outError))
        return {};
    return path;
}

} // namespace GameEngine::Editor
