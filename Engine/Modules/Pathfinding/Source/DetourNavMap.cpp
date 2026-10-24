#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/PathBuffer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

#if GE_PATHFINDING_BACKEND_DETOUR
#include <Recast.h>
#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#endif

namespace GameEngine::Pathfinding
{

#if GE_PATHFINDING_BACKEND_DETOUR

static_assert(sizeof(int) == sizeof(uint32), "Recast API expects int-sized indices");

static constexpr int32 kMaxPathPolys = 256;
static constexpr int32 kMaxStraightPathPoints = 256;
static constexpr float32 kPolySearchExtent[3] = {2.0f, 4.0f, 2.0f};
// Search nodes a query may visit: enough for a path across a full chart.
static constexpr int32 kQueryNodePool = 65535;

// The tallest cell is 0.2 m: finer tracks steps better, coarser loses a 0.4 m step to rounding.
static constexpr float32 kMaxCellHeight = 0.2f;
static constexpr float32 kCellHeightPerCellSize = 0.4f;
// Recast's demo values, in metres where they are lengths, scaled to the cell size below.
static constexpr float32 kMaxEdgeLengthMetres = 12.0f;
static constexpr float32 kMaxSimplificationError = 1.3f;
static constexpr int32 kMinRegionCells = 64;
static constexpr int32 kMergeRegionCells = 400;
static constexpr float32 kDetailSampleCells = 6.0f;
static constexpr int32 kMaxVertsPerPoly = 6;
// Cells beyond the agent radius a tile reads past its edge, so erosion near the edge sees the
// neighbouring geometry.
static constexpr int32 kTileBorderPaddingCells = 3;

namespace
{

float32 CellHeightFor(const NavMeshSettings& settings)
{
    return std::min(kMaxCellHeight, kCellHeightPerCellSize * settings.CellSize);
}

int32 WalkableRadiusCells(const NavMeshSettings& settings)
{
    return static_cast<int32>(std::ceil(settings.AgentRadius / settings.CellSize));
}

// The climb between neighbouring cells: the step, or a Max Slope slope's rise over one cell.
float32 CellClimbFor(const NavMeshSettings& settings)
{
    return WalkableClimb(settings.StepHeight, settings.MaxSlope, settings.CellSize);
}

// The climb rcFilterLedgeSpans tests: it compares the neighbours on both sides of a cell, two cells
// apart, so a slope within Max Slope rises up to twice a cell's climb across them. At the defaults
// (0.5 m cell, 0.4 m step, 45 degrees) the filter would otherwise mark any slope over 21.8 degrees
// as a ledge.
int32 LedgeClimbCells(const NavMeshSettings& settings, float32 cellHeight)
{
    const float32 climb = WalkableClimb(settings.StepHeight, settings.MaxSlope, 2.0f * settings.CellSize);
    return static_cast<int32>(std::floor(climb / cellHeight));
}

// The Recast configuration one tile is built with: appendix E of the navigation surface design
// maps the agent and the cell size to every Recast parameter.
rcConfig MakeRecastConfig(const NavMeshSettings& settings)
{
    rcConfig cfg = {};
    cfg.cs = settings.CellSize;
    cfg.ch = CellHeightFor(settings);
    cfg.walkableSlopeAngle = settings.MaxSlope;
    cfg.walkableHeight = static_cast<int32>(std::ceil(settings.AgentHeight / cfg.ch));
    cfg.walkableClimb = static_cast<int32>(std::floor(CellClimbFor(settings) / cfg.ch));
    cfg.walkableRadius = WalkableRadiusCells(settings);
    cfg.maxEdgeLen = static_cast<int32>(kMaxEdgeLengthMetres / cfg.cs);
    cfg.maxSimplificationError = kMaxSimplificationError;
    cfg.minRegionArea = kMinRegionCells;
    cfg.mergeRegionArea = kMergeRegionCells;
    cfg.maxVertsPerPoly = kMaxVertsPerPoly;
    cfg.detailSampleDist = kDetailSampleCells * cfg.cs;
    cfg.detailSampleMaxError = cfg.ch;
    cfg.tileSize = static_cast<int32>(kNavMeshTileCells);
    cfg.borderSize = cfg.walkableRadius + kTileBorderPaddingCells;
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    return cfg;
}

// Why Detour cannot hold `tile`'s bytes safely, or null when it can. Detour's addTile and its queries
// take the header's counts and every index in the data as they are, so each is checked here: the
// header, the section sizes summed in Detour's layout against the bytes, the coordinates against the
// key, and every polygon vertex, neighbour, detail and bounding-volume index against its array.
const char* RefuseTileData(const NavMeshTileData& tile)
{
    const std::vector<uint8>& data = tile.Data;
    if (data.size() < sizeof(dtMeshHeader))
        return "the tile is shorter than its header";
    dtMeshHeader header;
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.magic != DT_NAVMESH_MAGIC || header.version != DT_NAVMESH_VERSION)
        return "the tile is not Detour tile data of this version";
    if (header.x != tile.Key.X || header.y != tile.Key.Z || header.layer != 0)
        return "the tile's coordinates are not its key's";
    const int32 counts[] = {header.polyCount, header.vertCount, header.maxLinkCount, header.detailMeshCount,
                            header.detailVertCount, header.detailTriCount, header.bvNodeCount};
    for (const int32 count : counts)
        if (count < 0 || count > (1 << 20))
            return "a count in the tile's header is out of range";
    if (header.polyCount > static_cast<int32>(DetourNavMap::kMaxPolygonsPerTile) || header.maxLinkCount < 1 ||
        header.detailMeshCount != header.polyCount || header.offMeshConCount != 0 || header.offMeshBase != header.polyCount)
        return "the tile's header does not describe a tile this engine builds";

