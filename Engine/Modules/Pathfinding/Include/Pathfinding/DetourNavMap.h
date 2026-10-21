#pragma once

#include "Pathfinding/INavigationMap.h"
#include "Pathfinding/PathfindingTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Pathfinding
{

// One built Detour tile, produced by DetourNavMap::BuildTile and consumed by AddTile. Empty Data
// is a valid result: the tile holds no walkable area (all cliff, all water, nothing inside it).
struct NavMeshTileData
{
    uint32 TileX = 0;
    uint32 TileZ = 0;
    std::vector<uint8> Data;
    uint32 PolygonCount = 0;
};

// A Recast/Detour navigation mesh cut into square tiles (NavMeshTileLayout), so one tile can be
// rebuilt and swapped without touching its neighbours. Initialize sets the agent and the layout;
// BuildTile turns world-space triangles into one tile's data on any thread; AddTile and RemoveTile
// change the mesh on the thread that queries it.
class DetourNavMap : public INavigationMap
{
public:
    DetourNavMap();
    ~DetourNavMap();

    DetourNavMap(const DetourNavMap&) = delete;
    DetourNavMap& operator=(const DetourNavMap&) = delete;
    DetourNavMap(DetourNavMap&&) noexcept;
    DetourNavMap& operator=(DetourNavMap&&) noexcept;

    // INavigationMap
    PathStatus FindPath(const PathRequest& request, PathBuffer& pathBuffer, PathHandle& outPath) const override;
    bool IsPointNavigable(float32 x, float32 y, float32 z, float32 radius) const override;
    bool GetClosestNavigablePoint(float32 x, float32 y, float32 z, float32 searchRadius,
                                   float32& outX, float32& outY, float32& outZ) const override;
    bool Raycast(float32 startX, float32 startY, float32 startZ,
                 float32 endX, float32 endY, float32 endZ,
                 float32& hitX, float32& hitY, float32& hitZ) const override;

    // World-space triangles a tile is built from.
    struct InputGeometry
    {
        const float32* Vertices = nullptr; // [VertexCount * 3] XYZ interleaved
        uint32 VertexCount = 0;
        const uint32* Indices = nullptr;   // [TriangleCount * 3]
        uint32 TriangleCount = 0;
    };

    // The most tiles one mesh can hold: Detour addresses a polygon with 22 bits shared between the
    // tile and the polygon, and a tile keeps at least 256 polygons.
    static constexpr uint32 kMaxTiles = 1u << 14;

    // Replaces the mesh with an empty one cut by `layout` for `settings`'s agent. Fails, leaving
    // the map empty, when the layout has no tile or more than kMaxTiles.
    bool Initialize(const NavMeshSettings& settings, const NavMeshTileLayout& layout);
    bool IsBuilt() const;
    const NavMeshSettings& GetSettings() const;
    const NavMeshTileLayout& GetLayout() const;

    // Tile (tileX, tileZ)'s world-space XZ rectangle, without the border Recast reads around it.
    static void GetTileBounds(const NavMeshSettings& settings, const NavMeshTileLayout& layout, uint32 tileX,
                              uint32 tileZ, float32& outMinX, float32& outMinZ, float32& outMaxX, float32& outMaxZ);
    // Metres a tile's build reads beyond its rectangle on every side: the agent radius plus three
    // cells, so walkable area erodes the same on both sides of a tile edge.
    static float32 GetTileBorder(const NavMeshSettings& settings);

    // Builds tile (tileX, tileZ) from `geometry`, which must cover the tile's rectangle widened by
    // GetTileBorder. Reads only its arguments, so it runs on any thread. False with `outError`
    // when Recast or Detour refuses the input; a tile with no walkable area succeeds with empty data.
    static bool BuildTile(const NavMeshSettings& settings, const NavMeshTileLayout& layout, uint32 tileX,
                          uint32 tileZ, const InputGeometry& geometry, NavMeshTileData& outTile,
                          std::string& outError);

    // Puts `tile` into the mesh in place of whatever was at its coordinates; empty data only
    // removes. False when the map is not initialized or Detour refuses the data.
    bool AddTile(const NavMeshTileData& tile);
    // Removes the tile at (tileX, tileZ). False when there was none.
    bool RemoveTile(uint32 tileX, uint32 tileZ);

    uint32 GetTileCount() const;
    uint32 GetPolygonCount() const;
    uint64 GetTileDataBytes() const;

    // Debug mesh access: the polygons of every tile as a triangle list, rebuilt on first access
    // after a tile changes.
    uint32 GetDebugVertexCount() const;
    uint32 GetDebugTriangleCount() const;
    const float32* GetDebugVertices() const;
    const uint32* GetDebugIndices() const;
    // The edges of the walkable surface (polygon edges with no neighbour, tile borders included where
    // the next tile has no polygon), two indices per edge into GetDebugVertices.
    uint32 GetDebugBoundaryEdgeCount() const;
    const uint32* GetDebugBoundaryIndices() const;
    // Changes whenever the debug mesh is rebuilt and never repeats across maps in a process, so a
    // copy of the debug mesh keyed on it is current while the version matches.
    uint64 GetDebugMeshVersion() const;

    // Opaque access to Detour internals (for CrowdManager)
    void* GetDetourNavMesh() const;
    void* GetDetourNavMeshQuery() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace GameEngine::Pathfinding
