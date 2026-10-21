#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
class AssetManager;
struct BuildSettings;

/// The content list's file name under the export's scratch folder.
inline constexpr const char* kWebExportContentListName = "content.json";

/// Stages the project content a Web export ships under `scratchRoot` and writes the list
/// export_web_player.py stages verbatim (--content-list) to `scratchRoot`/kWebExportContentListName.
/// The content is the desktop export's: the manifest AssetCollector::CollectGameContent collects
/// over `settings.scenes` and `settings.renderPipelinePath`, so source files and Editor/ folders
/// never ship, and the scenes are staged under `scratchRoot` and stripped of their editor-only
/// components (StripEditorOnlyComponentsFromStagedScenes) as the desktop export strips them.
/// JSON: {"files": [{"source": "<absolute path>", "path": "<staged path>"}]}.
/// False with `outError` set when the content cannot be collected, a scene cannot be staged or
/// stripped, or the list cannot be written; `outWarnings` receives what ships incomplete.
bool StageWebExportContent(AssetManager& assets, const BuildSettings& settings,
                           const std::filesystem::path& projectAssetRoot, const std::filesystem::path& scratchRoot,
                           std::vector<std::string>& outWarnings, std::string& outError);
} // namespace GameEngine