    const size_t headerSize = static_cast<size_t>(dtAlign4(sizeof(dtMeshHeader)));
    const size_t vertsSize = static_cast<size_t>(dtAlign4(sizeof(float) * 3 * header.vertCount));
    const size_t polysSize = static_cast<size_t>(dtAlign4(sizeof(dtPoly) * header.polyCount));
    const size_t linksSize = static_cast<size_t>(dtAlign4(sizeof(dtLink) * header.maxLinkCount));
    const size_t detailMeshesSize = static_cast<size_t>(dtAlign4(sizeof(dtPolyDetail) * header.detailMeshCount));
    const size_t detailVertsSize = static_cast<size_t>(dtAlign4(sizeof(float) * 3 * header.detailVertCount));
    const size_t detailTrisSize = static_cast<size_t>(dtAlign4(sizeof(unsigned char) * 4 * header.detailTriCount));
    const size_t bvTreeSize = static_cast<size_t>(dtAlign4(sizeof(dtBVNode) * header.bvNodeCount));
    if (headerSize + vertsSize + polysSize + linksSize + detailMeshesSize + detailVertsSize + detailTrisSize +
            bvTreeSize !=
        data.size())
        return "the tile's sections do not add up to its size";

    const uint8* polysAt = data.data() + headerSize + vertsSize;
    const uint8* detailMeshesAt = polysAt + polysSize + linksSize;
    const uint8* detailTrisAt = detailMeshesAt + detailMeshesSize + detailVertsSize;
    const uint8* bvTreeAt = detailTrisAt + detailTrisSize;
    for (int32 p = 0; p < header.polyCount; ++p)
    {
        dtPoly poly;
        std::memcpy(&poly, polysAt + sizeof(dtPoly) * p, sizeof(poly));
        if (poly.vertCount < 3 || poly.vertCount > DT_VERTS_PER_POLYGON || poly.getType() != DT_POLYTYPE_GROUND)
            return "a polygon of the tile is malformed";
        for (uint32 v = 0; v < poly.vertCount; ++v)
        {
            if (poly.verts[v] >= header.vertCount)
                return "a polygon of the tile names a vertex it does not have";
            const uint16 neighbour = poly.neis[v];
            if (neighbour != 0 && (neighbour & DT_EXT_LINK) == 0 && neighbour > header.polyCount)
                return "a polygon of the tile names a neighbour it does not have";
        }
        dtPolyDetail detail;
        std::memcpy(&detail, detailMeshesAt + sizeof(dtPolyDetail) * p, sizeof(detail));
        if (static_cast<uint64>(detail.vertBase) + detail.vertCount > static_cast<uint64>(header.detailVertCount) ||
            static_cast<uint64>(detail.triBase) + detail.triCount > static_cast<uint64>(header.detailTriCount))
            return "a detail mesh of the tile lies outside its arrays";
        for (uint32 t = 0; t < detail.triCount; ++t)
        {
            const uint8* triangle = detailTrisAt + 4 * (static_cast<size_t>(detail.triBase) + t);
            for (int32 corner = 0; corner < 3; ++corner)
                if (triangle[corner] >= poly.vertCount + detail.vertCount)
                    return "a detail triangle of the tile names a vertex it does not have";
        }
    }
    for (int32 n = 0; n < header.bvNodeCount; ++n)
    {
        dtBVNode node;
        std::memcpy(&node, bvTreeAt + sizeof(dtBVNode) * n, sizeof(node));
        if (node.i >= header.polyCount || node.i < -header.bvNodeCount)
            return "a bounding-volume node of the tile names a polygon it does not have";
    }
    return nullptr;
}

// Owns the Recast intermediates of one tile build and frees them on every exit path.
struct TileBuildScratch
{
    rcHeightfield* Heightfield = nullptr;
    rcCompactHeightfield* Compact = nullptr;
    rcContourSet* Contours = nullptr;
    rcPolyMesh* PolyMesh = nullptr;
    rcPolyMeshDetail* DetailMesh = nullptr;

    ~TileBuildScratch()
    {
        rcFreeHeightField(Heightfield);
        rcFreeCompactHeightfield(Compact);
        rcFreeContourSet(Contours);
        rcFreePolyMesh(PolyMesh);
        rcFreePolyMeshDetail(DetailMesh);
    }
};

} // namespace

