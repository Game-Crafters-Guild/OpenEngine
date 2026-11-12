#include "TerrainECS/TerrainSurface/TerrainSurfaceStore.h"
#include "TerrainSurface/TerrainSurfaceSessionStore.h"

#include "TerrainECS/TerrainService.h"
#include "FileSystem/ScopedFileLock.h"
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

// The filter of a surface store of `plane`.
PageStreaming::PageFieldFilter SurfacePlaneFilter(PageFieldKind plane)
{
    switch (plane)
    {
    case PageFieldKind::Normal: return PageStreaming::PageFieldFilter::NormalTent;
    case PageFieldKind::GrassControl: return PageStreaming::PageFieldFilter::Tent;
    case PageFieldKind::Color: return PageStreaming::PageFieldFilter::PremultipliedTent;
    default: assert(plane == PageFieldKind::Splat); return PageStreaming::PageFieldFilter::WeightsTent;
    }
}

} // namespace

PageStreaming::PageFieldFormat SurfacePlaneFormat(PageFieldKind plane)
{
    switch (plane)
    {
    case PageFieldKind::Normal: return PageStreaming::PageFieldFormat::Bc5;
    case PageFieldKind::GrassControl: return PageStreaming::PageFieldFormat::Rg8;
    case PageFieldKind::Color: return PageStreaming::PageFieldFormat::Rgba8Srgb;
    default: assert(plane == PageFieldKind::Splat); return PageStreaming::PageFieldFormat::Rgba8;
    }
}

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

PageStreaming::PageStoreHeader MakeSurfaceStoreHeader(const TiledTerrainConfig& config, PageFieldKind plane, uint64 key)
{
    PageStreaming::PageStoreHeader header;
    header.Format = SurfacePlaneFormat(plane);
    header.Filter = SurfacePlaneFilter(plane);
    header.SamplesX = config.TilesPerAxisX * (config.TileConfig.HeightmapWidth - 1u) + 1u;
    header.SamplesZ = config.TilesPerAxisZ * (config.TileConfig.HeightmapHeight - 1u) + 1u;
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

std::filesystem::path SessionStoreRoot(const std::filesystem::path& cacheDirectory)
{
    return cacheDirectory / "Session";
}

std::filesystem::path SessionStoreDirectory(const std::filesystem::path& cacheDirectory, const GUID& session)
{
    return SessionStoreRoot(cacheDirectory) / session.ToString();
}

std::filesystem::path SessionStoreLock(const std::filesystem::path& cacheDirectory, const GUID& session)
{
    return SessionStoreRoot(cacheDirectory) / (session.ToString() + ".lock");
}

void RemoveOrphanSessionStores(const std::filesystem::path& cacheDirectory)
{
    std::error_code error;
    const std::filesystem::path root = SessionStoreRoot(cacheDirectory);
    std::vector<std::filesystem::path> sessions;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root, error))
        if (entry.is_directory(error))
            sessions.push_back(entry.path());
    for (const std::filesystem::path& session : sessions)
    {
        std::filesystem::path lockFile = session;
        lockFile += ".lock";
        const FileSystem::ScopedFileLock lock(lockFile, FileSystem::ScopedFileLock::Mode::Exclusive, false);
        if (!lock.IsLocked())
            continue;
        std::filesystem::remove_all(session, error);
        std::filesystem::remove(lockFile, error);
    }
}

} // namespace GameEngine::TerrainECS
