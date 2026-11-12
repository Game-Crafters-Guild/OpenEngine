#pragma once

// What an authored terrain size + sample density actually cost, derived once. The extraction
// system builds these resources, the inspector must show the author their consequence BEFORE
// they commit to a value, and the tests pin the engagement threshold — all three read this one
// derivation, so no surface can predict a geometry the renderer will not build.
//
// Pure: no service, no device, no GPU; the derivation allocates nothing.

#include "Terrain/TerrainTypes.h"
#include "Types/Types.h"

#include <string>

namespace GameEngine::TerrainECS
{

// The height texture a terrain reads where it does not read its pages. The unified texture holds
// the whole terrain, up to one texture's size; past that size, the resident-window atlas holds the
// streaming window.
enum class TerrainHeightSource : uint32
{
    Unified, // one whole-terrain texture set
    Atlas,   // tiled, resident-window slot atlas + coarse far field
};

// The unified texture set is allocated at the terrain's full sample resolution, so this cap is
// what one texture can carry: 8 tiles of 1024 interior samples plus the shared edge. Past it the
// source would be tens of gigabytes.
inline constexpr uint32 kMaxUnifiedTiledResolution = 8193;

// The widest a planar terrain of this sample density may be along either axis, in metres: 2^20
// lattice intervals, the most the CBT depth cap resolves to one sample (1048576 m at 1 sample
// per metre).
float32 MaxTerrainExtentMetres(float32 samplesPerMeter);

// The densest a planar terrain this wide and deep may be sampled, in samples per metre: the
// inverse of MaxTerrainExtentMetres for the longer side.
float32 MaxTerrainSamplesPerMeter(float32 sizeX, float32 sizeZ);

// Empty when a planar terrain of this size and density can be built, else the message that
// states the limit and the fix. Creation, a scene file and a component write all use this rule.
std::string PlanarTerrainSizeError(float32 sizeX, float32 sizeZ, float32 samplesPerMeter);

// The most tiles per axis DeriveTerrainSizingPlan counts. A terrain PlanarTerrainSizeError
// refuses can still reach the derivation (the inspector's preview); saturating here keeps the
// tile product and the unified width inside uint32.
inline constexpr uint32 kMaxDerivedTilesPerAxis = 65535;

struct TerrainSizingPlan
{
    TerrainHeightSource Source = TerrainHeightSource::Unified;

    // ---- Sampling ----
    float32 MetresPerTexelNear = 0.0f; // detail actually resolved where the source is resident

    // ---- Tiling ----
    uint32 TilesPerAxisX = 0;
    uint32 TilesPerAxisZ = 0;
    uint32 TileResolution = 0;     // samples per tile axis (interior + shared edge)
    float32 TileWorldSize = 0.0f;
    // The authored size in metres. The tile grid covers TilesPerAxis * TileWorldSize, which is
    // larger when a side is not a whole number of tiles.
    float32 WorldSizeX = 0.0f;
    float32 WorldSizeZ = 0.0f;

    // ---- Samples across the whole terrain, per axis (the unified source's size) ----
    // Zero only for a degenerate terrain.
    uint32 UnifiedWidth = 0;
    uint32 UnifiedHeight = 0;

    // ---- Resident window (Source == Atlas) ----
    uint32 TotalTiles = 0;
    uint32 ResidentSlots = 0;      // tiles held at full detail at once
    // Side of the square resident window, in metres. What "full detail near the camera" buys.
    float32 ResidentWindowMetres = 0.0f;
    // Everything outside the window resolves through one coarse field spanning the WHOLE
    // terrain, so this degrades linearly with world size — the island-scale limitation.
    float32 MetresPerTexelFar = 0.0f;

    uint64 PredictedVramBytes = 0; // height + splat + normal for the chosen source

    // ---- Height pages (the platform's height page profile) ----
    uint64 HeightPages = 0;        // pages of the whole height pyramid
    uint64 PinnedHeightPages = 0;  // pages kept resident at all times (FirstPinnedPageLevel)

    bool FullyResident() const { return TotalTiles > 0 && ResidentSlots >= TotalTiles; }
};

// Derive the plan for an authored size + density. patchGridSize / lodRangeScale mirror
// TerrainConfig's, so a caller holding a live TiledTerrainConfig passes its own values and the
// authoring surface passes the engine defaults.
TerrainSizingPlan DeriveTerrainSizingPlan(float32 worldSizeX, float32 worldSizeZ,
                                          float32 samplesPerMeter,
                                          uint32 patchGridSize = Terrain::kDefaultGridSize,
                                          float32 lodRangeScale = Terrain::kDefaultLODRangeScale);

} // namespace GameEngine::TerrainECS