struct DetourNavMap::Impl
{
    NavMeshSettings Settings;
    NavMeshChart Chart;
    dtNavMesh* NavMesh = nullptr;
    dtNavMeshQuery* NavQuery = nullptr;
    dtQueryFilter Filter;
    // Pool slots no tile holds, freed longest ago first: AddTile takes the front and RemoveTile
    // appends, so a slot (and the salt of its references) is reused as late as the pool allows.
    std::deque<int32> FreeSlots;
    // Each held tile as it was added. Detour links a tile in its own copy of the bytes, so these
    // are the bytes a file keeps: the same for the same build whatever the mesh around it.
    std::unordered_map<NavTileKey, NavMeshTileData, NavTileKeyHash> Tiles;

    // Rebuilt from the tiles on first access after AddTile or RemoveTile changed them.
    mutable std::vector<float32> DebugVertices;
    mutable std::vector<uint32> DebugIndices;
    mutable std::vector<uint32> DebugBoundaryIndices;
    mutable bool DebugMeshDirty = true;
    mutable uint64 DebugMeshVersion = 0;

    ~Impl()
    {
        if (NavQuery)
        {
            dtFreeNavMeshQuery(NavQuery);
            NavQuery = nullptr;
        }
        if (NavMesh)
        {
            dtFreeNavMesh(NavMesh);
            NavMesh = nullptr;
        }
    }

    bool CreateQuery()
    {
        NavQuery = dtAllocNavMeshQuery();
        if (!NavQuery)
            return false;
        if (dtStatusFailed(NavQuery->init(NavMesh, kQueryNodePool)))
        {
            dtFreeNavMeshQuery(NavQuery);
            NavQuery = nullptr;
            return false;
        }
        return true;
    }

    void ToLocal(float32 x, float32 y, float32 z, float32 out[3]) const
    {
        Chart.ToChart(x, y, z, out);
    }

    void ToWorld(const float32 local[3], float32& outX, float32& outY, float32& outZ) const
    {
        float64 world[3];
        Chart.ToWorld(local, world);
        outX = static_cast<float32>(world[0]);
        outY = static_cast<float32>(world[1]);
        outZ = static_cast<float32>(world[2]);
    }

    // An edge with no polygon across it: no neighbour inside the tile, and a tile-border edge whose
    // neighbouring tile linked no polygon to it.
    static bool IsBoundaryEdge(const dtMeshTile& tile, const dtPoly& poly, int32 edge)
    {
        const uint16 neighbour = poly.neis[edge];
        if (neighbour == 0)
            return true;
        if ((neighbour & DT_EXT_LINK) == 0)
            return false;
        for (uint32 link = poly.firstLink; link != DT_NULL_LINK; link = tile.links[link].next)
            if (tile.links[link].edge == edge)
                return false;
        return true;
    }

    void RefreshDebugMesh() const
    {
        if (!DebugMeshDirty)
            return;
        DebugMeshDirty = false;
        static std::atomic<uint64> s_DebugMeshVersions{0};
        DebugMeshVersion = ++s_DebugMeshVersions;
        DebugVertices.clear();
        DebugIndices.clear();
        DebugBoundaryIndices.clear();

        if (!NavMesh)
            return;

        // Extract debug geometry from the navmesh (use const pointer to
        // select the public const overload of getTile).
        const dtNavMesh* navMeshConst = NavMesh;
        for (int32 i = 0; i < navMeshConst->getMaxTiles(); ++i)
        {
            const dtMeshTile* tile = navMeshConst->getTile(i);
            if (!tile || !tile->header)
                continue;

            const uint32 baseVertex = static_cast<uint32>(DebugVertices.size() / 3);

            for (int32 v = 0; v < tile->header->vertCount; ++v)
            {
                float32 world[3];
                ToWorld(&tile->verts[v * 3], world[0], world[1], world[2]);
                DebugVertices.insert(DebugVertices.end(), world, world + 3);
            }

            for (int32 p = 0; p < tile->header->polyCount; ++p)
            {
                const dtPoly& poly = tile->polys[p];
                if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                    continue;

                // Triangulate the polygon as a fan
                for (int32 t = 2; t < static_cast<int32>(poly.vertCount); ++t)
                {
                    DebugIndices.push_back(baseVertex + poly.verts[0]);
                    DebugIndices.push_back(baseVertex + poly.verts[t - 1]);
                    DebugIndices.push_back(baseVertex + poly.verts[t]);
                }

                for (int32 edge = 0; edge < static_cast<int32>(poly.vertCount); ++edge)
                {
                    if (!IsBoundaryEdge(*tile, poly, edge))
                        continue;
                    DebugBoundaryIndices.push_back(baseVertex + poly.verts[edge]);
                    DebugBoundaryIndices.push_back(baseVertex + poly.verts[(edge + 1) % poly.vertCount]);
                }
            }
        }
    }
};

DetourNavMap::DetourNavMap()
    : m_Impl(std::make_unique<Impl>())
{
}

DetourNavMap::~DetourNavMap() = default;
DetourNavMap::DetourNavMap(DetourNavMap&&) noexcept = default;
DetourNavMap& DetourNavMap::operator=(DetourNavMap&&) noexcept = default;

