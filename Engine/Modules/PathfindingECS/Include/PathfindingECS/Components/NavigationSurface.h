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
// mesh renderers inside its bounds. It bakes when it first appears and again on RequestBake
// (PathfindingECS/Navigation.h); no physics world is needed, so it bakes in edit mode, in Play and
// in a game alike.
struct NavigationSurface
{
    NavigationSurfaceMode Mode = NavigationSurfaceMode::Mesh;
    // Grid mode only: square or hex cells.
    Pathfinding::GridType GridType = Pathfinding::GridType::Square;

    // The agent the surface is baked for, in metres and degrees.
    float32 AgentRadius = 0.5f;
    float32 AgentHeight = 1.8f;
    float32 MaxSlope = 45.0f;
    float32 StepHeight = 0.4f;

    // The one resolution dial: the voxel size of a mesh (a tile is 64 cells on a side) or the cell
    // size of a grid.
    float32 CellSize = 0.5f;

    // Bounds: the planar terrain's footprint, else everything the bake reads, when FitBounds is
    // set; otherwise the box of half size Extent around Center, which is relative to the entity's
    // position.
    bool FitBounds = true;
    Mathematics::Vector3 Center{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 Extent{50.0f, 50.0f, 50.0f};

    // Grid mode only: the costs and blocked cells painted with the grid brush (.navgrid).
    AssetRef<AssetType::NavigationGrid> AuthoredCells;

    // Runtime state below, never saved and never edited. The binding to the NavigationWorld map
    // this component owns is written only through PathfindingECS/NavigationSurfaceRuntime.h;
    // OwnerWorldId stamps the creating world, so a copy of a bound component in another world names
    // a map it does not own.
    uint32 NavMapIndex = 0;
    uint32 NavMapGeneration = 0;
    uint64 OwnerWorldId = 0;
    // Set by RequestBake; NavigationSurfaceSystem clears it when it starts the bake.
    bool BakeRequested = false;

    // The bake's state as NavigationSurfaceSystem last published it (FormatNavigationBakeStatus
    // turns it into the inspector's text).
    NavigationBakePhase BakePhase = NavigationBakePhase::NotBaked;
    NavigationBakeFailure BakeFailure = NavigationBakeFailure::None;
    NavigationBakeWait BakeWait = NavigationBakeWait::TerrainTile;
    // The authored fields differ from the ones the current bake was made with.
    bool SettingsChanged = false;
    // The mode the current bake was made in: the numbers below count its tiles or cells.
    NavigationSurfaceMode BakedMode = NavigationSurfaceMode::Mesh;
    // Mesh: tiles built and the layout's tile count. Grid: cells.
    uint32 BakeDone = 0;
    uint32 BakeTotal = 0;
    // Mesh: polygons. Grid: walkable cells.
    uint32 BakedWalkable = 0;
    uint64 BakedBytes = 0;
    // Counts completed bakes of this surface's map.
    uint32 BakeVersion = 0;
    // Waiting: the terrain tile that is not resident. Failed with TileBuild: the surface tile.
    int32 BlockingTileX = 0;
    int32 BlockingTileZ = 0;
    // Waiting for Model: the model still loading.
    AssetRef<AssetType::Model> BlockingModel;
};

static_assert(std::is_trivially_copyable_v<NavigationSurface>);
static_assert(std::is_standard_layout_v<NavigationSurface>);

} // namespace GameEngine::Components
