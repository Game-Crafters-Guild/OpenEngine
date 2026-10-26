#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{

class AssetRegistry;
struct AssetManifest;

/// What the export did with one shipped heightmap.
struct TerrainPageExportResult
{
    /// <contentRoot>/Assets/Cooked/Terrain/<guid>.geterrain: one per heightmap, one per scene terrain
    std::vector<std::filesystem::path> Containers;
    std::vector<std::string> Warnings;             ///< heightmaps that could not be cooked, each with the fix
};

/// Packs the cooked pages of every shipped terrain into <contentRoot>/Assets/Cooked/Terrain (design D5): one container
/// per raw heightmap (.r16, .r32; cooked in <projectRoot>/.Cache/TerrainPages when stale) and one per scene terrain
/// whose surface stores that cache holds. Raising `cancel` stops the cook and the heightmaps after it, as warnings.
TerrainPageExportResult ExportTerrainPages(const std::filesystem::path& projectRoot,
                                           const std::filesystem::path& contentRoot, const AssetManifest& manifest,
                                           const AssetRegistry& registry, JobSystem::WorkStealingThreadPool* pool,
                                           const std::atomic<bool>* cancel);

} // namespace GameEngine