bool DetourNavMap::Initialize(const NavMeshSettings& settings, const NavMeshChart& chart)
{
    m_Impl = std::make_unique<Impl>();
    if (!(settings.CellSize > 0.0f) || !chart.HasIdentityRotation())
        return false;

    // The origin is zero: tiles are built and kept in the chart's frame.
    dtNavMeshParams params = {};
    params.tileWidth = GetTileSize(settings);
    params.tileHeight = params.tileWidth;
    params.maxTiles = static_cast<int32>(kMaxTiles);
    params.maxPolys = static_cast<int32>(kMaxPolygonsPerTile);

    m_Impl->NavMesh = dtAllocNavMesh();
    if (!m_Impl->NavMesh)
        return false;
    if (dtStatusFailed(m_Impl->NavMesh->init(&params)) || !m_Impl->CreateQuery())
    {
        m_Impl = std::make_unique<Impl>();
        return false;
    }
    m_Impl->Settings = settings;
    m_Impl->Chart = chart;
    for (int32 slot = 0; slot < static_cast<int32>(kMaxTiles); ++slot)
        m_Impl->FreeSlots.push_back(slot);
    return true;
}

bool DetourNavMap::IsBuilt() const
{
    return m_Impl && m_Impl->NavMesh != nullptr && m_Impl->NavQuery != nullptr;
}

const NavMeshSettings& DetourNavMap::GetSettings() const
{
    return m_Impl->Settings;
}

const NavMeshChart& DetourNavMap::GetChart() const
{
    return m_Impl->Chart;
}

float32 DetourNavMap::GetTileSize(const NavMeshSettings& settings)
{
    return static_cast<float32>(kNavMeshTileCells) * settings.CellSize;
}

void DetourNavMap::GetTileBounds(const NavMeshSettings& settings, int32 tileX, int32 tileZ, float32& outMinX,
                                 float32& outMinZ, float32& outMaxX, float32& outMaxZ)
{
    const float32 tileSize = GetTileSize(settings);
    outMinX = static_cast<float32>(tileX) * tileSize;
    outMinZ = static_cast<float32>(tileZ) * tileSize;
    outMaxX = outMinX + tileSize;
    outMaxZ = outMinZ + tileSize;
}

float32 DetourNavMap::GetTileBorder(const NavMeshSettings& settings)
{
    return static_cast<float32>(WalkableRadiusCells(settings) + kTileBorderPaddingCells) * settings.CellSize;
}

