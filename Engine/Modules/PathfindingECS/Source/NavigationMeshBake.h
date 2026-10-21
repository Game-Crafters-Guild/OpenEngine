#pragma once

#include "Pathfinding/PathfindingTypes.h"
#include "Types/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
} // namespace JobSystem

namespace GameEngine::Pathfinding
{
class DetourNavMap;
} // namespace GameEngine::Pathfinding

namespace GameEngine::PathfindingECS
{

struct NavigationStaticGeometry;
struct NavigationTerrainSource;

// One navigation mesh bake in progress: every tile of a layout, built on the job system and put
// into the mesh as it finishes, so the tiles a bake has not reached yet keep serving.
//
// Threading: the main thread snapshots a tile's terrain lattice (the terrain's heightfield is
// main-thread data) and submits a job that owns that snapshot, a shared reference to the immutable
// static geometry and its own result. A job touches nothing else, so a bake destroyed with jobs in
// flight simply abandons them; their results are freed with the last reference.
class NavigationMeshBake
{
  public:
    NavigationMeshBake(const Pathfinding::NavMeshSettings& settings, const Pathfinding::NavMeshTileLayout& layout,
                       std::shared_ptr<const NavigationStaticGeometry> geometry, uint32 terrainStride);
    ~NavigationMeshBake();

    NavigationMeshBake(const NavigationMeshBake&) = delete;
    NavigationMeshBake& operator=(const NavigationMeshBake&) = delete;

    // Snapshots and submits up to `maxTiles` more tiles, in row order. With no pool the jobs run
    // here. False when the next tile reads a terrain tile that is not resident: `outTerrainTileX`
    // and `outTerrainTileZ` name it, and the same tile is tried again on the next call.
    bool SubmitTiles(const NavigationTerrainSource& terrain, JobSystem::WorkStealingThreadPool* pool, uint32 maxTiles,
                     int32& outTerrainTileX, int32& outTerrainTileZ);

    // Puts every finished tile into `map`. False when a tile failed to build: `outTileX`,
    // `outTileZ` and `outError` say which and why.
    bool ApplyFinishedTiles(Pathfinding::DetourNavMap& map, uint32& outTileX, uint32& outTileZ, std::string& outError);

    bool IsComplete() const;
    uint32 GetAppliedTileCount() const { return m_AppliedTiles; }
    uint32 GetTileCount() const { return m_Layout.TilesX * m_Layout.TilesZ; }

  private:
    struct TileJob;
    // The job body: reads only `job`, writes only `job`.
    static void RunTileJob(TileJob& job);

    Pathfinding::NavMeshSettings m_Settings;
    Pathfinding::NavMeshTileLayout m_Layout;
    std::shared_ptr<const NavigationStaticGeometry> m_Geometry;
    uint32 m_TerrainStride = 1;
    uint32 m_NextTile = 0;
    uint32 m_AppliedTiles = 0;
    std::vector<std::shared_ptr<TileJob>> m_InFlight;
};

} // namespace GameEngine::PathfindingECS
