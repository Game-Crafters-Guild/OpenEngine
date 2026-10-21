#include "WebExportContent.h"

#include "Engine/Build/AssetCollector.h"
#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/SceneExportStrip.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace GameEngine
{
namespace
{

std::string GenericUtf8(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::filesystem::path AbsoluteSource(const AssetManifestEntry& entry, const std::filesystem::path& projectAssetRoot)
{
    return entry.sourcePath.is_absolute() ? entry.sourcePath : projectAssetRoot / entry.sourcePath;
}

// Copies the manifest's scenes under `scenesRoot` at their staged paths and strips them there, as
// the desktop export strips the scenes it stages.
bool StageStrippedScenes(const AssetManifest& manifest, const std::filesystem::path& projectAssetRoot,
                         const std::filesystem::path& scenesRoot, std::string& outError)
{
    AssetManifest scenes;
    for (const AssetManifestEntry& entry : manifest.entries)
    {
        if (entry.type != AssetType::Scene || entry.sourcePath.empty())
            continue;
        const std::filesystem::path staged = scenesRoot / entry.outputPath;
        std::error_code ec;
        std::filesystem::create_directories(staged.parent_path(), ec);
        std::filesystem::copy_file(AbsoluteSource(entry, projectAssetRoot), staged,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            outError = "Failed to stage the scene " + entry.outputPath.generic_string() + " for the web export: " +
                       ec.message();
            return false;
        }
        scenes.entries.push_back(entry);
    }
    std::string reason;
    if (!StripEditorOnlyComponentsFromStagedScenes(scenesRoot, scenes, {}, reason))
    {
        outError = "Failed to strip the editor-only components from a scene for the web export: " + reason;
        return false;
    }
    return true;
}

} // namespace

bool StageWebExportContent(AssetManager& assets, const BuildSettings& settings,
                           const std::filesystem::path& projectAssetRoot, const std::filesystem::path& scratchRoot,
                           std::vector<std::string>& outWarnings, std::string& outError)
{
    AssetCollector collector(assets);
    AssetManifest manifest;
    if (!collector.CollectGameContent(settings.scenes, settings.renderPipelinePath, projectAssetRoot, manifest,
                                      outWarnings, outError))
        return false;

    const std::filesystem::path scenesRoot = scratchRoot / "scenes";
    if (!StageStrippedScenes(manifest, projectAssetRoot, scenesRoot, outError))
        return false;

    nlohmann::json files = nlohmann::json::array();
    for (const AssetManifestEntry& entry : manifest.entries)
    {
        if (entry.sourcePath.empty())
            continue;
        const std::filesystem::path source = entry.type == AssetType::Scene
                                                 ? scenesRoot / entry.outputPath
                                                 : AbsoluteSource(entry, projectAssetRoot);
        files.push_back({{"source", GenericUtf8(source)}, {"path", GenericUtf8(entry.outputPath)}});
    }

    const std::filesystem::path listPath = scratchRoot / kWebExportContentListName;
    std::ofstream out(listPath, std::ios::binary | std::ios::trunc);
    out << nlohmann::json{{"files", std::move(files)}}.dump(2) << '\n';
    if (!out)
    {
        outError = "Failed to write the web export content list at " + listPath.generic_string() +
                   ". Check that the build folder is writable.";
        return false;
    }
    return true;
}
} // namespace GameEngine