bool DetourNavMap::BuildTile(const NavMeshSettings& settings, const NavTileKey& key, const InputGeometry& geometry,
                             NavMeshTileData& outTile, std::string& outError)
{
    outTile = {};
    outTile.Key = key;
    if (!geometry.Vertices || geometry.VertexCount == 0 || !geometry.Indices || geometry.TriangleCount == 0)
        return true;

    rcConfig cfg = MakeRecastConfig(settings);
    float32 geometryMin[3];
    float32 geometryMax[3];
    rcCalcBounds(geometry.Vertices, static_cast<int32>(geometry.VertexCount), geometryMin, geometryMax);
    float32 minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
    GetTileBounds(settings, key.X, key.Z, minX, minZ, maxX, maxZ);
    const float32 border = static_cast<float32>(cfg.borderSize) * cfg.cs;
    cfg.bmin[0] = minX - border;
    cfg.bmin[1] = geometryMin[1];
    cfg.bmin[2] = minZ - border;
    cfg.bmax[0] = maxX + border;
    cfg.bmax[1] = geometryMax[1];
    cfg.bmax[2] = maxZ + border;

    rcContext ctx(false);
    TileBuildScratch scratch;
    scratch.Heightfield = rcAllocHeightfield();
    if (!scratch.Heightfield ||
        !rcCreateHeightfield(&ctx, *scratch.Heightfield, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
    {
        outError = "Recast could not allocate the tile's heightfield";
        return false;
    }

    const int32 triCount = static_cast<int32>(geometry.TriangleCount);
    const auto* indices = reinterpret_cast<const int32*>(geometry.Indices);
    std::vector<uint8> triAreas(static_cast<size_t>(triCount), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, geometry.Vertices, static_cast<int32>(geometry.VertexCount),
                            indices, triCount, triAreas.data());
    if (!rcRasterizeTriangles(&ctx, geometry.Vertices, static_cast<int32>(geometry.VertexCount), indices,
                              triAreas.data(), triCount, *scratch.Heightfield, cfg.walkableClimb))
    {
        outError = "Recast could not rasterize the tile's triangles";
        return false;
    }

    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *scratch.Heightfield);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, LedgeClimbCells(settings, cfg.ch), *scratch.Heightfield);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *scratch.Heightfield);

    scratch.Compact = rcAllocCompactHeightfield();
    if (!scratch.Compact ||
        !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *scratch.Heightfield,
                                   *scratch.Compact))
    {
        outError = "Recast could not compact the tile's heightfield";
        return false;
    }
    rcFreeHeightField(scratch.Heightfield);
    scratch.Heightfield = nullptr;

    // Carves are cut before erosion, so the agent keeps its radius from them as from a wall.
    for (uint32 i = 0; i < geometry.CarveCount; ++i)
    {
        const CarvePrism& carve = geometry.Carves[i];
        rcMarkConvexPolyArea(&ctx, carve.Points, static_cast<int32>(carve.PointCount), carve.MinY, carve.MaxY,
                             RC_NULL_AREA, *scratch.Compact);
    }

    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *scratch.Compact) ||
        !rcBuildDistanceField(&ctx, *scratch.Compact) ||
        !rcBuildRegions(&ctx, *scratch.Compact, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
    {
        outError = "Recast could not partition the tile into regions";
        return false;
    }

    scratch.Contours = rcAllocContourSet();
    if (!scratch.Contours ||
        !rcBuildContours(&ctx, *scratch.Compact, cfg.maxSimplificationError, cfg.maxEdgeLen, *scratch.Contours))
    {
        outError = "Recast could not trace the tile's contours";
        return false;
    }

    scratch.PolyMesh = rcAllocPolyMesh();
    if (!scratch.PolyMesh || !rcBuildPolyMesh(&ctx, *scratch.Contours, cfg.maxVertsPerPoly, *scratch.PolyMesh))
    {
        outError = "Recast could not build the tile's polygons";
        return false;
    }
    scratch.DetailMesh = rcAllocPolyMeshDetail();
    if (!scratch.DetailMesh || !rcBuildPolyMeshDetail(&ctx, *scratch.PolyMesh, *scratch.Compact,
                                                     cfg.detailSampleDist, cfg.detailSampleMaxError,
                                                     *scratch.DetailMesh))
    {
        outError = "Recast could not build the tile's detail mesh";
        return false;
    }

    rcPolyMesh& polyMesh = *scratch.PolyMesh;
    if (polyMesh.npolys == 0)
        return true;
    if (static_cast<uint32>(polyMesh.npolys) > kMaxPolygonsPerTile)
    {
        outError = "the tile has " + std::to_string(polyMesh.npolys) + " polygons and a tile holds " +
                   std::to_string(kMaxPolygonsPerTile) + ": raise Cell Size";
        return false;
    }
    for (int32 i = 0; i < polyMesh.npolys; ++i)
        polyMesh.flags[i] = 1;

    dtNavMeshCreateParams params = {};
    params.verts = polyMesh.verts;
    params.vertCount = polyMesh.nverts;
    params.polys = polyMesh.polys;
    params.polyAreas = polyMesh.areas;
    params.polyFlags = polyMesh.flags;
    params.polyCount = polyMesh.npolys;
    params.nvp = polyMesh.nvp;
    params.detailMeshes = scratch.DetailMesh->meshes;
    params.detailVerts = scratch.DetailMesh->verts;
    params.detailVertsCount = scratch.DetailMesh->nverts;
    params.detailTris = scratch.DetailMesh->tris;
    params.detailTriCount = scratch.DetailMesh->ntris;
    params.walkableHeight = settings.AgentHeight;
    params.walkableRadius = settings.AgentRadius;
    params.walkableClimb = CellClimbFor(settings);
    params.tileX = key.X;
    params.tileY = key.Z;
    params.tileLayer = 0;
    rcVcopy(params.bmin, polyMesh.bmin);
    rcVcopy(params.bmax, polyMesh.bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    uint8* navData = nullptr;
    int32 navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize))
    {
        outError = "Detour refused the tile's polygons";
        return false;
    }
    outTile.Data.assign(navData, navData + navDataSize);
    outTile.PolygonCount = static_cast<uint32>(polyMesh.npolys);
    dtFree(navData);
    return true;
}

bool DetourNavMap::AddTile(const NavMeshTileData& tile, std::string* outRefusal)
{
    if (!IsBuilt())
        return false;
    if (!tile.Data.empty())
    {
        const char* refusal = RefuseTileData(tile);
        if (refusal)
        {
            if (outRefusal)
                *outRefusal = refusal;
            return false;
        }
    }
    RemoveTile(tile.Key);
    if (tile.Data.empty())
    {
        // No walkable area: Detour holds nothing, the map keeps the key and the content hash.
        m_Impl->Tiles[tile.Key] = tile;
        return true;
    }
    if (m_Impl->FreeSlots.empty())
        return false;

    // Detour owns and frees the data it is given (DT_TILE_FREE_DATA), so it gets its own copy.
    auto* data = static_cast<uint8*>(dtAlloc(static_cast<int32>(tile.Data.size()), DT_ALLOC_PERM));
    if (!data)
        return false;
    std::memcpy(data, tile.Data.data(), tile.Data.size());
    // A tile reference naming the slot and its current salt makes Detour place the tile there.
    dtNavMesh& navMesh = *m_Impl->NavMesh;
    const int32 slot = m_Impl->FreeSlots.front();
    const dtMeshTile* slotTile = static_cast<const dtNavMesh&>(navMesh).getTile(slot);
    const dtTileRef slotRef = navMesh.encodePolyId(slotTile->salt, static_cast<uint32>(slot), 0);
    if (dtStatusFailed(
            navMesh.addTile(data, static_cast<int32>(tile.Data.size()), DT_TILE_FREE_DATA, slotRef, nullptr)))
    {
        dtFree(data);
        return false;
    }
    m_Impl->FreeSlots.pop_front();
    m_Impl->Tiles[tile.Key] = tile;
    m_Impl->DebugMeshDirty = true;
    return true;
}

