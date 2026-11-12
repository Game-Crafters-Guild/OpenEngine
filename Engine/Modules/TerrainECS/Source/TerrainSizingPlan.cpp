#include "TerrainECS/TerrainSizingPlan.h"

#include "CBTTerrain/CBTLayout.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainPages.h"
#include "Types/FormatNumber.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <span>
#include <string_view>

namespace GameEngine::TerrainECS
{

namespace
{

// The sparsest density a terrain is derived at: below it the tile's world size runs away.
constexpr float32 kMinSamplesPerMeter = 0.01f;

// The most lattice intervals a planar side may hold: two CBT levels halve an edge, so the depth
// cap of kMaxDecodeSubdiv subdivisions resolves 2^(kMaxDecodeSubdiv / 2) intervals.
constexpr uint32 kMaxPlanarLatticeIntervals = 1u << (CBTTerrain::kMaxDecodeSubdiv / 2u);

// The atlas row key holds a tile coordinate in 16 bits, more than the depth bound needs.
static_assert(kMaxDerivedTilesPerAxis <= 0xFFFFu, "a tile coordinate must fit the atlas row key");
static_assert(kMaxPlanarLatticeIntervals / (Terrain::kMaxTileResolution - 1) <= kMaxDerivedTilesPerAxis,
              "the largest planar terrain must fit the derived tile count");

} // namespace

float32 MaxTerrainExtentMetres(float32 samplesPerMeter)
{
    return static_cast<float32>(kMaxPlanarLatticeIntervals) / std::max(samplesPerMeter, kMinSamplesPerMeter);
}

float32 MaxTerrainSamplesPerMeter(float32 sizeX, float32 sizeZ)
{
    return static_cast<float32>(kMaxPlanarLatticeIntervals) / std::max(sizeX, sizeZ);
}

std::string PlanarTerrainSizeError(float32 sizeX, float32 sizeZ, float32 samplesPerMeter)
{
    if (std::isnan(samplesPerMeter))
        return "The terrain's samples per meter is not a number. Enter a number.";
    if (std::isnan(sizeX) || std::isnan(sizeZ))
        return "The terrain's size is not a number. Enter a size in meters.";
    // In doubles so the bound is exact.
    const double density = std::max(static_cast<double>(samplesPerMeter), static_cast<double>(kMinSamplesPerMeter));
    const double longer = std::max(static_cast<double>(sizeX), static_cast<double>(sizeZ));
    if (sizeX > 0.0f && sizeZ > 0.0f && longer * density <= static_cast<double>(kMaxPlanarLatticeIntervals))
        return {};
    const std::string_view unit = samplesPerMeter == 1.0f ? "sample" : "samples";
    return std::format("A terrain at {:g} {} per meter is up to {} m on each side; this one "
                       "is {} x {} m. Make it smaller or lower its samples per meter.",
                       samplesPerMeter, unit, FormatGroupedWholeNumber(MaxTerrainExtentMetres(samplesPerMeter)),
                       FormatGroupedWholeNumber(sizeX), FormatGroupedWholeNumber(sizeZ));
}

TerrainSizingPlan DeriveTerrainSizingPlan(float32 worldSizeX, float32 worldSizeZ,
                                          float32 samplesPerMeter, uint32 patchGridSize,
                                          float32 lodRangeScale)
{
    TerrainSizingPlan plan;
    if (worldSizeX <= 0.0f || worldSizeZ <= 0.0f)
        return plan;

    samplesPerMeter = std::max(samplesPerMeter, kMinSamplesPerMeter);
    patchGridSize = std::clamp(patchGridSize, Terrain::kMinPatchSize, Terrain::kMaxPatchSize);

    // The grid TerrainService::CreateTiledTerrain builds: square tiles, capped at the per-tile
    // sample ceiling, so the tile's world size falls out of the cap and the density. A terrain
    // inside the cap is one tile along its shorter side.
    const float32 maxTileWorld =
        static_cast<float32>(Terrain::kMaxTileResolution - 1) / samplesPerMeter;
    const float32 tileWorld = std::min({worldSizeX, worldSizeZ, maxTileWorld});
    const Terrain::TerrainConfig tile = Terrain::TerrainConfig::FromSamplesPerMeter(
        tileWorld, tileWorld, /*heightScale*/ 0.0f, samplesPerMeter, patchGridSize, lodRangeScale);

    plan.TileWorldSize = tileWorld;
    plan.WorldSizeX = worldSizeX;
    plan.WorldSizeZ = worldSizeZ;
    plan.TileResolution = tile.HeightmapWidth;
    // Counted in float and saturated before the conversion: a side the float range allows but no
    // tile grid does (a scene file's, a component write's, a NaN) would overflow the cast and the
    // products.
    const auto tilesAlong = [tileWorld](float32 side) {
        const float32 tiles = std::ceil(side / tileWorld);
        return tiles < static_cast<float32>(kMaxDerivedTilesPerAxis) ? static_cast<uint32>(tiles)
                                                                     : kMaxDerivedTilesPerAxis;
    };
    plan.TilesPerAxisX = tilesAlong(worldSizeX);
    plan.TilesPerAxisZ = tilesAlong(worldSizeZ);
    plan.TotalTiles = std::max(1u, plan.TilesPerAxisX * plan.TilesPerAxisZ);

    const uint32 tileInterior = plan.TileResolution > 0 ? plan.TileResolution - 1u : 0u;
    plan.UnifiedWidth = plan.TilesPerAxisX * tileInterior + 1u;
    plan.UnifiedHeight = plan.TilesPerAxisZ * tileInterior + 1u;

    // The near-field detail MUST come from the tile, not from a whole-terrain config: past
    // kMaxHeightmapDimension the whole-terrain derivation clamps and under-reports it (an 8 km
    // terrain at 2 samples/m clamps to 8193 and reads 1 m/texel, when the tiles deliver 0.5 m).
    plan.MetresPerTexelNear = tileWorld / static_cast<float32>(std::max(1u, tileInterior));

    // The height pyramid lies on the tile lattice; a pyramid within a quarter of the cache is
    // pinned whole and never streams (FirstPinnedPageLevel). The levels stay on the stack: the CBT
    // update derives a plan every frame.
    std::array<PageStreaming::PageStoreLevel, PageStreaming::kPageStoreMaxLevels> levelStorage;
    const std::span<const PageStreaming::PageStoreLevel> levels =
        PageStreaming::FillPageStoreLevels(plan.UnifiedWidth, plan.UnifiedHeight, levelStorage);
    plan.HeightPages = PageStreaming::DensePageCount(levels);
    const uint32 firstPinned =
        FirstPinnedPageLevel(levels, 1u, plan.MetresPerTexelNear, PlatformHeightPageProfile().Slots);
    for (std::size_t level = firstPinned; level < levels.size(); ++level)
        plan.PinnedHeightPages += static_cast<uint64>(levels[level].PagesX) * levels[level].PagesZ;

    if (plan.UnifiedWidth <= kMaxUnifiedTiledResolution &&
        plan.UnifiedHeight <= kMaxUnifiedTiledResolution)
    {
        plan.Source = TerrainHeightSource::Unified;
        plan.ResidentSlots = plan.TotalTiles; // the whole terrain is one texture: all of it is live
        plan.ResidentWindowMetres = std::max(worldSizeX, worldSizeZ);
        plan.MetresPerTexelFar = plan.MetresPerTexelNear;
        plan.PredictedVramBytes = static_cast<uint64>(plan.UnifiedWidth) * plan.UnifiedHeight *
                                  kAtlasSlotBytesPerTexel;
        return plan;
    }

    plan.Source = TerrainHeightSource::Atlas;
    // The plan is derived from the terrain's own config, before any modifier volume exists,
    // so it prices the ground maps alone. A terrain that later carries a regional grass
    // control atlas is charged 2 more bytes per texel at residency time
    // (TerrainExtractionSystem), which buys fewer slots than this notice reports.
    plan.ResidentSlots = std::min(
        plan.TotalTiles,
        DeriveAtlasSlotCount(plan.TilesPerAxisX, plan.TilesPerAxisZ, plan.TileResolution,
                             kAtlasVramBudgetBytes, kAtlasSlotBytesPerTexel));
    // The window is square-packed, so its side in tiles is the floor of the slot count's root.
    const uint32 windowTiles = static_cast<uint32>(
        std::floor(std::sqrt(static_cast<double>(plan.ResidentSlots))));
    plan.ResidentWindowMetres = static_cast<float32>(windowTiles) * tileWorld;
    plan.MetresPerTexelFar = std::max(worldSizeX, worldSizeZ) /
                             static_cast<float32>(kAtlasCoarseFieldDim - 1u);

    const uint64 slotStride = plan.TileResolution + 2ull * kAtlasApron;
    const uint32 slotsPerRow = static_cast<uint32>(
        std::ceil(std::sqrt(static_cast<double>(std::max(1u, plan.ResidentSlots)))));
    const uint64 atlasDim = static_cast<uint64>(slotsPerRow) * slotStride;
    plan.PredictedVramBytes =
        atlasDim * atlasDim * kAtlasSlotBytesPerTexel +
        static_cast<uint64>(kAtlasCoarseFieldDim) * kAtlasCoarseFieldDim * kAtlasSlotBytesPerTexel;
    return plan;
}

} // namespace GameEngine::TerrainECS
