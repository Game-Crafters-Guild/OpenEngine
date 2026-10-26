#include "TerrainECS/TerrainSurfaceStore.h"

#include "TerrainECS/TerrainService.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <system_error>

namespace GameEngine::TerrainECS
{

namespace
{

using PageStreaming::PageFieldKind;

// The name of every surface store of one terrain, up to and including the dash before its plane.
std::string SurfaceStoreStem(const GUID& scene, std::string_view entityTag)
{
    return std::format("{}-{:016x}-", scene.ToString(), Hashing::Fnv1a64(entityTag));
}

// The format and filter of a surface store of `plane`.
void SetSurfaceEncoding(PageFieldKind plane, PageStreaming::PageStoreHeader& header)
{
    switch (plane)
    {
    case PageFieldKind::Normal:
        header.Format = PageStreaming::PageFieldFormat::Bc5;
        header.Filter = PageStreaming::PageFieldFilter::NormalTent;
        return;
    case PageFieldKind::GrassControl:
        header.Format = PageStreaming::PageFieldFormat::Rg8;
        header.Filter = PageStreaming::PageFieldFilter::Tent;
        return;
    case PageFieldKind::Color:
        header.Format = PageStreaming::PageFieldFormat::Rgba8Srgb;
        header.Filter = PageStreaming::PageFieldFilter::PremultipliedTent;
        return;
    default:
        assert(plane == PageFieldKind::Splat);
        header.Format = PageStreaming::PageFieldFormat::Rgba8;
        header.Filter = PageStreaming::PageFieldFilter::WeightsTent;
        return;
    }
}

} // namespace

const char* SurfacePlaneName(PageFieldKind plane)
{
    switch (plane)
    {
    case PageFieldKind::Splat: return "splat";
    case PageFieldKind::Normal: return "normal";
    case PageFieldKind::GrassControl: return "grass";
    case PageFieldKind::Color: return "color";
    default: return "other";
    }
}

PageStreaming::PageStoreHeader MakeSurfaceStoreHeader(const TiledTerrainData& tiled, PageFieldKind plane, uint64 key)
{
    PageStreaming::PageStoreHeader header;
    SetSurfaceEncoding(plane, header);
    header.SamplesX = tiled.Config.TilesPerAxisX * (tiled.Config.TileConfig.HeightmapWidth - 1u) + 1u;
    header.SamplesZ = tiled.Config.TilesPerAxisZ * (tiled.Config.TileConfig.HeightmapHeight - 1u) + 1u;
    header.Key = key;
    return header;
}

std::filesystem::path SurfaceSamplesCachePath(const std::filesystem::path& store)
{
    std::filesystem::path cache = store;
    cache.replace_extension(".samples" + store.extension().string());
    return cache;
}

bool RemoveSurfaceStore(const std::filesystem::path& store)
{
    std::error_code ec;
    std::filesystem::remove(store, ec);
    std::filesystem::remove(SurfaceSamplesCachePath(store), ec);
    return !std::filesystem::exists(store, ec) && !std::filesystem::exists(SurfaceSamplesCachePath(store), ec);
}

std::filesystem::path SurfaceStoreFile(const std::filesystem::path& directory, const GUID& scene,
                                       std::string_view entityTag, PageFieldKind plane, uint64 key)
{
    if (directory.empty() || scene.IsNull() || entityTag.empty())
        return {};
    const std::string stem = SurfaceStoreStem(scene, entityTag);
    return directory / std::format("{}{}-{:016x}.gepage", stem, SurfacePlaneName(plane), key);
}

void RemoveSurfaceStores(const std::filesystem::path& directory, const GUID& scene, std::string_view entityTag,
                         PageFieldKind plane)
{
    std::error_code error;
    const std::string prefix = SurfaceStoreStem(scene, entityTag) + SurfacePlaneName(plane) + "-";
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error))
    {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(prefix) && name.ends_with(".gepage") && !name.ends_with(".samples.gepage"))
            RemoveSurfaceStore(entry.path());
    }
}

std::vector<SceneSurfaceStore> SceneSurfaceStores(const std::filesystem::path& directory, const GUID& scene)
{
    std::vector<SceneSurfaceStore> stores;
    std::error_code error;
    if (directory.empty() || scene.IsNull() || !std::filesystem::is_directory(directory, error))
        return stores;
    const std::string prefix = scene.ToString() + "-";
    constexpr std::size_t kTerrainDigits = 16;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error))
    {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with(prefix) || !name.ends_with(".gepage") || name.ends_with(".samples.gepage") ||
            name.size() <= prefix.size() + kTerrainDigits + 1u)
            continue;
        const std::string_view planeAndKey = std::string_view(name).substr(prefix.size() + kTerrainDigits + 1u);
        for (const PageFieldKind plane : kSurfacePlanes)
            if (planeAndKey.starts_with(std::string(SurfacePlaneName(plane)) + "-"))
                stores.push_back({name.substr(prefix.size(), kTerrainDigits), plane, entry.path()});
    }
    std::sort(stores.begin(), stores.end(), [](const SceneSurfaceStore& a, const SceneSurfaceStore& b) {
        return a.Terrain != b.Terrain ? a.Terrain < b.Terrain : a.Plane < b.Plane;
    });
    return stores;
}

} // namespace GameEngine::TerrainECS