bool DetourNavMap::RemoveTile(const NavTileKey& key)
{
    if (!IsBuilt())
        return false;
    dtNavMesh& navMesh = *m_Impl->NavMesh;
    const dtTileRef ref = navMesh.getTileRefAt(key.X, key.Z, 0);
    if (ref)
    {
        if (dtStatusFailed(navMesh.removeTile(ref, nullptr, nullptr)))
            return false;
        m_Impl->FreeSlots.push_back(static_cast<int32>(navMesh.decodePolyIdTile(ref)));
        m_Impl->DebugMeshDirty = true;
    }
    return m_Impl->Tiles.erase(key) > 0 || ref != 0;
}

uint32 DetourNavMap::GetTileCount() const
{
    if (!IsBuilt())
        return 0;
    const dtNavMesh* navMesh = m_Impl->NavMesh;
    uint32 count = 0;
    for (int32 i = 0; i < navMesh->getMaxTiles(); ++i)
    {
        const dtMeshTile* tile = navMesh->getTile(i);
        if (tile && tile->header)
            ++count;
    }
    return count;
}

uint32 DetourNavMap::GetPolygonCount() const
{
    if (!IsBuilt())
        return 0;
    const dtNavMesh* navMesh = m_Impl->NavMesh;
    uint32 count = 0;
    for (int32 i = 0; i < navMesh->getMaxTiles(); ++i)
    {
        const dtMeshTile* tile = navMesh->getTile(i);
        if (tile && tile->header)
            count += static_cast<uint32>(tile->header->polyCount);
    }
    return count;
}

uint64 DetourNavMap::GetTileDataBytes() const
{
    if (!IsBuilt())
        return 0;
    const dtNavMesh* navMesh = m_Impl->NavMesh;
    uint64 bytes = 0;
    for (int32 i = 0; i < navMesh->getMaxTiles(); ++i)
    {
        const dtMeshTile* tile = navMesh->getTile(i);
        if (tile && tile->header)
            bytes += static_cast<uint64>(tile->dataSize);
    }
    return bytes;
}

void DetourNavMap::CopyTiles(std::vector<NavMeshTileData>& out) const
{
    out.clear();
    if (!IsBuilt())
        return;
    out.reserve(m_Impl->Tiles.size());
    for (const auto& [key, tile] : m_Impl->Tiles)
        out.push_back(tile);
}

PathStatus DetourNavMap::FindPath(const PathRequest& request, PathBuffer& pathBuffer, PathHandle& outPath) const
{
    if (!IsBuilt())
        return PathStatus::Failed;

    float32 startPos[3];
    float32 endPos[3];
    m_Impl->ToLocal(request.StartX, request.StartY, request.StartZ, startPos);
    m_Impl->ToLocal(request.EndX, request.EndY, request.EndZ, endPos);
    const float32 extent[3] = {request.AgentRadius * 4.0f, request.AgentHeight, request.AgentRadius * 4.0f};

    dtPolyRef startRef = 0, endRef = 0;
    float32 nearestStartPos[3], nearestEndPos[3];

    dtStatus status = m_Impl->NavQuery->findNearestPoly(startPos, extent, &m_Impl->Filter,
                                                         &startRef, nearestStartPos);
    if (dtStatusFailed(status) || startRef == 0)
        return PathStatus::Failed;

    status = m_Impl->NavQuery->findNearestPoly(endPos, extent, &m_Impl->Filter,
                                                &endRef, nearestEndPos);
    if (dtStatusFailed(status) || endRef == 0)
        return PathStatus::Failed;

    dtPolyRef pathPolys[kMaxPathPolys];
    int32 pathPolyCount = 0;
    const dtStatus findPathStatus = m_Impl->NavQuery->findPath(
        startRef, endRef, nearestStartPos, nearestEndPos,
        &m_Impl->Filter, pathPolys, &pathPolyCount, kMaxPathPolys);
    if (dtStatusFailed(findPathStatus) || pathPolyCount == 0)
        return PathStatus::Failed;

    float32 straightPath[kMaxStraightPathPoints * 3];
    uint8 straightPathFlags[kMaxStraightPathPoints];
    dtPolyRef straightPathPolys[kMaxStraightPathPoints];
    int32 straightPathCount = 0;

    status = m_Impl->NavQuery->findStraightPath(nearestStartPos, nearestEndPos,
                                                  pathPolys, pathPolyCount,
                                                  straightPath, straightPathFlags,
                                                  straightPathPolys, &straightPathCount,
                                                  kMaxStraightPathPoints);
    if (dtStatusFailed(status) || straightPathCount == 0)
        return PathStatus::Failed;

    std::vector<PathPoint> points(straightPathCount);
    for (int32 i = 0; i < straightPathCount; ++i)
        m_Impl->ToWorld(&straightPath[i * 3], points[i].X, points[i].Y, points[i].Z);

    outPath = pathBuffer.AllocatePath(points.data(), static_cast<uint32>(points.size()));

    // Check the findPath status (not findStraightPath) for partial results.
    bool isPartial = dtStatusDetail(findPathStatus, DT_PARTIAL_RESULT);
    return isPartial ? PathStatus::Partial : PathStatus::Complete;
}

