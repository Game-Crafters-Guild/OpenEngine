#include "NavigationMeshBake.h"

#include "NavigationSourceCollector.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Pathfinding/DetourNavMap.h"

#include <atomic>
#include <exception>

namespace GameEngine::PathfindingECS
{

struct NavigationMeshBake::TileJob
{
    Pathfinding::NavMeshSettings Settings;
    Pathfinding::NavMeshTileLayout Layout;
    uint32 TileX = 0;
    uint32 TileZ = 0;
    // The tile's terrain lattice, snapshotted on the main thread; the job adds the static geometry.
    NavigationTriangles Input;
    std::shared_ptr<const NavigationStaticGeometry> Geometry;

    // Written by the job before Finished is released; read by the main thread after acquiring it.
    Pathfinding::NavMeshTileData Tile;
    std::string Error;
    bool Succeeded = false;
    std::atomic<bool> Finished{false};
};

namespace
{

void ReadTileRectangle(const Pathfinding::NavMeshSettings& settings, const Pathfinding::NavMeshTileLayout& layout,
                       uint32 tileX, uint32 tileZ, float32& minX, float32& minZ, float32& maxX, float32& maxZ)
{
    Pathfinding::DetourNavMap::GetTileBounds(settings, layout, tileX, tileZ, minX, minZ, maxX, maxZ);
    const float32 border = Pathfinding::DetourNavMap::GetTileBorder(settings);
    minX -= border;
    minZ -= border;
    maxX += border;
    maxZ += border;
}

} // namespace

void NavigationMeshBake::RunTileJob(TileJob& job)
{
    try
    {
        float32 minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
        ReadTileRectangle(job.Settings, job.Layout, job.TileX, job.TileZ, minX, minZ, maxX, maxZ);
        job.Geometry->AppendOverlapping(minX, minZ, maxX, maxZ, job.Input);

        Pathfinding::DetourNavMap::InputGeometry input;
        input.Vertices = job.Input.Vertices.data();
        input.VertexCount = job.Input.VertexCount();
        input.Indices = job.Input.Indices.data();
        input.TriangleCount = job.Input.TriangleCount();
        job.Succeeded = Pathfinding::DetourNavMap::BuildTile(job.Settings, job.Layout, job.TileX, job.TileZ, input,
                                                             job.Tile, job.Error);
    }
    catch (const std::exception& e)
    {
        job.Succeeded = false;
        job.Error = e.what();
    }
    job.Input = {};
    job.Finished.store(true, std::memory_order_release);
}

NavigationMeshBake::NavigationMeshBake(const Pathfinding::NavMeshSettings& settings,
                                       const Pathfinding::NavMeshTileLayout& layout,
                                       std::shared_ptr<const NavigationStaticGeometry> geometry, uint32 terrainStride)
    : m_Settings(settings), m_Layout(layout), m_Geometry(std::move(geometry)), m_TerrainStride(terrainStride)
{
}

NavigationMeshBake::~NavigationMeshBake() = default;

bool NavigationMeshBake::SubmitTiles(const NavigationTerrainSource& terrain, JobSystem::WorkStealingThreadPool* pool,
                                     uint32 maxTiles, int32& outTerrainTileX, int32& outTerrainTileZ)
{
    for (uint32 submitted = 0; submitted < maxTiles && m_NextTile < GetTileCount(); ++submitted)
    {
        auto job = std::make_shared<TileJob>();
        job->Settings = m_Settings;
        job->Layout = m_Layout;
        job->TileX = m_NextTile % m_Layout.TilesX;
        job->TileZ = m_NextTile / m_Layout.TilesX;
        job->Geometry = m_Geometry;

        float32 minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
        ReadTileRectangle(m_Settings, m_Layout, job->TileX, job->TileZ, minX, minZ, maxX, maxZ);
        if (!AppendTerrainLattice(terrain, minX, minZ, maxX, maxZ, m_TerrainStride, job->Input, outTerrainTileX,
                                  outTerrainTileZ))
            return false;

        ++m_NextTile;
        m_InFlight.push_back(job);
        if (pool)
            pool->Submit([job]() { RunTileJob(*job); });
        else
            RunTileJob(*job);
    }
    return true;
}

bool NavigationMeshBake::ApplyFinishedTiles(Pathfinding::DetourNavMap& map, uint32& outTileX, uint32& outTileZ,
                                            std::string& outError)
{
    for (size_t i = 0; i < m_InFlight.size();)
    {
        TileJob& job = *m_InFlight[i];
        if (!job.Finished.load(std::memory_order_acquire))
        {
            ++i;
            continue;
        }
        if (!job.Succeeded || !map.AddTile(job.Tile))
        {
            outTileX = job.TileX;
            outTileZ = job.TileZ;
            outError = job.Succeeded ? "Detour refused the tile's data" : job.Error;
            return false;
        }
        ++m_AppliedTiles;
        m_InFlight[i] = std::move(m_InFlight.back());
        m_InFlight.pop_back();
    }
    return true;
}

bool NavigationMeshBake::IsComplete() const
{
    return m_NextTile == GetTileCount() && m_InFlight.empty();
}

} // namespace GameEngine::PathfindingECS
