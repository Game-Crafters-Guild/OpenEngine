#pragma once

#include "Pathfinding/PathfindingTypes.h"
#include "Types/Types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
} // namespace JobSystem

namespace GameEngine::Pathfinding
{
class DetourNavMap;
struct NavSurfaceData;
struct NavSurfaceTile;
} // namespace GameEngine::Pathfinding

namespace GameEngine::PathfindingECS
{

struct NavigationStaticGeometry;
struct NavigationTerrainSource;
struct NavigationTriangles;

// A carving obstacle in world metres: a vertical cylinder of Radius about (X, Z) that cuts the
// walkable ground whose surface lies between MinY and MaxY.
struct NavigationCarve
{
    float32 X = 0.0f;
    float32 Z = 0.0f;
    float32 Radius = 0.0f;
    float32 MinY = 0.0f;
    float32 MaxY = 0.0f;

    bool operator==(const NavigationCarve&) const = default;
    // True when the cylinder's XZ square touches the rectangle.
    bool OverlapsXZ(float32 minX, float32 minZ, float32 maxX, float32 maxZ) const
    {
        return X + Radius >= minX && X - Radius <= maxX && Z + Radius >= minZ && Z - Radius <= maxZ;
    }
};

// The hash of a tile's content, from the inputs a bake of it reads: the terrain lattice under the
// tile's rectangle widened by the border, and the static sources and the carves overlapping that
// rectangle (their vertex positions and their cylinders, in any gather order). Seeded with the
// .navsurface format version; never fed a counter, a handle or an entity id, so two sessions agree
// on unchanged ground.
uint64 NavigationTileContentHash(const NavigationTriangles& lattice, const NavigationStaticGeometry& geometry,
                                 const std::vector<NavigationCarve>& carves, float32 minX, float32 minZ, float32 maxX,
                                 float32 maxZ);

// One navigation mesh bake in progress: a list of a chart's tiles (all of them for a bake, the
// dirty ones for a rebake), built on the job system and put into the mesh as each finishes, so
// every tile serves until its new one lands. With baked data (UseBakedTiles), a tile the data holds is not built:
// its job hashes the tile's content and the data's bytes go into the mesh, stale when the hash
// differs from the one they were baked with. Given the mesh's current tiles (SkipUnchangedTiles),
// a tile whose content hash equals the one its current tile was built with is not built again.
//
// Threading: the main thread snapshots a tile's terrain lattice (the terrain's heightfield is
// main-thread data) and submits a job that owns that snapshot, a shared reference to the immutable
// static geometry and its own result. A job touches nothing else, so a bake destroyed with jobs in
// flight simply abandons them; their results are freed with the last reference.
class NavigationMeshBake
{
  public:
    NavigationMeshBake(const Pathfinding::NavMeshSettings& settings, const Pathfinding::NavMeshChart& chart,
                       std::vector<Pathfinding::NavTileKey> tiles,
                       std::shared_ptr<const NavigationStaticGeometry> geometry,
                       std::shared_ptr<const std::vector<NavigationCarve>> carves, uint32 terrainStride);
    ~NavigationMeshBake();

    NavigationMeshBake(const NavigationMeshBake&) = delete;
    NavigationMeshBake& operator=(const NavigationMeshBake&) = delete;

    // Loads the tiles the first chart of `data`'s first profile holds instead of building them.
    void UseBakedTiles(std::shared_ptr<const Pathfinding::NavSurfaceData> data);
    // Leaves out the tiles whose content hash equals the one `mesh`'s tile at the same key was built
    // with: their content did not change, so a rebuild would make the same bytes.
    void SkipUnchangedTiles(const Pathfinding::DetourNavMap& mesh);

    // Snapshots and submits up to `maxTiles` more tiles, in list order. With no pool the jobs run
    // here. False when the next tile reads terrain height not on the CPU yet (AppendTerrainLattice):
    // `outWaitPoint` is where, and the same tile is tried again on the next call.
    bool SubmitTiles(const NavigationTerrainSource& terrain, JobSystem::WorkStealingThreadPool* pool, uint32 maxTiles,
                     Mathematics::Vector3& outWaitPoint);

    // Puts every finished tile into `map`, appending each tile's key to `outApplied`. False when a
    // tile failed to build or the mesh refused it: `outFailed` and `outError` name the first, and
    // the other finished tiles still go in. Baked bytes the mesh refuses count as stale instead.
    bool ApplyFinishedTiles(Pathfinding::DetourNavMap& map, std::vector<Pathfinding::NavTileKey>& outApplied,
                            Pathfinding::NavTileKey& outFailed, std::string& outError);

    bool IsComplete() const;
    // Tiles finished: put into the mesh, left as they were, or failed.
    uint32 GetFinishedTileCount() const { return m_FinishedTiles; }
    uint32 GetTileCount() const { return static_cast<uint32>(m_Tiles.size()); }
    // Tiles submitted and not applied yet.
    uint32 GetInFlightTileCount() const { return static_cast<uint32>(m_InFlight.size()); }
    // Tiles built with Recast, as opposed to loaded from baked data or left out unchanged.
    uint32 GetBuiltTileCount() const { return m_BuiltTiles; }
    // Loaded tiles whose content changed since they were baked, in the order they were applied.
    const std::vector<Pathfinding::NavTileKey>& GetStaleTiles() const { return m_StaleTiles; }

  private:
    struct TileJob;
    // The job body: reads only `job`, writes only `job`.
    static void RunTileJob(TileJob& job);

    Pathfinding::NavMeshSettings m_Settings;
    Pathfinding::NavMeshChart m_Chart;
    std::vector<Pathfinding::NavTileKey> m_Tiles;
    std::shared_ptr<const NavigationStaticGeometry> m_Geometry;
    std::shared_ptr<const std::vector<NavigationCarve>> m_Carves;
    uint32 m_TerrainStride = 1;
    uint32 m_NextTile = 0;
    uint32 m_FinishedTiles = 0;
    uint32 m_BuiltTiles = 0;
    std::vector<std::shared_ptr<TileJob>> m_InFlight;
    std::vector<Pathfinding::NavTileKey> m_StaleTiles;
    // The baked data and its tiles by key; the tiles point into the data, which this keeps alive.
    std::shared_ptr<const Pathfinding::NavSurfaceData> m_BakedData;
    std::unordered_map<Pathfinding::NavTileKey, const Pathfinding::NavSurfaceTile*, Pathfinding::NavTileKeyHash>
        m_BakedTiles;
    // The content hashes of the mesh's current tiles at this bake's keys (SkipUnchangedTiles).
    std::unordered_map<Pathfinding::NavTileKey, uint64, Pathfinding::NavTileKeyHash> m_CurrentHashes;
};

} // namespace GameEngine::PathfindingECS