bool DetourNavMap::IsPointNavigable(float32 x, float32 y, float32 z, float32 radius) const
{
    if (!IsBuilt())
        return false;

    float32 pos[3];
    m_Impl->ToLocal(x, y, z, pos);
    const float32 extent[3] = {radius, radius * 2.0f, radius};

    dtPolyRef ref = 0;
    float32 nearestPos[3];
    dtStatus status = m_Impl->NavQuery->findNearestPoly(pos, extent, &m_Impl->Filter, &ref, nearestPos);
    if (dtStatusFailed(status) || ref == 0)
        return false;

    // Check XZ distance to nearest navigable point (height is checked separately
    // with a more generous tolerance since navmesh Y can differ from query Y due
    // to terrain slope within a polygon).
    const float32 dx = nearestPos[0] - pos[0];
    const float32 dz = nearestPos[2] - pos[2];
    const float32 distXZSq = dx * dx + dz * dz;
    const float32 dy = std::abs(nearestPos[1] - pos[1]);
    return distXZSq <= radius * radius && dy <= radius * 2.0f;
}

bool DetourNavMap::GetClosestNavigablePoint(float32 x, float32 y, float32 z, float32 searchRadius,
                                             float32& outX, float32& outY, float32& outZ) const
{
    if (!IsBuilt())
        return false;

    float32 pos[3];
    m_Impl->ToLocal(x, y, z, pos);
    const float32 extent[3] = {searchRadius, searchRadius, searchRadius};

    dtPolyRef ref = 0;
    float32 nearestPos[3];
    dtStatus status = m_Impl->NavQuery->findNearestPoly(pos, extent, &m_Impl->Filter, &ref, nearestPos);
    if (dtStatusFailed(status) || ref == 0)
        return false;

    // Reject results outside the requested search radius in the XZ plane
    // (Detour's extent is an AABB, so corner results can exceed the radius).
    const float32 dx = nearestPos[0] - pos[0];
    const float32 dz = nearestPos[2] - pos[2];
    if (dx * dx + dz * dz > searchRadius * searchRadius)
        return false;

    m_Impl->ToWorld(nearestPos, outX, outY, outZ);
    return true;
}

// NOTE: The start position is snapped to the nearest point on the navmesh via
// findNearestPoly before the raycast is performed. This means the actual ray
// origin may differ from (startX, startY, startZ) if the start is off-mesh.
// The returned hit point is along the snapped ray, not the original ray.
// Callers that need the exact ray origin should verify the start is on the
// navmesh (e.g., via IsPointNavigable) before calling Raycast.
bool DetourNavMap::Raycast(float32 startX, float32 startY, float32 startZ,
                            float32 endX, float32 endY, float32 endZ,
                            float32& hitX, float32& hitY, float32& hitZ) const
{
    if (!IsBuilt())
        return false;

    float32 startPos[3];
    float32 endPos[3];
    m_Impl->ToLocal(startX, startY, startZ, startPos);
    m_Impl->ToLocal(endX, endY, endZ, endPos);

    dtPolyRef startRef = 0;
    float32 nearestPos[3];
    dtStatus status = m_Impl->NavQuery->findNearestPoly(startPos, kPolySearchExtent,
                                                         &m_Impl->Filter, &startRef, nearestPos);
    if (dtStatusFailed(status) || startRef == 0)
        return false;

    float32 t = 0.0f;
    float32 hitNormal[3];
    dtPolyRef pathPolys[kMaxPathPolys];
    int32 pathPolyCount = 0;

    status = m_Impl->NavQuery->raycast(startRef, nearestPos, endPos, &m_Impl->Filter,
                                        &t, hitNormal, pathPolys, &pathPolyCount, kMaxPathPolys);
    if (dtStatusFailed(status))
        return false;

    if (t >= 1.0f)
        return false; // No hit, ray reached end

    const float32 hit[3] = {nearestPos[0] + (endPos[0] - nearestPos[0]) * t,
                            nearestPos[1] + (endPos[1] - nearestPos[1]) * t,
                            nearestPos[2] + (endPos[2] - nearestPos[2]) * t};
    m_Impl->ToWorld(hit, hitX, hitY, hitZ);
    return true;
}

uint32 DetourNavMap::GetDebugVertexCount() const
{
    m_Impl->RefreshDebugMesh();
    return static_cast<uint32>(m_Impl->DebugVertices.size() / 3);
}

uint32 DetourNavMap::GetDebugTriangleCount() const
{
    m_Impl->RefreshDebugMesh();
    return static_cast<uint32>(m_Impl->DebugIndices.size() / 3);
}

const float32* DetourNavMap::GetDebugVertices() const
{
    m_Impl->RefreshDebugMesh();
    return m_Impl->DebugVertices.empty() ? nullptr : m_Impl->DebugVertices.data();
}

uint32 DetourNavMap::GetDebugBoundaryEdgeCount() const
{
    m_Impl->RefreshDebugMesh();
    return static_cast<uint32>(m_Impl->DebugBoundaryIndices.size() / 2);
}

const uint32* DetourNavMap::GetDebugBoundaryIndices() const
{
    m_Impl->RefreshDebugMesh();
    return m_Impl->DebugBoundaryIndices.empty() ? nullptr : m_Impl->DebugBoundaryIndices.data();
}

