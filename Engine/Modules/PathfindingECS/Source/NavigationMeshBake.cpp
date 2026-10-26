#include "NavigationMeshBake.h"

#include "NavigationSourceCollector.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/NavSurfaceFile.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <optional>

namespace GameEngine::PathfindingECS
{

struct NavigationMeshBake::TileJob
{
    Pathfinding::NavMeshSettings Settings;
    Pathfinding::NavMeshChart Chart;
    Pathfinding::NavTileKey Key;
    // The tile's terrain lattice in world metres, snapshotted on the main thread; the job adds the
    // static geometry and moves the whole input into the chart's frame.
    NavigationTriangles Input;
    std::shared_ptr<const NavigationStaticGeometry> Geometry;
    std::shared_ptr<const std::vector<NavigationCarve>> Carves;
    // The baked tile to load instead of building one, and the data that owns it; null to build.
    const Pathfinding::NavSurfaceTile* BakedTile = nullptr;
    std::shared_ptr<const Pathfinding::NavSurfaceData> BakedData;
    // The content hash the mesh's current tile was built with, when unchanged content is left out.
    std::optional<uint64> CurrentHash;

    // Written by the job before Finished is released; read by the main thread after acquiring it.
    Pathfinding::NavMeshTileData Tile;
    std::string Error;
    bool Succeeded = false;
    bool Stale = false;
    // The content equals the current tile's: nothing was built and nothing is applied.
    bool Unchanged = false;
    std::atomic<bool> Finished{false};
};

namespace
{

// The world XZ rectangle a tile's build reads: its chart-local rectangle widened by the border.
void ReadTileRectangle(const Pathfinding::NavMeshSettings& settings, const Pathfinding::NavMeshChart& chart,
                       const Pathfinding::NavTileKey& key, float32& minX, float32& minZ, float32& maxX, float32& maxZ)
{
    Pathfinding::DetourNavMap::GetTileBounds(settings, key.X, key.Z, minX, minZ, maxX, maxZ);
    const float32 border = Pathfinding::DetourNavMap::GetTileBorder(settings);
    const float32 localMin[3] = {minX - border, 0.0f, minZ - border};
    const float32 localMax[3] = {maxX + border, 0.0f, maxZ + border};
    float64 worldMin[3];
    float64 worldMax[3];
    chart.ToWorld(localMin, worldMin);
    chart.ToWorld(localMax, worldMax);
    minX = static_cast<float32>(worldMin[0]);
    minZ = static_cast<float32>(worldMin[2]);
    maxX = static_cast<float32>(worldMax[0]);
    maxZ = static_cast<float32>(worldMax[2]);
}

// Sides of the polygon a carve's cylinder is cut as. The polygon circumscribes the circle, so the cut
// covers the whole cylinder.
constexpr uint32 kCarveSides = 12;

// The carves overlapping the world rectangle as chart-local prisms; `points` holds their polygons.
void CarvesInChart(const std::vector<NavigationCarve>& carves, const Pathfinding::NavMeshChart& chart, float32 minX,
                   float32 minZ, float32 maxX, float32 maxZ, std::vector<float32>& points,
                   std::vector<Pathfinding::DetourNavMap::CarvePrism>& prisms)
{
    constexpr float32 kTwoPi = 6.28318530717958647692f;
    const float32 circumscribe = 1.0f / std::cos(kTwoPi / (2.0f * static_cast<float32>(kCarveSides)));
    std::vector<size_t> firsts;
    for (const NavigationCarve& carve : carves)
    {
        if (!carve.OverlapsXZ(minX, minZ, maxX, maxZ))
            continue;
        firsts.push_back(points.size());
        const float32 radius = carve.Radius * circumscribe;
        for (uint32 side = 0; side < kCarveSides; ++side)
        {
            const float32 angle = kTwoPi * static_cast<float32>(side) / static_cast<float32>(kCarveSides);
            float32 local[3];
            chart.ToChart(carve.X + radius * std::cos(angle), carve.MinY, carve.Z + radius * std::sin(angle), local);
            points.insert(points.end(), local, local + 3);
        }
        Pathfinding::DetourNavMap::CarvePrism& prism = prisms.emplace_back();
        prism.PointCount = kCarveSides;
        prism.MinY = static_cast<float32>(carve.MinY - chart.Origin.WorldY());
        prism.MaxY = static_cast<float32>(carve.MaxY - chart.Origin.WorldY());
    }
    // The pointers are set once every polygon is in: growing the array would move them.
    for (size_t i = 0; i < prisms.size(); ++i)
        prisms[i].Points = points.data() + firsts[i];
}

// Moves world-space vertices into the chart's frame in place.
void MoveIntoChart(const Pathfinding::NavMeshChart& chart, std::vector<float32>& vertices)
{
    for (size_t i = 0; i + 2 < vertices.size(); i += 3)
        chart.ToChart(vertices[i], vertices[i + 1], vertices[i + 2], &vertices[i]);
}

} // namespace

uint64 NavigationTileContentHash(const NavigationTriangles& lattice, const NavigationStaticGeometry& geometry,
                                 const std::vector<NavigationCarve>& carves, float32 minX, float32 minZ, float32 maxX,
                                 float32 maxZ)
{
    using Hashing::Fnv1a64;
    uint64 hash = Hashing::Fnv1a64Value(Hashing::kFnv1a64OffsetBasis, Pathfinding::kNavSurfaceFormatVersion);
    hash = Fnv1a64(lattice.Vertices.data(), lattice.Vertices.size() * sizeof(float32), hash);
    hash = Fnv1a64(lattice.Indices.data(), lattice.Indices.size() * sizeof(uint32), hash);
    hash = Hashing::Fnv1a64Value(hash, geometry.HashOverlapping(minX, minZ, maxX, maxZ));
    std::vector<uint64> carveHashes;
    for (const NavigationCarve& carve : carves)
        if (carve.OverlapsXZ(minX, minZ, maxX, maxZ))
            carveHashes.push_back(Fnv1a64(&carve, sizeof(carve)));
    std::sort(carveHashes.begin(), carveHashes.end());
    return Fnv1a64(carveHashes.data(), carveHashes.size() * sizeof(uint64), hash);
}

void NavigationMeshBake::RunTileJob(TileJob& job)
{
    try
    {
        float32 minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
        ReadTileRectangle(job.Settings, job.Chart, job.Key, minX, minZ, maxX, maxZ);
        const uint64 contentHash =
            NavigationTileContentHash(job.Input, *job.Geometry, *job.Carves, minX, minZ, maxX, maxZ);
        if (job.BakedTile)
        {
            // The baked bytes serve as they are, under the hash they were baked with.
            job.Tile.Key = job.Key;
            job.Tile.Data = job.BakedTile->Data;
            job.Tile.PolygonCount = job.BakedTile->PolygonCount;
            job.Tile.ContentHash = job.BakedTile->ContentHash;
            job.Stale = job.BakedTile->ContentHash != contentHash;
            job.Succeeded = true;
        }
        else if (job.CurrentHash == contentHash)
        {
            job.Unchanged = true;
            job.Succeeded = true;
        }
        else
        {
            job.Geometry->AppendOverlapping(minX, minZ, maxX, maxZ, job.Input);
            MoveIntoChart(job.Chart, job.Input.Vertices);
            std::vector<float32> carvePoints;
            std::vector<Pathfinding::DetourNavMap::CarvePrism> carves;
            CarvesInChart(*job.Carves, job.Chart, minX, minZ, maxX, maxZ, carvePoints, carves);

            Pathfinding::DetourNavMap::InputGeometry input;
            input.Vertices = job.Input.Vertices.data();
            input.VertexCount = job.Input.VertexCount();
            input.Indices = job.Input.Indices.data();
            input.TriangleCount = job.Input.TriangleCount();
            input.Carves = carves.data();
            input.CarveCount = static_cast<uint32>(carves.size());
            job.Succeeded = Pathfinding::DetourNavMap::BuildTile(job.Settings, job.Key, input, job.Tile, job.Error);
            job.Tile.ContentHash = contentHash;
        }
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
                                       const Pathfinding::NavMeshChart& chart, std::vector<Pathfinding::NavTileKey> tiles,
                                       std::shared_ptr<const NavigationStaticGeometry> geometry,
                                       std::shared_ptr<const std::vector<NavigationCarve>> carves, uint32 terrainStride)
    : m_Settings(settings), m_Chart(chart), m_Tiles(std::move(tiles)), m_Geometry(std::move(geometry)),
      m_Carves(std::move(carves)), m_TerrainStride(terrainStride)
{
}

NavigationMeshBake::~NavigationMeshBake() = default;

void NavigationMeshBake::SkipUnchangedTiles(const Pathfinding::DetourNavMap& mesh)
{
    std::vector<Pathfinding::NavMeshTileData> tiles;
    mesh.CopyTiles(tiles);
    for (const Pathfinding::NavMeshTileData& tile : tiles)
        m_CurrentHashes[tile.Key] = tile.ContentHash;
}

void NavigationMeshBake::UseBakedTiles(std::shared_ptr<const Pathfinding::NavSurfaceData> data)
{
    m_BakedTiles.clear();
    m_BakedData = std::move(data);
    if (!m_BakedData || m_BakedData->Profiles.empty() || m_BakedData->Profiles[0].Charts.empty())
        return;
    for (const Pathfinding::NavSurfaceTile& tile : m_BakedData->Profiles[0].Charts[0].Tiles)
        m_BakedTiles[{m_Chart.Id, tile.X, tile.Z}] = &tile;
}

bool NavigationMeshBake::SubmitTiles(const NavigationTerrainSource& terrain, JobSystem::WorkStealingThreadPool* pool,
                                     uint32 maxTiles, Mathematics::Vector3& outWaitPoint)
{
    for (uint32 submitted = 0; submitted < maxTiles && m_NextTile < GetTileCount(); ++submitted)
    {
        auto job = std::make_shared<TileJob>();
        job->Settings = m_Settings;
        job->Chart = m_Chart;
        job->Key = m_Tiles[m_NextTile];
        job->Geometry = m_Geometry;
        job->Carves = m_Carves;
        if (const auto baked = m_BakedTiles.find(job->Key); baked != m_BakedTiles.end())
        {
            job->BakedTile = baked->second;
            job->BakedData = m_BakedData;
        }
        if (const auto current = m_CurrentHashes.find(job->Key); current != m_CurrentHashes.end())
            job->CurrentHash = current->second;

        float32 minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
        ReadTileRectangle(m_Settings, m_Chart, job->Key, minX, minZ, maxX, maxZ);
        if (!AppendTerrainLattice(terrain, minX, minZ, maxX, maxZ, m_TerrainStride, job->Input, outWaitPoint))
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

bool NavigationMeshBake::ApplyFinishedTiles(Pathfinding::DetourNavMap& map,
                                            std::vector<Pathfinding::NavTileKey>& outApplied,
                                            Pathfinding::NavTileKey& outFailed, std::string& outError)
{
    bool allApplied = true;
    for (size_t i = 0; i < m_InFlight.size();)
    {
        TileJob& job = *m_InFlight[i];
        if (!job.Finished.load(std::memory_order_acquire))
        {
            ++i;
            continue;
        }
        std::string refusal;
        if (job.Succeeded && !job.Unchanged && !map.AddTile(job.Tile, &refusal))
        {
            if (job.BakedTile)
            {
                // Baked bytes the mesh refuses are stale: the tile rebakes from the ground.
                job.Stale = true;
            }
            else
            {
                job.Succeeded = false;
                job.Error = "Detour refused the tile: " + refusal;
            }
        }
        else if (job.Succeeded && !job.Unchanged)
        {
            outApplied.push_back(job.Key);
            if (!job.BakedTile)
                ++m_BuiltTiles;
        }
        if (!job.Succeeded && allApplied)
        {
            // The first failure is reported; the other finished tiles still go in.
            outFailed = job.Key;
            outError = job.Error;
            allApplied = false;
        }
        ++m_FinishedTiles;
        if (job.Stale)
            m_StaleTiles.push_back(job.Key);
        m_InFlight[i] = std::move(m_InFlight.back());
        m_InFlight.pop_back();
    }
    return allApplied;
}

bool NavigationMeshBake::IsComplete() const
{
    return m_NextTile == GetTileCount() && m_InFlight.empty();
}

} // namespace GameEngine::PathfindingECS
