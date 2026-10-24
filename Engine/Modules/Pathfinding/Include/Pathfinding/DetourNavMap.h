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
// Data is chart-local: the same input around the same chart-local point gives the same bytes
// wherever the chart sits in the world.
struct NavMeshTileData
{
    NavTileKey Key;
    std::vector<uint8> Data;
    uint32 PolygonCount = 0;
    // The hash of the content the tile was built from; the mesh keeps it beside the tile.
    uint64 ContentHash = 0;
};

// A Recast/Detour navigation mesh of one chart (NavMeshChart), cut into square tiles so one tile
// can be rebuilt and swapped without touching its neighbours. Detour runs in chart-local
// coordinates with its origin at zero; the queries take and return world points and convert once
// per call. Initialize sets the agent and the chart; BuildTile turns chart-local triangles into
// one tile's data on any thread; AddTile and RemoveTile change the mesh on the thread that
// queries it.
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

    // A carve: a vertical prism whose inside is not walkable. A convex polygon in XZ, as
    // chart-local XYZ points (Y unused), between MinY and MaxY; walkable ground whose surface lies
    // in that height range is cut out, and the agent keeps its radius from the cut.
    struct CarvePrism
    {
        const float32* Points = nullptr; // [PointCount * 3] XYZ interleaved
        uint32 PointCount = 0;
        float32 MinY = 0.0f;
        float32 MaxY = 0.0f;
    };

    // Chart-local triangles a tile is built from, and the carves cut out of it.
    struct InputGeometry
    {
        const float32* Vertices = nullptr; // [VertexCount * 3] XYZ interleaved
        uint32 VertexCount = 0;
        const uint32* Indices = nullptr;   // [TriangleCount * 3]
        uint32 TriangleCount = 0;
        const CarvePrism* Carves = nullptr;
        uint32 CarveCount = 0;
    };

    // Detour packs a polygon reference into 32 bits: 12 for the tile slot (the pool of
    // kNavChartTilePool tiles a chart holds), 10 for the polygon in its tile and 10 for the salt
    // that refuses a reference to a tile since replaced.
    static constexpr uint32 kMaxTiles = kNavChartTilePool;
    static constexpr uint32 kMaxPolygonsPerTile = 1u << 10;

    // Replaces the mesh with an empty one for `settings`'s agent in `chart`'s frame. Fails, leaving
    // the map empty, when the cell size is not positive or the chart's rotation is not the
    // identity.
    bool Initialize(const NavMeshSettings& settings, const NavMeshChart& chart);
    bool IsBuilt() const;
    const NavMeshSettings& GetSettings() const;
    const NavMeshChart& GetChart() const;

    // Metres along a tile's side.
    static float32 GetTileSize(const NavMeshSettings& settings);
    // Tile (tileX, tileZ)'s chart-local XZ rectangle, without the border Recast reads around it.
    static void GetTileBounds(const NavMeshSettings& settings, int32 tileX, int32 tileZ, float32& outMinX,
                              float32& outMinZ, float32& outMaxX, float32& outMaxZ);
    // Metres a tile's build reads beyond its rectangle on every side: the agent radius plus three
    // cells, so walkable area erodes the same on both sides of a tile edge.
    static float32 GetTileBorder(const NavMeshSettings& settings);

    // Builds tile `key` from chart-local `geometry`, which must cover the tile's rectangle widened
    // by GetTileBorder. Reads only its arguments, so it runs on any thread. False with `outError`
    // when Recast or Detour refuses the input; a tile with no walkable area succeeds with empty data.
    static bool BuildTile(const NavMeshSettings& settings, const NavTileKey& key, const InputGeometry& geometry,
                          NavMeshTileData& outTile, std::string& outError);

    // Puts `tile` into the mesh in place of whatever was at its key. Empty data (no walkable area)
    // removes what was there and keeps the key and its content hash.
    // False, with the reason in `outRefusal` when given, when the map is not initialized, the pool
    // is full, or the bytes are not a tile Detour can hold safely: Detour reads them without bounds
    // checks, so the header's counts are checked against the bytes, its coordinates against the
    // key, and every polygon and detail index against its array. A refused tile leaves the old one
    // in place. A tile
    // goes into the pool slot freed longest ago, so a polygon reference held across swaps stays
    // refused until its slot has been reused 1,023 times (the salt's period): with F free slots,
    // after 1,023 x F swaps.
    bool AddTile(const NavMeshTileData& tile, std::string* outRefusal = nullptr);
    // Removes the tile at `key`, empty or not. False when there was none.
    bool RemoveTile(const NavTileKey& key);

    uint32 GetTileCount() const;
    uint32 GetPolygonCount() const;
    uint64 GetTileDataBytes() const;
    // Every tile the mesh holds as it was added, empty ones included: its key, its bytes as built
    // (Detour links tiles in a copy of its own) and its content hash.
    void CopyTiles(std::vector<NavMeshTileData>& out) const;

    // Debug mesh access: the polygons of every tile as a triangle list in world metres, rebuilt on
    // first access after a tile changes.
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

    // Opaque access to Detour internals (for CrowdManager), in chart-local coordinates.
    void* GetDetourNavMesh() const;
    void* GetDetourNavMeshQuery() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace GameEngine::Pathfinding