uint64 DetourNavMap::GetDebugMeshVersion() const
{
    m_Impl->RefreshDebugMesh();
    return m_Impl->DebugMeshVersion;
}

const uint32* DetourNavMap::GetDebugIndices() const
{
    m_Impl->RefreshDebugMesh();
    return m_Impl->DebugIndices.empty() ? nullptr : m_Impl->DebugIndices.data();
}

void* DetourNavMap::GetDetourNavMesh() const
{
    return m_Impl ? m_Impl->NavMesh : nullptr;
}

void* DetourNavMap::GetDetourNavMeshQuery() const
{
    return m_Impl ? m_Impl->NavQuery : nullptr;
}

#else // !GE_PATHFINDING_BACKEND_DETOUR

struct DetourNavMap::Impl
{
};

DetourNavMap::DetourNavMap()
    : m_Impl(std::make_unique<Impl>())
{
}

DetourNavMap::~DetourNavMap() = default;
DetourNavMap::DetourNavMap(DetourNavMap&&) noexcept = default;
DetourNavMap& DetourNavMap::operator=(DetourNavMap&&) noexcept = default;

bool DetourNavMap::Initialize(const NavMeshSettings& /*settings*/, const NavMeshChart& /*chart*/)
{
    return false;
}

bool DetourNavMap::IsBuilt() const
{
    return false;
}

const NavMeshSettings& DetourNavMap::GetSettings() const
{
    static const NavMeshSettings kNone;
    return kNone;
}

const NavMeshChart& DetourNavMap::GetChart() const
{
    static const NavMeshChart kNone;
    return kNone;
}

float32 DetourNavMap::GetTileSize(const NavMeshSettings& settings)
{
    return static_cast<float32>(kNavMeshTileCells) * settings.CellSize;
}

void DetourNavMap::GetTileBounds(const NavMeshSettings& settings, int32 tileX, int32 tileZ, float32& outMinX,
                                 float32& outMinZ, float32& outMaxX, float32& outMaxZ)
{
    const float32 tileSize = GetTileSize(settings);
    outMinX = static_cast<float32>(tileX) * tileSize;
    outMinZ = static_cast<float32>(tileZ) * tileSize;
    outMaxX = outMinX + tileSize;
    outMaxZ = outMinZ + tileSize;
}

float32 DetourNavMap::GetTileBorder(const NavMeshSettings& /*settings*/)
{
    return 0.0f;
}

bool DetourNavMap::BuildTile(const NavMeshSettings& /*settings*/, const NavTileKey& /*key*/,
                             const InputGeometry& /*geometry*/, NavMeshTileData& /*outTile*/, std::string& outError)
{
    outError = "the engine was built without the Recast/Detour backend";
    return false;
}

bool DetourNavMap::AddTile(const NavMeshTileData& /*tile*/, std::string* /*outRefusal*/)
{
    return false;
}

bool DetourNavMap::RemoveTile(const NavTileKey& /*key*/)
{
    return false;
}

uint32 DetourNavMap::GetTileCount() const
{
    return 0;
}

uint32 DetourNavMap::GetPolygonCount() const
{
    return 0;
}

uint64 DetourNavMap::GetTileDataBytes() const
{
    return 0;
}

void DetourNavMap::CopyTiles(std::vector<NavMeshTileData>& out) const
{
    out.clear();
}

PathStatus DetourNavMap::FindPath(const PathRequest& /*request*/, PathBuffer& /*pathBuffer*/, PathHandle& /*outPath*/) const
{
    return PathStatus::Failed;
}

bool DetourNavMap::IsPointNavigable(float32 /*x*/, float32 /*y*/, float32 /*z*/, float32 /*radius*/) const
{
    return false;
}

bool DetourNavMap::GetClosestNavigablePoint(float32 /*x*/, float32 /*y*/, float32 /*z*/, float32 /*searchRadius*/,
                                             float32& /*outX*/, float32& /*outY*/, float32& /*outZ*/) const
{
    return false;
}

bool DetourNavMap::Raycast(float32 /*startX*/, float32 /*startY*/, float32 /*startZ*/,
                            float32 /*endX*/, float32 /*endY*/, float32 /*endZ*/,
                            float32& /*hitX*/, float32& /*hitY*/, float32& /*hitZ*/) const
{
    return false;
}

uint32 DetourNavMap::GetDebugVertexCount() const
{
    return 0;
}

uint32 DetourNavMap::GetDebugTriangleCount() const
{
    return 0;
}

const float32* DetourNavMap::GetDebugVertices() const
{
    return nullptr;
}

const uint32* DetourNavMap::GetDebugIndices() const
{
    return nullptr;
}

uint64 DetourNavMap::GetDebugMeshVersion() const
{
    return 0;
}

uint32 DetourNavMap::GetDebugBoundaryEdgeCount() const
{
    return 0;
}

const uint32* DetourNavMap::GetDebugBoundaryIndices() const
{
    return nullptr;
}

void* DetourNavMap::GetDetourNavMesh() const
{
    return nullptr;
}

void* DetourNavMap::GetDetourNavMeshQuery() const
{
    return nullptr;
}

#endif // GE_PATHFINDING_BACKEND_DETOUR

} // namespace GameEngine::Pathfinding
