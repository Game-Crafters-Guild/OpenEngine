#pragma once

#include "Components/Terrain/Terrain.h"
#include "TerrainECS/CoarseTileStandIn.h"
#include "TerrainECS/DirtyRegionLog.h"
#include "TerrainECS/GeneratedNoiseRelief.h"
#include "TerrainECS/TerrainHeightFunction/TerrainHeightFunctionKey.h"
#include "TerrainECS/TerrainSurface/TerrainSurfacePageSource.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "TerrainECS/TerrainColorMap/TerrainColorMap.h"
#include "TerrainECS/TerrainZonePayload.h"
#include "TerrainECS/TerrainGpuBake.h"
#include "TerrainECS/TerrainGrass/TerrainGrassField.h"
#include "TerrainECS/TerrainReprovisionDebounce.h" // TiledTerrainEdit
#include "CBTTerrain/CBTSphereFaceMap.h"
#include "Terrain/TerrainTypes.h"
#include "Terrain/Heightfield.h"
#include "Terrain/CDLODQuadtree.h"
#include "Terrain/TerrainNoiseStack.h"
#include "AssetCore/AssetReloadInvalidator.h"
#include "AssetCore/GUID.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageAddress.h"
#include "Types/FlatMap.h"
#include "Types/FunctionRef.h"
#include "GenerationalVector/GenerationalVector.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::Components
{
struct WorldTransform;
}

namespace GameEngine::TerrainECS
{
class RevocableTerrainHeightFunction;
class TerrainHeightFunctionInstances;

// The top of the normalized height band: 1 is HeightScale above the terrain's origin. A Generated
// noise's detail band ends here; its larger octaves reach Terrain::TerrainNoiseStack::BandTop.
static constexpr float32 kNominalHeightBandTop = 1.0f;

// All CPU-side data for a single terrain.
struct TerrainData
{
    TerrainGrassField GrassField;
    Terrain::TerrainConfig Config;
    Terrain::HeightfieldData Heightfield;
    Terrain::CDLODQuadtree Quadtree;

    // ---- Splatmap (RGBA8: 4 material layer weights per texel) ----
    std::vector<uint8> Splatmap;          // RGBA8 interleaved, row-major
    uint32 SplatmapWidth = 0;
    uint32 SplatmapHeight = 0;
    bool SplatmapDirty = false;

    // The texels whose splat changed since the colour map last composed over them
    // (TerrainColorMapBaker consumes and clears it). Exclusive max.
    DirtyRegionLog::Region SplatColorDirty;
    bool SplatColorDirtyFull = false;

    // Record a splat write for the GPU upload and the colour map: the whole splat,
    // or one rect of it.
    void MarkSplatChanged()
    {
        SplatmapDirty = true;
        SplatColorDirtyFull = true;
    }
    void MarkSplatChanged(const DirtyRegionLog::Region& region)
    {
        SplatmapDirty = true;
        SplatColorDirty.Union(region);
    }

    // The colour map composed from the splat and its writers (TerrainColorMap.h).
    TerrainColorMap ColorMap;

    // Set by a full splat re-bake (ResetSplatmapAndCommitRange): every texel was
    // reset, so the GPU upload must be full-texture. A height-rect band is only
    // valid for region re-bakes. Cleared by extraction together with SplatmapDirty.
    bool SplatmapFullDirty = false;

    // Height range of the heightfield in normalized units: the nominal band widened to the data
    // (WidenToNominalHeightBand). Set by a full splat bake (ResetSplatmapAndCommitRange) and by
    // each modifier region height bake; GetHeightRangeMeters reads it.
    float32 HeightRangeMinH = 0.0f;
    float32 HeightRangeMaxH = 0.0f;
    bool HeightRangeValid = false;

    // ---- Normal map (R16G16_FLOAT: XZ normal components, row-major) ----
    std::vector<uint8> Normalmap;
    uint32 NormalmapWidth = 0;
    uint32 NormalmapHeight = 0;

    // Reset the splatmap to unbaked (sized to the heightfield, all zero) and commit the
    // heightfield's range, widened to the nominal band, as the terrain's height range
    // (TerrainService::GetHeightRangeMeters reads it).
    void ResetSplatmapAndCommitRange();

    // Cached per-LOD morph ranges (only depend on config, not per-frame data).
    float32 VisRanges[Terrain::kMaxLODLevels] = {};
    float32 MorphStart[Terrain::kMaxLODLevels] = {};
    float32 MorphEnd[Terrain::kMaxLODLevels] = {};
    bool MorphRangesComputed = false;

    // Monotonic counter incremented on each heightfield modification.
    // Used by PhysicsInitSystem (via HeightFieldColliderShape.lastBuiltVersion)
    // to detect when the collision shape needs rebuilding.
    uint64 HeightfieldVersion = 0;

    // Per-version dirty-region journal. Consumers (render extraction, the
    // physics heightfield gather) each keep their own cursor and call
    // CollectSince — nobody clears shared state, so consumers can't starve
    // each other regardless of frame order.
    DirtyRegionLog HeightfieldDirtyLog;

    // Mark the entire heightfield as needing re-upload.
    void MarkFullDirty()
    {
        ++HeightfieldVersion;
        HeightfieldDirtyLog.Append(HeightfieldVersion,
                                   {0, 0,
                                    static_cast<int32>(Heightfield.GetWidth()),
                                    static_cast<int32>(Heightfield.GetHeight())});
    }

    // Mark a sub-region as dirty. Max bounds are exclusive.
    void MarkRegionDirty(int32 minX, int32 minZ, int32 maxX, int32 maxZ)
    {
        ++HeightfieldVersion;
        HeightfieldDirtyLog.Append(HeightfieldVersion, {minX, minZ, maxX, maxZ});
    }
};

// ---- Tile types for tiled terrain ----

struct TileCoord
{
    int32 X = 0;
    int32 Z = 0;

