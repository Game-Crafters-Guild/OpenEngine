#pragma once

#include "AssetCore/AssetTypes.h"
#include "Components/AssetRef.h"
#include "Mathematics/Vector3.h"
#include "Pathfinding/PathfindingTypes.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// What a surface is baked into. A mesh is a Recast/Detour navigation mesh, the default for agents
// that walk anywhere their size fits. A grid is one height and one walkable flag per cell, for
// games that need per-cell costs, cell reservations or hex cells.
enum class NavigationSurfaceMode : uint8
{
    Mesh = 0,
    Grid = 1,
};

// Where a surface's bake stands. Written by NavigationSurfaceSystem; read by the inspector and the
// debug server.
enum class NavigationBakePhase : uint8
{
    NotBaked = 0,
    Waiting = 1,
    Baking = 2,
    Baked = 3,
    Failed = 4,
};

// What a bake waits for, when BakePhase is Waiting.
enum class NavigationBakeWait : uint8
{
    // A terrain tile inside the bounds has not streamed in; BlockingTileX/Z name it.
    TerrainTile = 0,
    // The terrain exists but its heightfield has not been built yet.
    TerrainData = 1,
    // A model a baked mesh renderer shows is still loading; BlockingModel names it.
    Model = 2,
    // The surface's baked data (BakedData) is still loading.
    BakedData = 3,
};

// Why a bake failed, when BakePhase is Failed.
enum class NavigationBakeFailure : uint8
{
    None = 0,
    // Nothing the bake reads lies inside the bounds.
    NoGeometry = 1,
    // The only terrain is a planet, which has no planar heightfield to read.
    SphericalTerrain = 2,
    // The bounds cut into more tiles (mesh) or cells (grid) than a surface holds.
    TooLarge = 3,
    // Recast or Detour refused one tile; FailedTileX/Z name it and the log says why.
    TileBuild = 4,
};

// A walkable surface for navigation agents, baked from the terrain, static collider shapes and
// mesh renderers inside its bounds. When it first appears a mesh surface loads its tiles from
// BakedData if that matches its settings and bounds, and bakes otherwise; it bakes again on
// RequestBake (PathfindingECS/NavigationScriptApi.h). Once baked, a mesh surface rebakes only the
// tiles a terrain edit, a carving obstacle or Navigation::MarkDirty touches, a few a frame, each
// tile serving until its new one replaces it. No physics world is needed, so it bakes in edit mode,
// in Play and in a game alike.
struct NavigationSurface
{
    // @ge-tooltip Mesh: a navigation mesh agents walk anywhere their size fits. Grid: one height and one walkable flag per cell, for per-cell costs, reservations and hex cells.
    NavigationSurfaceMode Mode = NavigationSurfaceMode::Mesh;
    // Grid mode only: square or hex cells.
    // @ge-tooltip Square or hex cells.
    Pathfinding::GridType GridType = Pathfinding::GridType::Square;

    // The agent the surface is baked for, in metres and degrees.
    // @ge-tooltip Meters. The surface keeps this far from walls and drops gaps narrower than twice it.
    // @ge-range 0.01
    float32 AgentRadius = 0.5f;
    // @ge-tooltip Meters. Space under an overhang lower than this is not walkable.
    // @ge-range 0.01
    float32 AgentHeight = 1.8f;
    // @ge-tooltip Degrees. Ground steeper than this is not walkable.
    // @ge-range 0 89
    float32 MaxSlope = 45.0f;
    // @ge-tooltip Meters. The tallest step an agent climbs without a slope.
    // @ge-range 0
    float32 StepHeight = 0.4f;

    // The one resolution dial: the voxel size of a mesh (a tile is 64 cells on a side) or the cell
    // size of a grid.
    // @ge-tooltip Meters. The surface's resolution: a mesh is voxelized at this size and cut into tiles of 64 cells; a grid has one cell of this size. Smaller follows the ground more closely and bakes longer.
    // @ge-range 0.05
    float32 CellSize = 0.5f;

    // Bounds: the planar terrain's footprint, else everything the bake reads, when FitBounds is
    // set; otherwise the box of half size Extent around Center, which is relative to the entity's
    // position.
    // @ge-tooltip Bake over the terrain's footprint, or over everything the bake reads when there is no terrain. Off: bake inside Center and Extent.
    bool FitBounds = true;
    // @ge-tooltip Meters from the entity's position to the middle of the baked box.
    Mathematics::Vector3 Center{0.0f, 0.0f, 0.0f};
    // @ge-tooltip Meters: half the baked box's size.
    Mathematics::Vector3 Extent{50.0f, 50.0f, 50.0f};

