#include "Engine/Build/TerrainPageExport.h"

#include "Engine/Build/AssetCollector.h"

#include "PageStreaming/HeightStoreCache.h"
#include "PageStreaming/TerrainPageContainer.h"
#include "TerrainECS/RawHeightmap.h"
#include "TerrainECS/TerrainSurfaceStore.h"

namespace GameEngine
{

namespace
{

// Packs the surface stores of each terrain of `scene` into one container a terrain,
// <contentRoot>/Assets/Cooked/Terrain/<scene guid>-<terrain>.geterrain. A Normal store's samples
// cache is a bake cache and stays behind.
void ExportSceneSurfaceStores(const std::filesystem::path& cacheDirectory, const std::filesystem::path& contentRoot,
                              const GUID& scene, TerrainPageExportResult& result)
{
    const std::vector<TerrainECS::SceneSurfaceStore> stores = TerrainECS::SceneSurfaceStores(cacheDirectory, scene);
    for (std::size_t first = 0; first < stores.size();)
    {
        std::vector<PageStreaming::TerrainContainerField> fields;
        std::size_t next = first;
        for (; next < stores.size() && stores[next].Terrain == stores[first].Terrain; ++next)
            fields.push_back({stores[next].Plane, stores[next].File});
        const std::string name = scene.ToString() + "-" + stores[first].Terrain + ".geterrain";
        const std::filesystem::path container = contentRoot / "Assets" / "Cooked" / "Terrain" / name;
        if (std::string reason = PageStreaming::WriteTerrainContainer(container, fields); !reason.empty())
            result.Warnings.push_back("Terrain surface pages: " + name + ": " + reason);
        else
            result.Containers.push_back(container);
        first = next;
    }
}

} // namespace

TerrainPageExportResult ExportTerrainPages(const std::filesystem::path& projectRoot,
                                           const std::filesystem::path& contentRoot, const AssetManifest& manifest,
                                           const AssetRegistry& registry, JobSystem::WorkStealingThreadPool* pool,
                                           const std::atomic<bool>* cancel)
{
    TerrainPageExportResult result;
    const std::filesystem::path cacheDirectory = projectRoot / ".Cache" / "TerrainPages";
    const PageStreaming::HeightStoreCache cache(cacheDirectory);
    for (const AssetManifestEntry& entry : manifest.entries)
    {
        if (entry.type != AssetType::TerrainHeightmap)
            continue;
        if (cancel && cancel->load())
        {
            result.Warnings.push_back("Terrain pages: the export was cancelled before " +
                                      entry.sourcePath.filename().string());
            continue;
        }
        const std::string name = entry.sourcePath.filename().string();
        // The grid the terrain decodes the file on: the import settings, one of them, or a square file.
        TerrainECS::RawHeightmapLayout declared;
        TerrainECS::RawHeightmapLayout grid;
        std::string reason = TerrainECS::ResolveDeclaredRawHeightmapLayout(
            TerrainECS::ReadRawHeightmapSettings(registry, entry.sourcePath), declared);
        if (reason.empty())
            reason = TerrainECS::ResolveRawHeightmapFileLayout(entry.sourcePath, declared.Width, declared.Height, grid);
        if (!reason.empty())
        {
            result.Warnings.push_back("Terrain pages: " + name + ": " + reason);
            continue;
        }
        const PageStreaming::HeightStoreCache::Result store =
            cache.Ensure(entry.guid, entry.sourcePath, grid.Width, grid.Height, pool, nullptr, cancel);
        if (!store.Error.empty())
        {
            result.Warnings.push_back("Terrain pages: " + name + ": " + store.Error);
            continue;
        }
        const std::filesystem::path container =
            contentRoot / "Assets" / "Cooked" / "Terrain" / (entry.guid.ToString() + ".geterrain");
        const PageStreaming::TerrainContainerField field{PageStreaming::PageFieldKind::Height, store.Store};
        if (reason = PageStreaming::WriteTerrainContainer(container, {&field, 1}); !reason.empty())
        {
            result.Warnings.push_back("Terrain pages: " + name + ": " + reason);
            continue;
        }
        result.Containers.push_back(container);
    }
    for (const AssetManifestEntry& entry : manifest.entries)
        if (entry.type == AssetType::Scene)
            ExportSceneSurfaceStores(cacheDirectory, contentRoot, entry.guid, result);
    return result;
}

} // namespace GameEngine