    bool operator==(const TileCoord& o) const { return X == o.X && Z == o.Z; }
    bool operator!=(const TileCoord& o) const { return !(*this == o); }
    // Lexicographic order (Z major, X minor) so a baked-tile list can be
    // sorted + de-duplicated. Not a spatial ordering — just a total order.
    bool operator<(const TileCoord& o) const { return Z != o.Z ? Z < o.Z : X < o.X; }
};

struct TileCoordHash
{
    size_t operator()(const TileCoord& c) const
    {
        // Combine X and Z into a single hash (good for moderate grid sizes).
        auto h1 = std::hash<int32>{}(c.X);
        auto h2 = std::hash<int32>{}(c.Z);
        return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

// LOD state for async-streamed tiles.
enum class TileLodState : uint8
{
    Empty,   // Tile slot exists but has no heightfield data yet
    Coarse,  // Coarse (low-res) heightfield loaded, awaiting full detail
    Full     // Full-resolution heightfield + splatmap + normalmap loaded
};

// Per-tile data within a tiled terrain. Each tile has its own heightfield and splatmap.
struct TerrainTileData
{
    TerrainGrassField GrassField;
    TileCoord Coord;
    float32 WorldOriginX = 0.0f;    // world-space corner of this tile
    float32 WorldOriginZ = 0.0f;

    Terrain::TerrainConfig Config;   // per-tile config (same resolution for all tiles)
    Terrain::HeightfieldData Heightfield;

    std::vector<uint8> Splatmap;
    uint32 SplatmapWidth = 0;
    uint32 SplatmapHeight = 0;
    bool SplatmapDirty = false;

    // Splat writes the colour map has not composed over yet; see TerrainData.
    bool SplatColorDirtyFull = false;
    void MarkSplatChanged()
    {
        SplatmapDirty = true;
        SplatColorDirtyFull = true;
    }

    // This tile's colour map, on the tile's own lattice (TerrainColorMap.h).
    TerrainColorMap ColorMap;

    std::vector<uint8> Normalmap;
    uint32 NormalmapWidth = 0;
    uint32 NormalmapHeight = 0;
    // The HeightfieldVersion the Normalmap was generated for. The cook bakes the normal on the JOB
    // thread and integration sets this equal to HeightfieldVersion, so a freshly-streamed tile does
    // NOT re-generate the normal on the extraction thread (it only re-generates when a modifier bumps
    // HeightfieldVersion — heights actually changed). Guards the O(tileRes^2) extraction-thread stall.
    uint64 NormalmapVersion = 0;

    bool HeightfieldDirty = true;
    uint64 HeightfieldVersion = 0;

    // Per-version dirty-region journal for this tile's heightfield, consumed by
    // the extraction's normal regeneration. Mirrors TerrainData::HeightfieldDirtyLog:
    // the consumer keeps its own cursor and unions everything newer, so a region
    // bake that touches only this tile regenerates only that region's normals.
    // HeightfieldDirty (above) stays the whole-tile GPU-upload flag.
    DirtyRegionLog HeightfieldDirtyLog;

    // Cached per-tile height range for incremental global min/max tracking.
    // Updated by TileStreamingManager when heightfield data is integrated.
    float32 CachedMinH = 0.0f;
    float32 CachedMaxH = 0.0f;

    // Coarse per-block height min/max grid backing the incremental per-tile range.
    // A region edit rescans only the blocks its dirty rect overlaps (O(region)) and
    // the tile CachedMin/MaxH is the aggregate of every block (O(blocks)); a full
    // O(tile-samples) rescan is paid only when the grid is (re)built (first edit,
    // heightfield resize, streamed-in tile). This retires the ~4.6 ms/touched-tile
    // full rescan RefreshTileHeightRange ran per dab (the dominant sculpt-stroke
    // main-thread cost). Byte-identical to a full scan: blocks partition the tile
    // exactly, so unedited blocks keep their exact cached extremes and the aggregate
    // equals a full min/max. HeightBlockDim == 0 means "not built" (rebuild on next
    // range refresh). Mirrors the single-terrain PatchQuadtreeRegion incremental path.
    std::vector<float32> HeightBlockMinH;
    std::vector<float32> HeightBlockMaxH;
    uint32 HeightBlockDim = 0; // blocks per axis (0 = grid not built)

    // A Coarse tile's full-resolution heights and normal, built by the coarse streaming job.
    // The extraction consumes it on the tile's first coarse patch and releases it; a new
    // heightfield (SetTileHeightfield) releases it too.
    std::shared_ptr<const CoarseTileStandIn> CoarseStandIn;

    // Async streaming LOD state. Only Full tiles patch the unified GPU source.
    TileLodState LodState = TileLodState::Empty;

    // False until the modifier bake has applied the authored modifiers to this
    // tile. A tile arrives from streaming with the terrain's base only (the streaming
    // job never sees modifiers), so the modifier system bakes it ONCE, scoped to
    // this tile (BakeTiledStreamedTiles) — rather than re-baking (and version-
    // bumping, which regenerates the normals of) every resident tile whenever one tile
    // streams in. Reset to false whenever streaming (re)sets this tile's
    // heightfield content (SetTileHeightfield); set true by the modifier bake.
    bool ModifiersApplied = false;

    // False until the surface rules have been composited over this tile's COARSE splat.
    // A coarse tile is the distant, still-streaming LOD, and every full-detail splat path
    // gates on LodState == Full — so without a coarse pass the tile shows the unbaked base
    // (channel 0) and then POPS to the ruled surface the moment it upgrades. Separate from
    // ModifiersApplied because that flag gates the FULL bake, which must still run on
    // upgrade. Reset with ModifiersApplied whenever streaming (re)sets this tile's content.
    bool CoarseSplatBaked = false;

    // While a Full arrival's splat bake runs over several frames (the per-frame row budget,
    // TerrainModifierSystem::kArrivalSplatRowsPerFrame), the tile shows the splat it showed
    // while Coarse: SetTileHeightfield keeps it here (ArrivalSplatStandInDim square), and the
    // uploaders draw it instead of the partly baked Splatmap, and the colour map waits for the
    // bake's end. Empty when no arrival bake runs.
    std::vector<uint8> ArrivalSplatStandIn;
    uint32 ArrivalSplatStandInDim = 0;
    // The first Splatmap row the arrival bake has not composited yet.
    uint32 ArrivalSplatNextRow = 0;
    // The heightfield samples (max exclusive) a GPU-bake readback replaced that the splat does not
    // follow yet. TerrainModifierSystem re-splats them on its next update and empties it.
    DirtyRegionLog::Region ReadbackSplatRegion;
    // The arrival bake has composed this tile's heights, on its first bake frame; a tile waiting
    // for the row budget is not composed again.
    bool ArrivalHeightsComposed = false;

    // The authored modifiers are baked into this tile. Ends a pending arrival bake: the
    // stand-in goes, and the splat is marked changed so the uploaders send the baked one.
    void MarkModifiersApplied()
    {
        ModifiersApplied = true;
        ArrivalSplatNextRow = 0;
        ArrivalHeightsComposed = false;
        if (ArrivalSplatStandIn.empty())
            return;
        ArrivalSplatStandIn = {};
        ArrivalSplatStandInDim = 0;
        MarkSplatChanged();
    }

    void MarkFullDirty()
    {
        ++HeightfieldVersion;
        HeightfieldDirty = true;
        HeightfieldDirtyLog.Append(HeightfieldVersion,
                                   {0, 0,
                                    static_cast<int32>(Heightfield.GetWidth()),
                                    static_cast<int32>(Heightfield.GetHeight())});
    }

    // Mark a sub-region of this tile's heightfield dirty (E6 region bake). Max
    // bounds are exclusive. Bumps the version + logs the rect for the collider,
    // and sets the whole-tile GPU-upload flag (tile textures upload whole today).
    void MarkRegionDirty(int32 minX, int32 minZ, int32 maxX, int32 maxZ)
    {
        ++HeightfieldVersion;
        HeightfieldDirty = true;
        HeightfieldDirtyLog.Append(HeightfieldVersion, {minX, minZ, maxX, maxZ});
    }

    void ClearDirty() { HeightfieldDirty = false; }
};

// The ground every tile of a tiled terrain starts from before the modifier stack
// (bake stage A): the terrain's Terrain.BaseSource and, for HeightmapAsset, the
// decoded heightmap. The heightmap is shared with TerrainService's decode cache, so
// a streaming job that captured it keeps it alive across a hot-reload eviction.
// TerrainService::ResolveTiledTerrainBase builds one, and
// TerrainService::IsTiledTerrainBaseCurrent tells when the authored base has moved on.
// The terrain and height function a Function base's warnings name.
struct FunctionBaseNames
{
    std::string Terrain;
    std::string Function;
};

// What one row of a Function base became (FillFunctionBaseRow). Every result but Filled writes zeros.
enum class FunctionBaseRowFill : uint8
{
    Filled,
    Revoked,      // the function's module unloaded
    NoHeightBand, // the band is 0 or less, so meters have no normalized height
    NonFinite,    // the function returned NaN or infinity; warned once per terrain and function
};

struct TiledTerrainBase
{
    Components::TerrainBaseSource Source = Components::TerrainBaseSource::Flat;
    // Engaged for a Flat base under the scene's Generated noise effect: the stack FillTiledBaseRegion,
    // the generated pages and the GPU height bake all evaluate (GeneratedNoiseStackOf).
    std::optional<Terrain::TerrainNoiseStack> Noise;
    // Null for HeightmapAsset when the asset is missing or failed to decode: the
    // base is then flat, as it is on an untiled terrain.
    std::shared_ptr<const Terrain::HeightfieldData> Heightmap;
    // What Heightmap was resolved from (HeightmapAsset only; null and 0 otherwise):
    // the asset and its content version (TerrainService::GetHeightmapContentVersion),
    // so a hot reload is seen without resolving the heightmap again.
    GUID HeightmapGuid;
    uint64 HeightmapContentVersion = 0;
    // Function only: the built height function (null when no module registers the name: the base is
    // then flat), the name, registration and context it was built for, and the names its warnings carry.
    std::shared_ptr<const RevocableTerrainHeightFunction> HeightFunction;
    TerrainHeightFunctionKey HeightFunctionKey;
    FunctionBaseNames Names;
};

// Configuration for a tiled terrain.
struct TiledTerrainConfig
{
    float32 WorldSizeX = 1024.0f;
    float32 WorldSizeZ = 1024.0f;
    float32 HeightScale = 256.0f;
    float32 SamplesPerMeter = 1.0f;
    uint32 PatchGridSize = Terrain::kDefaultGridSize;
    float32 LODRangeScale = Terrain::kDefaultLODRangeScale;
    float32 StreamingRadius = 0.0f;   // 0 = auto

    // Fixed for the tiled terrain's lifetime: the extraction system re-provisions
    // the terrain when the authored base or its decoded heightmap changes, the same
    // way it does for a size or density change, so every tile (streamed before or
    // after the change, coarse or full) starts from one base.
    TiledTerrainBase Base;

    // Derived per-tile config (computed at creation time).
    Terrain::TerrainConfig TileConfig;
    float32 TileWorldSize = 0.0f;     // world-space size of each tile
    uint32 TilesPerAxisX = 0;
    uint32 TilesPerAxisZ = 0;
};

/// How the surface stream shows a changed surface rect: an edit at once, a refinement faded in.
enum class SurfaceRefreshReason : uint8
{
    Edit,
    Refinement,
};

// All data for a tiled terrain: the tile grid and the whole-terrain fields built over it.
struct TiledTerrainData
{
    // Analytic whole-terrain fallback includes regions on never-streamed tiles.
    TerrainGrassField GrassCoarseField;
    TerrainGrassField GrassUnifiedField;
    bool GrassRegionsActive = false;
    // The colour map of a terrain whose unified textures fit (TiledRenderExtent's unified lattice),
    // assembled from the tiles' maps; and the coarse map an atlas-backed terrain reads out of the
    // resident window. ColorMapActive: a texel of either, or of a tile's map, holds a weight.
    TerrainColorMap ColorUnifiedMap;
    TerrainColorMap ColorCoarseMap;
    bool ColorMapActive = false;
    TiledTerrainConfig Config;
    float32 WorldOriginX = 0.0f;     // world-space corner of tile (0,0)
    float32 WorldOriginZ = 0.0f;

    // Active (loaded) tiles, keyed by TileCoord.
    std::unordered_map<TileCoord, std::unique_ptr<TerrainTileData>, TileCoordHash> Tiles;

    // Monotonic counter for structural and tile-content changes. Systems that
    // need to notice tiled terrain changes can hash this instead of walking the
    // full tile map each frame.
    uint64 Revision = 0;

    // Whole-terrain GPU terrain handle: the TerrainData the render feature and the
    // height pages address the tiled terrain by. Created by the extraction system on its
    // first update of the terrain.
    uint32 GlobalGpuHandleIndex = 0;
    uint32 GlobalGpuHandleGeneration = 0;

    // Resident height range in normalized units (GetHeightRangeMeters reads it for level selection
    // and the default-rules placement). Seeded to the noise band [0, NominalHeightBandTop], or to the
    // height function's range for a Function base; the bakes re-derive it, widened to the same band.
    float32 CachedGlobalMinH = 0.0f;
    float32 CachedGlobalMaxH = kNominalHeightBandTop;

    // The first whole-terrain splat bake ran: every resident Full tile has a splat, and an edit
    // re-splats only the samples it touches.
    bool ResidentSplatsBaked = false;

    // ---- GPU height bake (slice-1b, GE_TERRAIN_GPU_BAKE) ----
    // Set by TerrainExtractionSystem each frame: true when the atlas is backing this terrain,
    // the GPU-bake flag is on, and the whole terrain fits the resident window (every tile always
    // resident — no eviction race). The modifier system reads last frame's value to decide
    // whether the height pass can run on the GPU.
    bool AtlasGpuBakeEligible = false;
    // Filled by TerrainModifierSystem when it routes the height pass to the GPU (skipping the CPU
    // eval); drained by TerrainExtractionSystem, which resolves each tile's atlas slot and hands
    // the dispatches to TerrainRenderFeature. Transient per bake.
    GpuHeightBakeBatch GpuBakeBatch;
    // The last height bake's path and refusals (TerrainModifierSystem); read by the debug readback
    // and the volume inspector.
    TerrainHeightBakeReport HeightBakeReport;

    // ---- Height page overlay (TerrainModifierSystem, BakeHeightPageOverlay) ----
    // What the modifiers add to this terrain's base height pages: the baked minus the base under
    // each height modifier's bounds, on the tile lattice (level 0 of the terrain's pages), held as
    // the pages those bounds reach. Null when no height modifier reaches the terrain.
    // PageOverlayVersion takes a new process-wide value on every change (zero until the first: two
    // terrains' data never share a non-zero version) and PageOverlayChanged is the level-0 rect that
    // change touched relative to version PageOverlayChangedSince; PageOverlayCurrent is set once the
    // overlay reflects the gathered modifier set, so the terrain pages only with its modifiers.
    // PageOverlayByteCap is set by the height page driver each frame: the platform's overlay cap
    // (HeightPageProfile::OverlayByteCap) when the terrain has CPU pages (TerrainHeightHasCpuPages), zero
    // when it has no page source: only a terrain with a cap holds an overlay.
    // PageOverlayRefusedBytes is what the overlay's pages would hold when they are over the cap, zero
    // otherwise: such a terrain keeps its texture and the driver logs OverlayRefusal.
    // PageOverlayOffGrid (set by the driver, PageOverlayAllowance) says the terrain's pages are not
    // on the tile lattice the overlay is baked on; PageOverlayRefusedOffGrid is set when a height
    // modifier reaches such a terrain: it keeps its texture and the driver logs OverlayGridRefusal.
    // The modifier system owns PageOverlay and writes it in place; the height pages hold it read-only.
    std::shared_ptr<PageStreaming::HeightPageLayer> PageOverlay;
    uint64 PageOverlayVersion = 0;
    PageStreaming::PageSampleRect PageOverlayChanged;
    uint64 PageOverlayChangedSince = 0;
    bool PageOverlayCurrent = false;
    uint64 PageOverlayByteCap = 0;
    uint64 PageOverlayRefusedBytes = 0;
    bool PageOverlayOffGrid = false;
    bool PageOverlayRefusedOffGrid = false;

    // Surface pages (TerrainModifierSystem): each bake or grass compose that changes the surface increments
    // SurfaceVersion and adds the level-0 sample rect and the planes it changed to the record: SurfaceChanged
    // and SurfaceChangedPlanes hold every change since SurfaceChangedSince. TerrainSurfacePageDriver takes
    // the record and sets SurfaceChangedSince to SurfaceVersion, so no change is lost between its frames.
    uint64 SurfaceVersion = 0;
    PageStreaming::PageSampleRect SurfaceChanged;
    uint8 SurfaceChangedPlanes = 0; // SurfacePlaneBit of each plane
    uint64 SurfaceChangedSince = 0;
    SurfaceRefreshReason SurfaceRefresh = SurfaceRefreshReason::Edit;
    // The surface stores as the surface page stream reads them: set by TerrainSurfacePageDriver each frame.
    TerrainSurfacePageSource SurfacePages;

    /// True when the terrain has height pages and its overlay is neither built nor refused: the
    /// modifier system bakes it at its next update.
    bool AwaitsPageOverlay() const
    {
        return PageOverlayByteCap != 0 && !PageOverlayCurrent && PageOverlayRefusedBytes == 0 &&
               !PageOverlayRefusedOffGrid;
    }
};

// The world position of the first sample of tile `tile` along one axis, for a terrain whose tile 0
// starts at `terrainOrigin`. Every tile and every surface page place their samples from it.
inline float32 TileWorldOrigin(float32 terrainOrigin, int32 tile, float32 tileWorldSize)
{
    return terrainOrigin + static_cast<float32>(tile) * tileWorldSize;
}

// Handle to a tiled terrain within TerrainService.
struct TiledTerrainHandle
{
    uint32 Index = 0;
    uint32 Generation = 0;

    bool operator==(const TiledTerrainHandle& o) const { return Index == o.Index && Generation == o.Generation; }
    bool operator!=(const TiledTerrainHandle& o) const { return !(*this == o); }
};

// Handle to a terrain data instance within TerrainService.
struct TerrainHandle
{
    uint32 Index = 0;
    uint32 Generation = 0;

    bool operator==(const TerrainHandle& other) const
    {
        return Index == other.Index && Generation == other.Generation;
    }

    bool operator!=(const TerrainHandle& other) const { return !(*this == other); }
};

class TerrainPages;
class PlanetSculptDab;
class TerrainRenderFeature;
class TerrainPageFeature;
class HeightPageStoreLoader;
struct HeightPageStoreLocation;

/// A terrain's height range, in meters above its base.
struct TerrainHeightRangeMeters
{
    float32 Min = 0.0f;
    float32 Max = 0.0f;
};

// Singleton service owning all terrain data. Follows the PhysicsWorldService /
// NavigationService pattern: static Initialize/Shutdown/Get.
class TerrainService
{
public:
    TerrainService();
    ~TerrainService();

    static void Initialize();
    static void Shutdown();
    static TerrainService& Get();
    static TerrainService* TryGet();
    static bool IsInitialized();
    static uint64 GetGeneration();

    // Create a new terrain and return its handle.
    TerrainHandle CreateTerrain(Terrain::TerrainConfig config);

    // Creates a single-tile terrain and fills its heights from `terrain`'s base source with the
    // modifier bake's base fill, so the first bake does not move the ground. Resolves a
    // heightmap base on the calling thread (main thread only, as ResolveHeightmapAsset).
    TerrainHandle CreateTerrainFromBase(const Terrain::TerrainConfig& config, const Components::Terrain& terrain);

    // Destroy a terrain by handle.
    void DestroyTerrain(TerrainHandle handle);

    // Access terrain data. Returns nullptr if handle is invalid/stale.
    TerrainData* GetTerrainData(TerrainHandle handle);
    const TerrainData* GetTerrainData(TerrainHandle handle) const;
    // Shares ownership of a terrain's data, so a job off the update can read it after
    // DestroyTerrain retires the slot. Null if the handle is invalid/stale. The data is
    // still written in place by TerrainModifierSystem's bakes: a reader that runs
    // concurrently must be ordered before the next one.
    std::shared_ptr<const TerrainData> ShareTerrainData(TerrainHandle handle) const;

    // Iterate all active terrains.
    uint32 GetActiveTerrainCount() const;
    uint32 GetActiveTiledTerrainCount() const;

    // Size of the slot tables, which only ever grows: a slot is returned to the
    // free list on destroy and reused, so a table larger than the high-water
    // mark of concurrently-live terrains means some teardown is not releasing.
    // That makes the per-open leak countable in one poll — the active counts
    // return to their steady value either way.
    uint32 GetTerrainSlotCount() const;
    uint32 GetTiledTerrainSlotCount() const;

    // Rebuild the CDLOD quadtree for a terrain (call after heightfield changes).
    void RebuildQuadtree(TerrainHandle handle);

    // Region-scoped analogue of RebuildQuadtree: re-scan only the quadtree's
    // finest nodes over the sample rect [minSampleX, maxSampleX] x
    // [minSampleZ, maxSampleZ] (inclusive) from the current heights, then
    // re-derive coarse levels — avoiding the O(all-samples) finest-level scan a
    // full rebuild pays every modifier edit. Also returns the resulting global
    // height range (the root node's min/max) in outMinH/outMaxH; the return
    // value is true when that range is exact (the finest level tiles the field),
    // false when the caller must scan the heightfield itself for the range.
    // Locked like RebuildQuadtree so a concurrent render read / DestroyTerrain
    // stays safe. Falls back to a full Build when the tree isn't built yet.
    bool PatchQuadtreeRegion(TerrainHandle handle,
                             int32 minSampleX, int32 minSampleZ,
                             int32 maxSampleX, int32 maxSampleZ,
                             float32& outMinH, float32& outMaxH);

    // ---- Tiled terrain API ----

    // Create a tiled terrain and return its handle. Computes per-tile config
    // from the tiled config, but does NOT create any tiles — tiles are loaded
    // on demand via LoadTile().
    TiledTerrainHandle CreateTiledTerrain(TiledTerrainConfig config);

    // Destroy a tiled terrain and all its loaded tiles.
    void DestroyTiledTerrain(TiledTerrainHandle handle);

    // Access tiled terrain data. Returns nullptr if handle is invalid/stale.
    TiledTerrainData* GetTiledTerrainData(TiledTerrainHandle handle);
    const TiledTerrainData* GetTiledTerrainData(TiledTerrainHandle handle) const;

    // Load a tile at the given coordinate. Creates its heightfield.
    // The tile is filled from the terrain's base (FillTiledBaseRegion).
    TerrainTileData* LoadTile(TiledTerrainHandle handle, TileCoord coord);

    // Unload a tile. Frees CPU data. Caller is responsible for GPU cleanup.
    void UnloadTile(TiledTerrainHandle handle, TileCoord coord);

    // Check if a tile is loaded.
    bool IsTileLoaded(TiledTerrainHandle handle, TileCoord coord) const;

    // ---- Per-page collider addressing (design page-streaming D3) ----
    //
    // A paged terrain gets one Jolt heightfield body per finest page inside a CPU window, and a
    // streamed planet one per level-0 page of its CPU windows, in the page's chart frame (design
    // page-streaming-planets-r4-colliders). A body's samples live in a slot here from the frame its
    // page is provisioned until its collider is torn down; the physics HeightFieldDataProvider
    // resolves a handle carrying kPagePhysicsHandleBit to the slot. TerrainPhysicsSystem owns the
    // page -> handle mapping (TerrainPageCollider, TerrainPlanetPageCollider).

    // dataHandle discriminator marking a page handle. Single-terrain slot indices are small, so the
    // bit never collides with them.
    static constexpr uint32 kPagePhysicsHandleBit = 0x20000000u;

    struct PagePhysicsHandle
    {
        uint32 Index = 0;      // slot index OR'd with kPagePhysicsHandleBit; 0 for no slot
        uint32 Generation = 0;
    };

    // One page body's height data and where its samples were read.
    struct PageColliderData
    {
        // kFinestPageGridSamples squared. A planar page: normalized (SurfaceHeightQuery::FillFinestPage),
        // with Physics::kHeightFieldNoCollision past the footprint's last sample. A planet page: meters
        // along its chart's up (FillPlanetPageBody).
        std::vector<float32> Samples;
        uint64 Version = 0;
        DirtyRegionLog DirtyLog;
        // A planar page: the terrain's CPU page record and overlay version when the samples were
        // read: a region update reads the page changes and the overlay edits since. Unused on a planet.
        SurfaceHeightPageStamp ReadAt;
        uint64 OverlayVersion = 0;
    };

    // Reads finest page (pageX, pageZ) of the paged terrain `query` reads into a new slot. No slot
    // (Index 0) when that page is not on the CPU at level 0 or owns no cell on the footprint (it
    // lies past the authored edge, or holds only the footprint's last sample, which its neighbor's
    // body covers).
    PagePhysicsHandle AcquirePagePhysicsHandle(const SurfaceHeightQuery& query, uint32 pageX, uint32 pageZ);

    // Brings page body `handle` (finest page (pageX, pageZ) of the terrain `query` reads) up to the
    // page and the modifier overlay as they are now. When the page changed in the CPU page record, or
    // an overlay edit reached its samples, since they were read, it reads them again and records the
    // samples that differ as one region of a new version: the physics region update
    // (PhysicsWorld::UpdateHeightFieldRegion) then rewrites only those, never the whole body for a
    // small edit. A page not on the CPU at level 0 keeps its samples and its read position, so a
    // later frame reads it. Main thread; a stale handle does nothing.
    void RefreshPagePhysicsHandle(const SurfaceHeightQuery& query, uint32 pageX, uint32 pageZ,
                                  PagePhysicsHandle handle);

    // Takes planet page body samples `meters` (FillPlanetPageBody) into a new slot, version 1, all
    // dirty. Main thread.
    PagePhysicsHandle AcquirePlanetPagePhysicsHandle(std::vector<float32>&& meters);

    // Replaces the samples of planet page body `handle` with `meters` as a new version, all dirty: the
    // physics rebuilds the body from them. Main thread; a stale handle does nothing.
    void RefillPlanetPagePhysicsHandle(PagePhysicsHandle handle, std::span<const float32> meters);

    // Release a page handle (the page left every window, or its collider was torn down). The
    // release is generation-checked, so a stale handle frees nothing. No-op for other handles.
    void ReleasePagePhysicsHandle(uint32 handleIndex, uint32 generation);

    // Release every page slot, at the world-transition seams that destroy the collider entities
    // without the teardown sweep (Play-exit).
    void ReleaseAllPagePhysicsHandles();

    // The page body data a handle names, or null when the slot is stale. handleIndex includes the
    // discriminator bit. Used by the physics provider.
    const PageColliderData* ResolvePageCollider(uint32 handleIndex, uint32 generation) const;

    // ---- Planet height page layer ----
    /// What a planet's layers add to its height pages, the version the pages compare, and each face's
    /// level-0 samples the versions after ChangedSince changed (ChangedSince zero: a layer is new).
    struct PlanetPageOverlay
    {
        std::shared_ptr<PageStreaming::HeightPageLayer> Layer;     ///< the sculpt: brush dabs add to it
        std::shared_ptr<PageStreaming::HeightPageLayer> Modifiers; ///< the modifier bake, under the sculpt
        uint64 Version = 0;
        PageStreaming::PageFaceRects Changed;
        uint64 ChangedSince = 0;
        GUID LayerAsset;              ///< the height layer asset the sculpt was loaded from or saved to
        GUID LayerLoadFailed;         ///< a height layer asset whose load failed: not read again
        std::string LayerRefusal;     ///< why the load of LayerLoadFailed failed
        uint64 SculptVersion = 0;     ///< the Version of the last sculpt change
        uint64 SavedSculptVersion = 0; ///< the SculptVersion the layer asset holds
    };
    /// Writes `deltas` (row-major over level-0 rect `rect` of face `face`) into the layer of the planet
    /// whose terrain entity id is `planet`, a six-face layer of the pyramid `levels`; an empty layer made
    /// for another pyramid is replaced. Writes nothing while PlanetSculptRefusal is not empty. The pages
    /// the write reaches are rewritten. Main thread, as the page requests read it.
    void WritePlanetPageLayer(uint32 planet, std::span<const PageStreaming::PageStoreLevel> levels, uint8 face,
                              const PageStreaming::PageSampleRect& rect, std::span<const float32> deltas);
    /// Removes the sculpt layer of planet `planet`: the pages it reached are rewritten without it.
    void ClearPlanetPageLayer(uint32 planet);
    /// Gives `write` the modifier layer of planet `planet` (`faceSamples` level-0 samples a side), and
    /// `write` appends the face rects it changes. `fresh`, or a layer of another pyramid, starts from an
    /// empty layer. The pages the rects reach are rewritten. Main thread.
    void WritePlanetModifierLayer(
        uint32 planet, uint32 faceSamples, bool fresh,
        FunctionRef<void(PageStreaming::HeightPageLayer&, std::vector<PageStreaming::PageFaceRect>&)> write);
    /// The layer of planet `planet`, or null when it has none.
    const PlanetPageOverlay* FindPlanetPageOverlay(uint32 planet) const;
    /// A count that moves whenever any planet's layer changes. Main thread.
    uint64 PlanetLayerVersion() const { return m_PlanetPageOverlayVersion; }
    /// Adds `dab` to the layer of planet `planet`, whose height pages have `faceSamples` level-0 samples
    /// a side (PlanetFaceSamples); an empty layer made for another pyramid is replaced. Adds nothing
    /// while PlanetSculptRefusal is not empty. The pages the dab reaches are rewritten. Main thread.
    void ApplyPlanetSculptDab(uint32 planet, uint32 faceSamples, const PlanetSculptDab& dab);
    /// Starts to keep the layer pages a sculpt stroke on planet `planet` changes, for its undo; makes
    /// the layer for `faceSamples` (as ApplyPlanetSculptDab) when the planet has none. Does nothing
    /// while PlanetSculptRefusal is not empty.
    void BeginPlanetSculptStroke(uint32 planet, uint32 faceSamples);
    /// Why the brush may not change the sculpt of planet `planet`, whose height pages have `faceSamples`
    /// level-0 samples a side at a level-0 texel of `level0Texel` meters: its height layer asset did not
    /// load, or the layer it holds has pages made for another pyramid. The text states the fix, with the
    /// Planet Radius range the sculpt fits. Empty when the brush may change it.
    std::string PlanetSculptRefusal(uint32 planet, uint32 faceSamples, float32 level0Texel) const;
    /// The layer pages the stroke changed, as they were before it; ends the stroke. Empty when it
    /// changed nothing.
    std::vector<PageStreaming::HeightPageLayerPage> TakePlanetSculptStroke(uint32 planet);
    /// The layer pages of planet `planet` that `keys` name, as they are now.
    std::vector<PageStreaming::HeightPageLayerPage> CopyPlanetSculptPages(
        uint32 planet, std::span<const PageStreaming::HeightPageLayerPage> keys) const;
    /// Puts `pages` back into the layer of planet `planet` byte for byte; the pages they reach are
    /// rewritten.
    void RestorePlanetSculptPages(uint32 planet, std::span<const PageStreaming::HeightPageLayerPage> pages);
    /// True when the sculpt of planet `planet` changed after its height layer asset was loaded or saved.
    bool PlanetHeightLayerNeedsSave(uint32 planet) const;
    /// True when PlanetHeightLayerNeedsSave holds for any planet.
    bool AnyPlanetHeightLayerNeedsSave() const;
    /// Records that the height layer asset `asset` (null: no asset, the sculpt is empty) holds the
    /// sculpt of planet `planet` as of SculptVersion `sculptVersion`.
    void MarkPlanetHeightLayerSaved(uint32 planet, const GUID& asset, uint64 sculptVersion);
    /// Loads the height layer asset `asset` as the sculpt of planet `planet` (`faceSamples` level-0 samples
    /// a side), unless it came from `asset`. A refused file, or one without six faces, is not read again; a
    /// layer of another pyramid is held. Both warn and set PlanetSculptRefusal. Main thread.
    void EnsurePlanetHeightLayerLoaded(uint32 planet, uint32 faceSamples, const GUID& asset);

    // ---- Planet edits: the CBT's dirty regions ----
    static constexpr uint32 kPlanetSculptFaceCount = 6u; // the six cube faces

    struct PlanetSculptFaceRect
    {
        bool Touched = false;
        float32 MinU = 0.0f, MinV = 0.0f, MaxU = 0.0f, MaxV = 0.0f;
    };

    // Drop every planet's sculpt and modifier layers and the CBT's dirty regions. The CBT update
    // system calls this when the active spherical terrain is deleted or leaves the sphere domain,
    // so a new planet does not take the old planet's layers.
    void ResetPlanetSculpt();

    // Drain (clear-on-read) the per-face union of the planet's height page changes
    // (AccumulatePlanetPageDirtyRegions) since the last drain into `out`, as (face, UV rect)
    // regions. The union keeps every change between two CBT updates, not only the last one.
    // Returns false (out unchanged) when nothing is dirty.
    bool ConsumePlanetLayerDirtyRegions(CBTTerrain::SphereEditRegions& out);

    // ---- Async tile API (used by TileStreamingManager) ----

    // Create an empty tile slot at the given coordinate (no heightfield data).
    TerrainTileData* CreateEmptyTile(TiledTerrainHandle handle, TileCoord coord);

    // Set the tile's heightfield data (coarse or full). Moves the heightfield
    // into the tile and updates CachedMinH/MaxH and LodState.
    void SetTileHeightfield(TiledTerrainHandle handle, TileCoord coord,
                            Terrain::HeightfieldData&& heightfield,
                            float32 minH, float32 maxH);

    // Set the tile's splatmap and normalmap data. Moves the data into the tile.
    void SetTileMaps(TiledTerrainHandle handle, TileCoord coord,
                     std::vector<uint8>&& splatmap, uint32 splatW, uint32 splatH,
                     std::vector<uint8>&& normalmap, uint32 normW, uint32 normH);

    // ---- Base-source heightmap assets + asset-reload lane ----

    // Decoded base-source heightmap cache, keyed by asset GUID. Decodes once
    // via the AssetManager (16-bit PNG through stb's 16-bit path; .r16/.r32
    // raw on the grid its import settings declare, RawHeightmap.h) and caches until a
    // hot-reload eviction. Returns null when the GUID can't be resolved or
    // decoded (negative-cached until eviction, the reason in
    // GetHeightmapDecodeError). Main-thread only — the
    // modifier and extraction systems that consume it run sequentially. The
    // result is shared: an eviction drops the cache's reference, and a holder
    // (a tiled terrain's base, a streaming job) keeps reading the content it
    // was given until it lets go.
    std::shared_ptr<const Terrain::HeightfieldData> ResolveHeightmapAsset(const GUID& guid);

    // Why the heightmap `guid` last failed to decode, worded as the fix ("set the heightmap's
    // width and height in its import settings"); empty when it decoded or has not been resolved.
    std::string GetHeightmapDecodeError(const GUID& guid) const;

    // ---- Height pages (TerrainPages.h; design page-streaming D-c) ----

    // The process's one height-page residency, created on first use with the engine's job pool.
    TerrainPages& GetTerrainPages();
    // The built height functions of Function terrains (TerrainBaseSource::Function), created on first
    // use with the engine's job pool, or building inline when the engine is not initialized.
    TerrainHeightFunctionInstances& GetHeightFunctionInstances();
    /// Main thread, before the images of the loads [firstGeneration, endGeneration) of `moduleId` unmap.
    /// Revokes the height functions those loads registered and every instance built from them.
    static void RevokeHeightFunctions(std::string_view moduleId, uint64 firstGeneration, uint64 endGeneration);
    /// The height pages, or null before anything asked for them (GetTerrainPages makes them).
    const TerrainPages* FindTerrainPages() const;
    /// The CPU height of the planar terrain `terrain` describes (its component, placed by `transform`),
    /// valid until the next height-page Update: its pages when it is tiled (unsampled while they have no
    /// source), its heightfield when it is one map; an invalid query when it has no height data yet.
    SurfaceHeightQuery SurfaceQuery(const Components::Terrain& terrain, const Components::WorldTransform& transform);
    /// The height range of the planar `terrain` in meters above its base, the unit of every height rule: [0, HeightScale]
    /// widened to its resident heights. Before its tiles exist, a Flat terrain under `generatedNoise`
    /// (ResolveGeneratedNoiseRelief) reports the noise stack's RealizedTop.
    TerrainHeightRangeMeters GetHeightRangeMeters(const Components::Terrain& terrain,
                                                  std::optional<GeneratedNoiseRelief> generatedNoise) const;
    /// Fetches the finest CPU height page under `worldPoint` of the paged terrain `terrain` describes
    /// now, on the main thread (TerrainPages::FetchFinestPage): a pick reads the finest height
    /// where it lands, wherever the cameras are. Returns whether that page is then on the CPU.
    bool FetchFinestHeightPage(const Components::Terrain& terrain, const Mathematics::Vector3& worldPoint);
    /// The level-0 pages of the CPU windows of the paged terrain `terrain` describes
    /// (TerrainPages::CpuWindowPages): the pages its colliders stand on. Empty when it is not
    /// paged.
    std::span<const PageStreaming::PageAddress> FinestWindowPages(const Components::Terrain& terrain);
    // The cooked height stores of the heightmaps paged terrains read.
    HeightPageStoreLoader& GetHeightPageStores();
    // Where height stores are found or cooked; set at startup by the engine.
    void SetHeightPageStoreLocation(HeightPageStoreLocation location);
    // Set by the modifier system at each gather: whether any modifier is authored. Until the first
    // gather the set is unknown and HeightModifiersMayApply answers true.
    void SetHeightModifiersPresent(bool present)
    {
        m_HeightModifiersGathered = true;
        m_HeightModifiersPresent = present;
    }
    bool HeightModifiersMayApply() const { return !m_HeightModifiersGathered || m_HeightModifiersPresent; }

    // Content version for a heightmap GUID: bumped on each eviction so
    // terrain-state hashes treat reloaded content as a change. 0 until the
    // first eviction; never requires (or triggers) a decode.
    uint64 GetHeightmapContentVersion(const GUID& guid) const;

    // Drop the decoded entry and bump its content version. Returns true when
    // the GUID was referenced (entry or version existed) — i.e. a re-bake is
    // warranted.
    bool EvictDecodedHeightmap(const GUID& guid);

    // Test seam: unit tests run without an AssetManager and inject decoded
    // heightfields directly into the same cache the decode path fills.
    void SeedDecodedHeightmapForTests(const GUID& guid, Terrain::HeightfieldData heightfield);

    // The base a tiled terrain's tiles start from, for a terrain authored with `source`
    // and the heightmap asset `heightmapGuid` (read for HeightmapAsset only). The
    // heightmap comes from the decode cache above. Main-thread only, like the cache.
    TiledTerrainBase ResolveTiledTerrainBase(Components::TerrainBaseSource source,
                                             const GUID& heightmapGuid);

    // True while `base` is what ResolveTiledTerrainBase would return for `source` and
    // `heightmapGuid` now: the same source and, for HeightmapAsset, the same asset at
    // the same content version. Lookups only, never a decode, so the extraction system
    // asks every frame and resolves only on a change.
    //
    // The content version moves on every EvictDecodedHeightmap of that GUID, and the
    // asset-reload lane evicts on each ContentEvents event for it, not only a hot reload:
    // Modified and Created from the file watcher, Reloaded, Unloaded and Destroyed. Each
    // of those makes the base stale, and a stale base costs a full re-provision of every
    // tiled terrain on that heightmap (the whole tile set re-streamed and re-baked).
    bool IsTiledTerrainBaseCurrent(const TiledTerrainBase& base, Components::TerrainBaseSource source,
                                   const GUID& heightmapGuid) const;

    // The config for the tiled terrain of `terrain`: size, height scale, samples per meter, streaming radius,
    // patch grid, LOD range and base. A Flat base takes `generatedNoise` as its relief. `terrainName` labels
    // the base's warnings. Main thread only.
    TiledTerrainConfig BuildTiledTerrainConfig(const Components::Terrain& terrain, std::string_view terrainName,
                                               std::optional<GeneratedNoiseRelief> generatedNoise);

    // False while the height function of the Function terrain `terrain` builds for its seed and
    // footprint: the extraction system does not provision it until then. True for every other base.
    // The first ask starts the build. Main-thread only.
    bool IsTiledTerrainBaseReady(const Components::Terrain& terrain);

    /// The names of the height functions the loaded modules register (GE_REGISTER_TERRAIN_HEIGHT_FUNCTION),
    /// sorted. A Function terrain's HeightFunction field takes one of them. Main thread only.
    static std::vector<std::string> GetHeightFunctionNames();
    /// True when a loaded module registers the height function `name`. A Function terrain whose name is
    /// not registered is flat. Main thread only.
    static bool IsHeightFunctionRegistered(std::string_view name);

    // How the live tiled terrain's `config` differs from the component `terrain` now: its density, its size and its base
    // (IsTiledTerrainBaseCurrent). Lookups only; the extraction system passes the answer to StepTiledReprovision each frame.
    // A Function terrain reads as unchanged until the function for its new seed, name and size is built
    // (IsTiledTerrainBaseReady), so the old ground stays drawn; a Height Scale edit changes its base.
    // A Generated noise that appears, goes, or changes preset or Weight over a Flat base changes its base.
    TiledTerrainEdit CompareTiledTerrainConfig(const TiledTerrainConfig& config,
                                               const Components::Terrain& terrain,
                                               std::optional<GeneratedNoiseRelief> generatedNoise);

    // Asset-reload lane (design §9.1/§9.2). The invalidator handlers only
    // ENQUEUE here — they can fire on the file-watcher thread.
    // TerrainModifierSystem drains on the main thread at Update start and
    // evicts the decoded caches there.
    void EnqueueAssetInvalidation(const GUID& guid);
    std::vector<GUID> TakePendingAssetInvalidations();

    // ---- Zone payload store (edit-pipeline design §9.3) ----
    //
    // Brush-authored sculpt/paint payloads keyed by the zone component's
    // payload GUID. The bake resolves payloads here; the brush writes here.
    // Main-thread only, like the heightmap cache — the modifier/extraction/tool
    // paths that touch it run sequentially.

    // Fetch an already-resident payload (no asset load). Null when absent.
    TerrainZonePayload* GetZonePayload(const GUID& guid);

    // Any resident payload with unsaved brush edits. Brush strokes never touch
    // ECS state, so the editor's dirty prompts must ask here — the scene
    // document's own flag misses stroke-only sessions.
    bool AnyZonePayloadNeedsSave() const;

    // Drop every resident payload (scene replace / new scene). Payloads are
    // per-scene authored data: a replace-open must not leak the previous
    // session's edits — a discarded stroke left resident would keep the
    // document dirty forever and would be silently persisted by the next
    // explicit Save. The new scene's zones re-resolve lazily from their
    // .tzone files. Bumps the edit epoch so the modifier gate re-bakes.
    void ClearZonePayloads();

    // Resolve a payload: return the resident one, or decode it from its .tzone
    // asset on a miss. Null when the GUID can't be resolved/decoded.
    const TerrainZonePayload* ResolveZonePayload(const GUID& guid);

    // Create (or reset) a resident payload for a new zone. Bumps the edit epoch
    // so the modifier gate wakes and bakes the fresh zone.
    TerrainZonePayload& EnsureZonePayload(const GUID& guid, ZonePayloadFormat format,
                                          uint32 width, uint32 height);

    // Record a brush write: bump DataVersion, accumulate the dirty texel rect,
    // bump the global edit epoch (a payload write changes no ECS state, so the
    // modifier gate has nothing else to wake on), and flag NeedsSave. Max
    // bounds are exclusive. No-op when the GUID isn't resident.
    void NotifyZonePayloadEdited(const GUID& guid, int32 minX, int32 minZ, int32 maxX, int32 maxZ);

    // Clear a payload's accumulated dirty rect after the bake consumed it.
    void ClearZonePayloadDirty(const GUID& guid);

    // Drop a resident payload. Returns true when one existed (a re-bake is
    // warranted); bumps the edit epoch so the gate re-reads.
    bool EvictZonePayload(const GUID& guid);

    // Monotonic epoch bumped on any zone-payload edit/create/evict/restore.
    // TerrainModifierSystem's change gate compares it to detect brush strokes.
    uint64 GetZonePayloadEditEpoch() const { return m_ZonePayloadEditEpoch; }

    // Replace a resident payload wholesale (undo/redo restore path). Bumps
    // DataVersion + the edit epoch but records NO dirty rect, so the next bake
    // re-applies the full zone footprint (a stroke undo reverts the whole zone).
    void SetZonePayload(const GUID& guid, TerrainZonePayload payload);

    // Test seam: inject a resident payload directly (no AssetManager).
    void SeedZonePayloadForTests(const GUID& guid, TerrainZonePayload payload);

    // Play-mode snapshot/restore (design §7/§10): zone payloads mutated during
    // Play restore on Play-exit by default. Snapshot deep-copies the whole
    // store on Play-enter; restore swaps it back and bumps the edit epoch so
    // the next bake reflects the reverted payloads.
    void SnapshotZonePayloadsForPlay();
    void RestoreZonePayloadsFromPlaySnapshot();

    // ---- Thread safety guards ----
    // The modifier and extraction systems both mutate terrain data (heightfields,
    // quadtrees, splatmaps) and MUST NOT run concurrently. Today they run
    // sequentially via system dependencies (Modifiers → Extraction). These guards
    // assert that invariant at runtime so a future parallelization mistake is
    // caught immediately.
    //
    // Future async support: to allow concurrent modifier + extraction, adopt a
    // staging pattern — the modifier writes to a staging heightfield, and
    // extraction atomically swaps the staging data into the live slot. This
    // eliminates shared mutable state entirely. See the design doc's caching
    // strategy (Section 3) for the conceptual foundation.
    void BeginModifierAccess()  { assert(!m_ModifierAccessActive.exchange(true)); }
    void EndModifierAccess()    { m_ModifierAccessActive.store(false); }
    void AssertNoModifierAccess() const { assert(!m_ModifierAccessActive.load()); }

    // ---- Interactive modifier-drag signal ----
    // Set by the editor while an interactive edit is in progress.
    // TerrainModifierSystem coalesces the expensive per-frame full-footprint
    // re-bake while this is set, deferring to one accurate settle bake the moment
    // the edit ends — a gizmo drag on a large atlas terrain re-baked old∪new every
    // frame otherwise (single-digit fps). Defaults off, so headless runs, gameplay,
    // and tests keep the immediate per-frame bake.
    //
    // The signal is the OR of independent SOURCES, not one bool. The scene view
    // writes its source EVERY frame from the gizmo's live state, so a second
    // writer sharing one bool would have its arming cleared on the very next
    // frame — an inspector drag would arm the throttle and lose it before the
    // modifier system ever coalesced anything.
    enum class InteractiveEditSource : uint32
    {
        TransformGizmo = 0, // a modifier/zone gizmo being moved, rotated or scaled
        InspectorDrag = 1,  // an inspector control being dragged (surface-rule bands)
        Count
    };

    void SetInteractiveModifierEdit(InteractiveEditSource source, bool active)
    {
        const uint32 bit = 1u << static_cast<uint32>(source);
        m_InteractiveModifierEditSources =
            active ? (m_InteractiveModifierEditSources | bit)
                   : (m_InteractiveModifierEditSources & ~bit);
    }
    bool IsInteractiveModifierEdit() const { return m_InteractiveModifierEditSources != 0; }

    // ---- GPU height-bake settle-readback -> splat handoff (slice-1c) ----
    // The extraction system's settle-readback adopt refreshed tile heights from the GPU and
    // recorded each tile's ReadbackSplatRegion; this one-shot signal tells the modifier system's
    // next Update to re-splat those regions.
    void MarkGpuReadbackSplatPending() { m_GpuReadbackSplatPending = true; }
    bool TakeGpuReadbackSplatPending()
    {
        const bool v = m_GpuReadbackSplatPending;
        m_GpuReadbackSplatPending = false;
        return v;
    }

private:
    // UpdateHeightPages adds the planets' height page changes to the render dirty union.
    friend void UpdateHeightPages(TerrainService& service, TerrainRenderFeature& terrainFeature,
                                  TerrainPageFeature& pages, uint64 frameIndex, float32 deltaSeconds,
                                  uint32 framesInFlight);

    /// Adds the planet's height page changes (TerrainPages::PlanetDirtyRegions) to the render
    /// per-face dirty union ConsumePlanetLayerDirtyRegions drains, so the CBT re-evaluates the
    /// bisectors over them.
    void AccumulatePlanetPageDirtyRegions(const CBTTerrain::SphereEditRegions& regions);

    // Subscribe the texture + heightmap reload invalidators. No-op until the
    // engine (and its AssetManager) is initialized — unit tests construct the
    // service standalone and the lane stays inactive there.
    void InstallAssetReloadInvalidators();

    std::atomic<bool> m_ModifierAccessActive{false};

    // One bit per InteractiveEditSource, held for the duration of that source's
    // gesture (see SetInteractiveModifierEdit). Single-threaded main-thread state
    // — the editor writes it and the modifier system reads it in the same frame's
    // system pass.
    uint32 m_InteractiveModifierEditSources = 0;
    bool m_GpuReadbackSplatPending = false;

    struct DecodedHeightmapEntry
    {
        // Null when the decode failed: a negative cache entry, so a failing
        // source is not re-decoded every bake (until its eviction).
        std::shared_ptr<const Terrain::HeightfieldData> Heightfield;
        uint64 ContentVersion = 0;
        std::string DecodeError; // why Heightfield is null; empty when it decoded
    };
    std::unordered_map<GUID, DecodedHeightmapEntry> m_HeightmapCache;
    // Content versions survive eviction (see GetHeightmapContentVersion).
    std::unordered_map<GUID, uint64> m_HeightmapVersions;

    std::mutex m_PendingAssetInvalidationsMutex;
    std::vector<GUID> m_PendingAssetInvalidations;
    AssetReloadInvalidator m_TextureReloadInvalidator;
    AssetReloadInvalidator m_HeightmapReloadInvalidator;
    AssetReloadInvalidator m_ZonePayloadReloadInvalidator;

    // Zone payload store (see the public zone API above).
    std::unordered_map<GUID, TerrainZonePayload> m_ZonePayloads;
    uint64 m_ZonePayloadEditEpoch = 0;
    std::unordered_map<GUID, TerrainZonePayload> m_ZonePayloadPlaySnapshot;
    bool m_HasZonePayloadPlaySnapshot = false;

    struct Slot
    {
        std::shared_ptr<TerrainData> Data;
        uint32 Generation = 0;
        bool Active = false;
    };

    std::vector<Slot> m_Slots;
    std::vector<uint32> m_FreeList;

    // Tiled terrain slots (separate pool from single-terrain slots).
    struct TiledSlot
    {
        std::unique_ptr<TiledTerrainData> Data;
        uint32 Generation = 0;
        bool Active = false;
    };
    std::vector<TiledSlot> m_TiledSlots;
    std::vector<uint32> m_TiledFreeList;

    GenerationalVector::GenerationalVector<PageColliderData> m_PagePhysicsSlots;
    // The page a refresh reads before it compares (RefreshPagePhysicsHandle), kept across calls.
    std::vector<float32> m_PageColliderScratch;

    // The layer of planet `planet` for the pyramid `levels`; a layer made for another pyramid is
    // replaced. `derived` is false when the layer is new.
    PlanetPageOverlay& PlanetLayerFor(uint32 planet, std::span<const PageStreaming::PageStoreLevel> levels,
                                      bool& derived);
    // A new version of `overlay` that changed `rects`, as the pages of planet `planet` read it.
    void RecordPlanetLayerChange(uint32 planet, PlanetPageOverlay& overlay, bool derived,
                                 std::span<const PageStreaming::PageFaceRect> rects);
    // RecordPlanetLayerChange for a change of the sculpt layer: the sculpt then needs a save.
    void RecordPlanetSculptChange(uint32 planet, PlanetPageOverlay& overlay, bool derived,
                                  std::span<const PageStreaming::PageFaceRect> rects);
    FlatMap<uint32, PlanetPageOverlay> m_PlanetPageOverlays; // by terrain entity id
    std::vector<PageStreaming::PageFaceRect> m_PlanetLayerRects; // reused by the planet layer writes
    uint64 m_PlanetPageOverlayVersion = 0;                   // the last version given, from 1

    // The per-face union ConsumePlanetLayerDirtyRegions drains, and the mutex that guards it.
    std::array<PlanetSculptFaceRect, kPlanetSculptFaceCount> m_RenderSculptFaces{};
    mutable std::mutex m_RenderSculptFacesMutex;

    std::unique_ptr<TerrainPages> m_TerrainPages;
    std::unique_ptr<TerrainHeightFunctionInstances> m_HeightFunctions;
    std::unique_ptr<HeightPageStoreLoader> m_HeightPageStores;
    bool m_HeightModifiersGathered = false;
    bool m_HeightModifiersPresent = false;

    mutable std::mutex m_Mutex;

    static std::unique_ptr<TerrainService> s_Instance;
    static uint64 s_Generation;
};

// Real render geometry for a tiled terrain's unified GPU source. WorldSizeX/Z is the
// REAL per-axis extent (tilesPerAxis * tileWorldSize) the tile grid covers — NOT a
// power-of-2 square. CBT maps
// world = origin + uv*size and samples the unified heightmap at that uv 1:1, so a
// squared/pow2 size would stretch a non-square or non-pow2-tile-count terrain
// (a 3-tile axis would render 4/3 too wide with a phantom clamped-edge band). Unified
// resolution = tilesPerAxis * (tileRes-1) + 1 — the grid an untiled terrain of equal
// content uses. Pure function of the config so it is directly unit-testable.
struct TiledRenderExtent
{
    float32 WorldSizeX = 0.0f;
    float32 WorldSizeZ = 0.0f;
    uint32 TilesPerAxisX = 0;
    uint32 TilesPerAxisZ = 0;
    uint32 TileInteriorX = 0;  // tileRes - 1 (shared-edge spacing)
    uint32 TileInteriorZ = 0;
    uint32 UnifiedWidth = 0;
    uint32 UnifiedHeight = 0;
};
TiledRenderExtent ComputeTiledRenderExtent(const TiledTerrainConfig& config);
/// True when the unified lattice is wider than one texture carries (kMaxUnifiedTiledResolution on an
/// axis): the terrain's heights then come from the resident-window atlas.
bool TiledRenderExtentUsesAtlas(const TiledRenderExtent& extent);
/// True when Terrain::BehindCameraDetail acts: a planar terrain whose heights stream through the tile
/// atlas. The inspector shows the setting and the CBT update applies it only on such a terrain.
bool BehindCameraDetailApplies(const Components::Terrain& terrain, const TerrainService& service);

// Fill heightfield samples in [minX, maxX] x [minZ, maxZ] (inclusive,
// clamped) from a terrain's base source (bake stage A, design §3.1):
// a decoded heightmap (bilinear over global UV when resolutions differ), or
// flat zero. Deterministic per-sample, so a
// region fill is an exact subset of a full fill — region re-bakes rely on
// this. `heightmap` is the decoded source for HeightmapAsset (from
// TerrainService::ResolveHeightmapAsset); null/empty falls back to flat zero
// so a missing asset is visually obvious rather than silently noise.
void FillHeightfieldBaseRegion(Terrain::HeightfieldData& heightfield,
                               Components::TerrainBaseSource source,
                               const Terrain::HeightfieldData* heightmap,
                               int32 minX, int32 minZ, int32 maxX, int32 maxZ);

// Widens a height data range [minH, maxH] (normalized heights) to the nominal band [0, bandTop]
// joined with the data, for whole-map and paged terrains. An edit inside the band does not
// move the range.
void WidenToNominalHeightBand(float32& minH, float32& maxH, float32 bandTop);

// The stack a Generated noise `relief` gives `terrain`: the preset's gain at the terrain's longer side's
// size, with the relief's Weight as the stack's Amplitude.
Terrain::TerrainNoiseStack GeneratedNoiseStackOf(const Components::Terrain& terrain, const GeneratedNoiseRelief& relief);

// The top of a tiled terrain's nominal band (normalized heights): the noise stack's RealizedTop for a
// Generated noise base, kNominalHeightBandTop for the other bases.
float32 NominalHeightBandTop(const TiledTerrainConfig& config);

/// Fills samples [minX, maxX] x [minZ, maxZ] (inclusive, clamped) of `field`, a grid over the world rect
/// (fieldOriginX, fieldOriginZ) + (fieldSizeX, fieldSizeZ), from `config.Base` of the terrain whose corner is
/// (terrainOriginX, terrainOriginZ). A value depends only on its world position, so tiles agree on their edges.
/// @param minResolvedCellMeters In meters. A noise octave whose cell is shorter is replaced by its
///        mean (Terrain::SampleWorldSpaceNoise); a Function base takes it as its minResolvedWavelength.
///        0 for a tile; twice the sample spacing for a coarse field. A heightmap base ignores it.
void FillTiledBaseRegion(Terrain::HeightfieldData& field, const TiledTerrainConfig& config,
                         float32 terrainOriginX, float32 terrainOriginZ,
                         float32 fieldOriginX, float32 fieldOriginZ,
                         float32 fieldSizeX, float32 fieldSizeZ,
                         int32 minX, int32 minZ, int32 maxX, int32 maxZ, float32 minResolvedCellMeters);

// Fills `field` from `tiled`'s base over the tile grid's extent (ComputeTiledRenderExtent), the rect
// BuildCoarseHeightField maps its texels over. A noise or function base drops relief finer than
// two texels.
void FillAtlasCoarseBaseField(Terrain::HeightfieldData& field, const TiledTerrainData& tiled);

// The height band in meters a Function base's meters are divided by. Every reader takes it from here.
// The band is the terrain's Height Scale.
float32 FunctionBaseHeightBand(const TiledTerrainConfig& config);

// One row of a Function base: the heights of `function` at terrain-local (localX[i], localZ) in meters,
// divided by `heightBand` (meters) and not clamped, into outNormalized[i]. Every base reader fills
// through it, so their samples agree bit for bit. `names` label the non-finite warning.
FunctionBaseRowFill FillFunctionBaseRow(const RevocableTerrainHeightFunction& function, const FunctionBaseNames& names,
                                        float32 localZ, std::span<const float32> localX,
                                        float32 minResolvedWavelength, float32 heightBand,
                                        std::span<float32> outNormalized);

// Reset a splatmap to the unbaked state: sized to the heightfield, RGBA8, all zero.
// Surface rules composite the material placement on top (ApplySplatModifiers).
//
// ZERO IS THE ONLY CORRECT BASE, for two independent reasons. CompositeSplatTexel
// with Replace off adds a row's weight then renormalizes the texel (with Replace on it
// reads an all-zero texel as the first material at 255), so a channel-0-full base would
// pin channel 0 at 1.0 in the accumulator and cap any row targeting another channel
// at 50%. And an all-zero texel is not "no data" — the surface resolves it to
// channel 0 (cbt_surface.glsl's wSum guard, normalizeLayerWeights in the grass
// shaders), so "unbaked reads as the first material" is the shipped convention and
// a texel no rule claims already renders correctly.
void ResetSplatmap(const Terrain::HeightfieldData& heightfield,
                   std::vector<uint8>& outSplatmap,
                   uint32& outWidth, uint32& outHeight);

// Region-scoped variant: zero only the texels in [minX, maxX] x [minZ, maxZ]
// (inclusive, clamped), leaving the rest of the splatmap untouched. The splatmap
// must already be sized to the heightfield.
void ResetSplatmapRegion(const Terrain::HeightfieldData& heightfield,
                         std::vector<uint8>& splatmap,
                         int32 minX, int32 minZ, int32 maxX, int32 maxZ);

// Generate an R16G16_FLOAT normal map from a heightfield using central differences.
// Stores XZ normal components as float16; Y reconstructed in shader.
// Optional neighbor heightfields provide correct cross-tile edge normals.
void GenerateNormalmapFromHeightfield(
    const Terrain::HeightfieldData& heightfield,
    float32 worldSizeX, float32 worldSizeZ,
    float32 heightScale,
    std::vector<uint8>& outNormalmap,
    uint32& outWidth, uint32& outHeight,
    const Terrain::HeightfieldData* neighborLeft = nullptr,
    const Terrain::HeightfieldData* neighborRight = nullptr,
    const Terrain::HeightfieldData* neighborUp = nullptr,
    const Terrain::HeightfieldData* neighborDown = nullptr);

// Region-scoped variant: recompute only normal texels in
// [minX, maxX] x [minZ, maxZ] (inclusive, clamped), leaving the rest of the
// normalmap untouched. The normalmap must already be sized to the heightfield.
// Callers must pad the region by the central-difference radius (1 sample)
// beyond the changed heightfield samples. Optional neighbor heightfields give
// bit-identical cross-tile edge normals to the full generator, so a region
// edit that touches a tile boundary keeps the two slots' shared-edge normal
// equal (the atlas boundary-seam invariant).
void GenerateNormalmapRegionFromHeightfield(
    const Terrain::HeightfieldData& heightfield,
    float32 worldSizeX, float32 worldSizeZ,
    float32 heightScale,
    std::vector<uint8>& normalmap,
    int32 minX, int32 minZ, int32 maxX, int32 maxZ,
    const Terrain::HeightfieldData* neighborLeft = nullptr,
    const Terrain::HeightfieldData* neighborRight = nullptr,
    const Terrain::HeightfieldData* neighborUp = nullptr,
    const Terrain::HeightfieldData* neighborDown = nullptr);

// Bilinear-upsample a square R32F field from srcDim x srcDim to dstDim x dstDim,
// writing dstDim*dstDim floats into `out` (resized as needed). The endpoint texels
// (u,v in {0,1}) sample the source corners exactly, so two adjacent tiles' shared
// edge upsamples to bit-identical values, as the terrain's base already agrees there
// (it resolves per world position): that keeps tile seams crack-free. Used to render a Coarse (not-yet-Full) streamed tile as a
// smooth low-frequency approximation of its Full bake instead of a flat/black hole
// (planet-streaming coarse-tile rendering). No-op when srcDim < 2 or dstDim < 2.
void UpsampleHeightfieldBilinear(const float32* src, uint32 srcDim,
                                 std::vector<float32>& out, uint32 dstDim);

// Bilinear-upsample a square RGBA8 splatmap from srcDim x srcDim to dstDim x dstDim
// (per-channel bilinear, round-to-nearest). Endpoints exact, like the height variant.
void UpsampleSplatmapBilinear(const uint8* src, uint32 srcDim,
                              std::vector<uint8>& out, uint32 dstDim);

// Which sides of a coarse tile's unified patch are shared with a resident-Full
// neighbor. In the unified texture adjacent tiles overlap by one texel (tile
// (tx,tz) starts at tx*interior), so a coarse tile's boundary row/column IS the
// Full neighbor's edge. A coarse write there would stomp the Full side's exact +
// modifier-applied edge with approximate, modifier-free data — a persistent 1-2
// texel seam trench until the coarse tile upgrades. The Full side owns those
// shared texels; a coarse↔coarse edge upsamples bit-identically (endpoints exact)
// so it needs no exclusion.
struct CoarsePatchNeighbors
{
    bool LeftFull = false;   // -X neighbor is resident Full
    bool RightFull = false;  // +X neighbor is resident Full
    bool TopFull = false;    // -Z neighbor is resident Full
    bool BottomFull = false; // +Z neighbor is resident Full
};

// The sub-rect (in a coarse tile's regionDim x regionDim upsample) that should be
// uploaded, excluding the 1-texel boundary shared with a resident-Full neighbor.
struct CoarsePatchRect
{
    uint32 X0 = 0, Z0 = 0, Width = 0, Height = 0;
};

// Pure function: the interior-clamped upload rect for a coarse tile given which
// neighbors are resident Full. Directly unit-testable (the coarse↔Full shared-edge
// oracle) — the discriminating check the coarse↔coarse oracle could not see.
CoarsePatchRect ComputeCoarsePatchRect(uint32 regionDim, const CoarsePatchNeighbors& neighbors);

// Copy the [x0,z0]+[width,height] sub-block out of a regionDim x regionDim row-major
// source into a tightly-packed destination (resized as needed). T is the per-texel
// unit: float32 for the R32F heightmap, uint32 for a packed RGBA8 splat texel.
template<typename T>
void ExtractSubBlock(const T* src, uint32 regionDim, uint32 x0, uint32 z0,
                     uint32 width, uint32 height, std::vector<T>& out)
{
    out.resize(static_cast<std::size_t>(width) * height);
    for (uint32 z = 0; z < height; ++z)
        for (uint32 x = 0; x < width; ++x)
            out[static_cast<std::size_t>(z) * width + x] =
                src[static_cast<std::size_t>(z0 + z) * regionDim + (x0 + x)];
}

} // namespace GameEngine::TerrainECS