    // Grid mode only: the costs and blocked cells painted with the grid brush (.navgrid).
    // @ge-tooltip Grid only: the cell costs and blocked cells painted with the navigation grid brush (.navgrid).
    AssetRef<AssetType::NavigationGrid> AuthoredCells;
    // Mesh mode only: the baked tiles saved with the scene (.navsurface), written by the editor
    // when the scene is saved.
    // @ge-tooltip Mesh only: the baked tiles saved with the scene (.navsurface). The surface loads them instead of baking when they match its settings and bounds. Written when the scene is saved.
    AssetRef<AssetType::NavigationSurfaceData> BakedData;

    // Runtime state below, never saved and never edited. The binding to the NavigationWorld map
    // this component owns is written only through PathfindingECS/NavigationSurfaceRuntime.h;
    // OwnerWorldId stamps the creating world, so a copy of a bound component in another world names
    // a map it does not own.
    // @ge-hidden
    // [DoNotSerialize]
    uint32 NavMapIndex = 0;
    // @ge-hidden
    // [DoNotSerialize]
    uint32 NavMapGeneration = 0;
    // @ge-hidden
    // [DoNotSerialize]
    uint64 OwnerWorldId = 0;
    // Set by RequestBake; NavigationSurfaceSystem clears it when it starts the bake.
    // @ge-hidden
    // [DoNotSerialize]
    bool BakeRequested = false;

    // The bake's state as NavigationSurfaceSystem last published it (FormatNavigationBakeStatus
    // turns it into the inspector's text).
    // @ge-hidden
    // [DoNotSerialize]
    NavigationBakePhase BakePhase = NavigationBakePhase::NotBaked;
    // @ge-hidden
    // [DoNotSerialize]
    NavigationBakeFailure BakeFailure = NavigationBakeFailure::None;
    // @ge-hidden
    // [DoNotSerialize]
    NavigationBakeWait BakeWait = NavigationBakeWait::TerrainTile;
    // The authored fields differ from the ones the current bake was made with.
    // @ge-hidden
    // [DoNotSerialize]
    bool SettingsChanged = false;
    // The map holds the tiles or cells of a completed bake. They serve agents while a rebake runs,
    // waits or fails; a bake into an empty map (the first, or a new layout or Mode) clears it until
    // the bake completes.
    // @ge-hidden
    // [DoNotSerialize]
    bool MapHoldsBake = false;
    // The mode the current bake was made in: the numbers below count its tiles or cells.
    // @ge-hidden
    // [DoNotSerialize]
    NavigationSurfaceMode BakedMode = NavigationSurfaceMode::Mesh;
    // Mesh: tiles built and the layout's tile count. Grid: cells.
    // @ge-hidden
    // [DoNotSerialize]
    uint32 BakeDone = 0;
    // @ge-hidden
    // [DoNotSerialize]
    uint32 BakeTotal = 0;
    // Mesh: polygons. Grid: walkable cells.
    // @ge-hidden
    // [DoNotSerialize]
    uint32 BakedWalkable = 0;
    // @ge-hidden
    // [DoNotSerialize]
    uint64 BakedBytes = 0;
    // Mesh: counts the tiles put into the map (by a bake, a load from BakedData or a rebake), so it
    // changes whenever a tile changes. Grid: counts completed bakes.
    // @ge-hidden
    // [DoNotSerialize]
    uint64 BakeVersion = 0;
    // Mesh: tiles loaded from BakedData whose content (the terrain and the sources under them)
    // changed since they were baked. They serve as they are until their rebake replaces them.
    // @ge-hidden
    // [DoNotSerialize]
    uint32 StaleTiles = 0;
    // Mesh: tiles waiting to rebake, and tiles rebaking now.
    // @ge-hidden
    // [DoNotSerialize]
    uint32 TilesDirty = 0;
    // @ge-hidden
    // [DoNotSerialize]
    uint32 TilesBaking = 0;
    // Mesh: tiles built with Recast since the surface appeared; a surface that loaded all its
    // tiles from BakedData built none.
    // @ge-hidden
    // [DoNotSerialize]
    uint32 TilesBuilt = 0;
    // Waiting: the terrain tile that is not resident. Failed with TileBuild: the surface tile.
    // Baked with StaleTiles: the first stale tile.
    // @ge-hidden
    // [DoNotSerialize]
    int32 BlockingTileX = 0;
    // @ge-hidden
    // [DoNotSerialize]
    int32 BlockingTileZ = 0;
    // Waiting for Model: the model still loading.
    // @ge-hidden
    // [DoNotSerialize]
    AssetRef<AssetType::Model> BlockingModel;
};

static_assert(std::is_trivially_copyable_v<NavigationSurface>);
static_assert(std::is_standard_layout_v<NavigationSurface>);

} // namespace GameEngine::Components
