#pragma once

#include "TerrainECS/TerrainGrassField.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"
#include "Spline/SplineData.h"
#include "Types/Types.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <span>
#include <vector>

namespace JobSystem { class WorkStealingThreadPool; }

namespace GameEngine::TerrainECS
{

// Which grass field of a terrain a compose target names.
enum class TerrainGrassFieldOwner : uint8
{
    Single,       // TerrainData::GrassField
    TiledUnified, // TiledTerrainData::GrassUnifiedField
    TiledCoarse,  // TiledTerrainData::GrassCoarseField
    TiledTile     // TerrainTileData::GrassField of one tile
};

// One field of a grass compose. The request names the field by handle and never holds a pointer
// to terrain data, so a terrain or tile removed while the compose runs is simply not found when
// the result is applied.
struct TerrainGrassComposeTarget
{
    TerrainGrassFieldOwner Owner = TerrainGrassFieldOwner::Single;
    TerrainHandle Terrain{};    // Single
    TiledTerrainHandle Tiled{}; // the Tiled owners
    TileCoord Tile{};           // TiledTile
    // The live field when the request was made. A field that no longer matches was replaced
    // meanwhile (a tile streamed out and back in), so the result does not describe it.
    uint64 SourceVersion = 0;
    bool SourceInitialized = false;
    // TiledUnified and TiledCoarse: TiledTerrainData::GrassRegionsActive for the requested stack,
    // set when the result swaps in.
    bool RegionsActive = false;
    // False when the field already shows the requested stack and only records it as applied.
    bool Compose = false;
    // The whole field rather than the edit's dirty rectangle.
    bool Entire = false;
    uint32 Width = 0;
    uint32 Height = 0;
    float32 SizeX = 0.0f;
    float32 SizeZ = 0.0f;
    float32 OriginX = 0.0f;
    float32 OriginZ = 0.0f;
    // Inclusive sample rectangle; negative maxima mean the full field (ComposeTerrainGrassField).
    int32 MinX = 0;
    int32 MinZ = 0;
    int32 MaxX = -1;
    int32 MaxZ = -1;
    // A copy of the live field with no pending upload, composed in place by the job.
    TerrainGrassField Field;
    std::chrono::duration<float64, std::milli> ComposeTime{};
};

// One grass compose: the stack, the fields it writes, and their results. Self-contained, so the
// job that composes it never reads the modifier system, the terrain service or the gather's
// arenas, all of which change or go away while it runs.
struct TerrainGrassComposeRequest
{
    // The modifier system's grass generation when the request was made; a result whose
    // generation is no longer current describes a stack the user has edited since.
    uint64 Generation = 0;
    // The grass stack in order, copied out of the gather for a job (empty for an in-place compose,
    // which reads the gather's own stack). Each ResolvedSpline points into Splines and Route is null.
    // Every entry is a volume holding only grass effects, so the zone payload pointers are never read.
    std::vector<ResolvedModifier> Modifiers;
    std::deque<Spline::SplineData> Splines;
    std::vector<TerrainGrassComposeTarget> Targets;
    // Set by ComposeTerrainGrassRequest once every target is composed. A job cancelled before it
    // ran (pool shutdown) completes its handle without setting it.
    std::atomic<bool> Composed{false};
};

// Copies `modifiers` into the request, each with its own copy of the spline it reads.
void SnapshotTerrainGrassModifiers(std::span<const ResolvedModifier> modifiers, TerrainGrassComposeRequest& request);

// Composes every target that asks for it from `modifiers` (row bands across `jobs` when given), then
// sets Composed. Legal on a pool worker.
void ComposeTerrainGrassRequest(TerrainGrassComposeRequest& request, std::span<const ResolvedModifier> modifiers,
                                JobSystem::WorkStealingThreadPool* jobs);

// The live field a target names, or null when its terrain or tile is gone.
TerrainGrassField* FindTerrainGrassField(TerrainService& service, const TerrainGrassComposeTarget& target);

// Moves a composed target's field into the live field it was copied from. An upload still pending
// on the live field is kept: its rectangle joins the result's on the same lattice (a resized result
// already uploads whole).
void ApplyTerrainGrassComposeTarget(TerrainGrassComposeTarget& target, TerrainGrassField& live);

} // namespace GameEngine::TerrainECS
