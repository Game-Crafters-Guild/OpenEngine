#pragma once

#include "AssetCore/GUID.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "Types/Types.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::TerrainECS
{

struct TiledTerrainConfig;

/// The planes of a tiled terrain's surface stores (design page-streaming 3.8).
inline constexpr std::array<PageStreaming::PageFieldKind, 4> kSurfacePlanes = {
    PageStreaming::PageFieldKind::Splat, PageStreaming::PageFieldKind::Normal, PageStreaming::PageFieldKind::GrassControl,
    PageStreaming::PageFieldKind::Color};

/// The bit of `plane` in a mask of surface planes.
constexpr uint8 SurfacePlaneBit(PageStreaming::PageFieldKind plane)
{
    return static_cast<uint8>(1u << static_cast<uint8>(plane));
}
inline constexpr uint8 kEverySurfacePlane = 0xFFu;
/// The surface planes whose bake reads the grass regions.
inline constexpr uint8 kGrassSurfacePlanes =
    SurfacePlaneBit(PageStreaming::PageFieldKind::GrassControl) | SurfacePlaneBit(PageStreaming::PageFieldKind::Color);

/// The format of a `plane` surface store's samples: Rgba8 (Splat), Bc5 (Normal), Rg8 (GrassControl) or
/// Rgba8Srgb (Color).
PageStreaming::PageFieldFormat SurfacePlaneFormat(PageStreaming::PageFieldKind plane);
/// The word of `plane` in a surface store's name: splat, normal, grass or color.
const char* SurfacePlaneName(PageStreaming::PageFieldKind plane);

/// The header of the `plane` store under `key` of a tiled terrain of `config`, on its height lattice: Splat
/// (RGBA8 weights, the weights tent), Normal (Bc5, the normal tent), GrassControl (Rg8, the tent) or
/// Color (premultiplied sRGB RGBA8, the premultiplied tent).
PageStreaming::PageStoreHeader MakeSurfaceStoreHeader(const TiledTerrainConfig& config,
                                                      PageStreaming::PageFieldKind plane, uint64 key);

/// The cache beside the Normal store at `store`: its unencoded samples, from which its coarser pages
/// are filtered. The bake makes it again; an export never packs it.
std::filesystem::path SurfaceSamplesCachePath(const std::filesystem::path& store);

/// Deletes the surface store at `store` and its samples cache. False when a file stays.
bool RemoveSurfaceStore(const std::filesystem::path& store);

/// The surface store of plane `plane` of the terrain loaded from scene `scene` with scene tag
/// `entityTag`, under bake key `key`: <directory>/<scene GUID>-<FNV-1a 64 of the tag, 16 hex
/// digits>-<plane>-<key, 16 hex digits>.gepage. Empty when the terrain has no identity.
std::filesystem::path SurfaceStoreFile(const std::filesystem::path& directory, const GUID& scene,
                                       std::string_view entityTag, PageStreaming::PageFieldKind plane, uint64 key);

/// Deletes every `plane` store of that terrain in `directory`, whatever its key, with its samples cache.
void RemoveSurfaceStores(const std::filesystem::path& directory, const GUID& scene, std::string_view entityTag,
                         PageStreaming::PageFieldKind plane);

/// One surface store of a scene's terrain, as SceneSurfaceStores finds it.
struct SceneSurfaceStore
{
    std::string Terrain; ///< the FNV-1a 64 of the terrain's scene tag, 16 hex digits
    PageStreaming::PageFieldKind Plane = PageStreaming::PageFieldKind::Splat;
    std::filesystem::path File;

    bool operator==(const SceneSurfaceStore&) const = default;
};

/// The surface stores of scene `scene` in `directory`, by terrain then plane: every plane of every
/// terrain, the samples caches not included.
std::vector<SceneSurfaceStore> SceneSurfaceStores(const std::filesystem::path& directory, const GUID& scene);

} // namespace GameEngine::TerrainECS
