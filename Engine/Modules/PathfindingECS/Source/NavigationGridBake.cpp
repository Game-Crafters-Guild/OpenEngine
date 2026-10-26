#include "NavigationGridBake.h"

#include "NavigationSourceCollector.h"

#include "Pathfinding/GridMap.h"
#include "Pathfinding/PathfindingTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace GameEngine::PathfindingECS
{

namespace
{

constexpr float32 kNoGround = std::numeric_limits<float32>::lowest();
constexpr float32 kDegreesToRadians = 3.14159265358979323846f / 180.0f;
// Cells per side of a bucket the static triangles are sorted into, so a cell tests only the
// triangles near it.
constexpr uint32 kBucketCells = 8;

// The up-facing static triangles, sorted into square buckets over the grid's extent.
class TriangleBuckets
{
  public:
    TriangleBuckets(const Pathfinding::GridMap& grid, const NavigationTriangles& triangles)
        : m_Triangles(triangles)
    {
        const auto& settings = grid.GetSettings();
        float32 maxX = 0.0f, maxZ = 0.0f;
        grid.CellToWorld(settings.Width - 1, settings.Depth - 1, maxX, maxZ);
        m_MinX = settings.OriginX - settings.CellSize;
        m_MinZ = settings.OriginZ - settings.CellSize;
        m_BucketSize = settings.CellSize * static_cast<float32>(kBucketCells);
        m_BucketsX = static_cast<uint32>(std::ceil((maxX + settings.CellSize - m_MinX) / m_BucketSize)) + 1;
        m_BucketsZ = static_cast<uint32>(std::ceil((maxZ + settings.CellSize - m_MinZ) / m_BucketSize)) + 1;
        m_Buckets.resize(static_cast<size_t>(m_BucketsX) * m_BucketsZ);

        for (uint32 t = 0; t < triangles.TriangleCount(); ++t)
        {
            const float32* a = Vertex(t, 0);
            const float32* b = Vertex(t, 1);
            const float32* c = Vertex(t, 2);
            // Recast's convention: the cross of (b - a) and (c - a) points up for a walkable face.
            const float32 upward = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
            if (!(upward > 0.0f))
                continue;
            const uint32 x0 = BucketX(std::min({a[0], b[0], c[0]}));
            const uint32 x1 = BucketX(std::max({a[0], b[0], c[0]}));
            const uint32 z0 = BucketZ(std::min({a[2], b[2], c[2]}));
            const uint32 z1 = BucketZ(std::max({a[2], b[2], c[2]}));
            for (uint32 z = z0; z <= z1; ++z)
                for (uint32 x = x0; x <= x1; ++x)
                    m_Buckets[static_cast<size_t>(z) * m_BucketsX + x].push_back(t);
        }
    }

    // The highest up-facing triangle under (x, z), or kNoGround.
    float32 GroundAt(float32 x, float32 z) const
    {
        float32 ground = kNoGround;
        for (uint32 t : m_Buckets[static_cast<size_t>(BucketZ(z)) * m_BucketsX + BucketX(x)])
        {
            float32 height = 0.0f;
            if (HeightOnTriangle(t, x, z, height))
                ground = std::max(ground, height);
        }
        return ground;
    }

  private:
    const float32* Vertex(uint32 triangle, uint32 corner) const
    {
        return &m_Triangles.Vertices[static_cast<size_t>(m_Triangles.Indices[triangle * 3 + corner]) * 3];
    }

    uint32 BucketX(float32 x) const
    {
        const float32 cell = std::floor((x - m_MinX) / m_BucketSize);
        return static_cast<uint32>(std::clamp(cell, 0.0f, static_cast<float32>(m_BucketsX - 1)));
    }

    uint32 BucketZ(float32 z) const
    {
        const float32 cell = std::floor((z - m_MinZ) / m_BucketSize);
        return static_cast<uint32>(std::clamp(cell, 0.0f, static_cast<float32>(m_BucketsZ - 1)));
    }

    // The triangle's height above (x, z) when the point lies inside its XZ projection.
    bool HeightOnTriangle(uint32 triangle, float32 x, float32 z, float32& outHeight) const
    {
        const float32* a = Vertex(triangle, 0);
        const float32* b = Vertex(triangle, 1);
        const float32* c = Vertex(triangle, 2);
        const float32 det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
        if (std::abs(det) < 1.0e-12f)
            return false;
        const float32 wa = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / det;
        const float32 wb = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / det;
        const float32 wc = 1.0f - wa - wb;
        constexpr float32 kEdge = -1.0e-5f;
        if (wa < kEdge || wb < kEdge || wc < kEdge)
            return false;
        outHeight = wa * a[1] + wb * b[1] + wc * c[1];
        return true;
    }

    const NavigationTriangles& m_Triangles;
    float32 m_MinX = 0.0f;
    float32 m_MinZ = 0.0f;
    float32 m_BucketSize = 1.0f;
    uint32 m_BucketsX = 1;
    uint32 m_BucketsZ = 1;
    std::vector<std::vector<uint32>> m_Buckets;
};

// A static triangle clipped to a cell's XZ rectangle, with its heights: the triangle's three
// corners gain at most one per rectangle side.
struct ClippedTriangle
{
    float32 Points[7][3] = {};
    uint32 Count = 0;
};

// Keeps the part of `polygon` on the inner side of the line `axis` (0 for X, 2 for Z) = `bound`:
// where (coordinate - bound) * `inward` is not negative.
void ClipToSide(ClippedTriangle& polygon, uint32 axis, float32 bound, float32 inward)
{
    ClippedTriangle kept;
    for (uint32 i = 0; i < polygon.Count; ++i)
    {
        const float32* a = polygon.Points[i];
        const float32* b = polygon.Points[(i + 1) % polygon.Count];
        const float32 insideA = (a[axis] - bound) * inward;
        const float32 insideB = (b[axis] - bound) * inward;
        if (insideA >= 0.0f)
            std::copy(a, a + 3, kept.Points[kept.Count++]);
        if ((insideA >= 0.0f) != (insideB >= 0.0f))
        {
            const float32 t = insideA / (insideA - insideB);
            float32* point = kept.Points[kept.Count++];
            for (uint32 k = 0; k < 3; ++k)
                point[k] = a[k] + (b[k] - a[k]) * t;
        }
    }
    polygon = kept;
}

// What an agent on a cell's ground gets past: a rise up to the climb between neighbouring cells
// (WalkableClimb over the cell spacing, the rise the slope pass allows between neighbours) below
// its feet, and nothing between its feet and its head.
struct AgentClearance
{
    float32 Climb = 0.0f;
    float32 Height = 0.0f;
};

// True when the clipped part of a steep triangle stands in the way of an agent on the cell's ground:
// its top rises above the climb and its bottom is below the agent's head. One threshold wherever in
// the cell the part stands, so moving a wall by part of a cell does not change whether it blocks.
bool Obstructs(const ClippedTriangle& part, float32 ground, const AgentClearance& agent)
{
    float32 low = std::numeric_limits<float32>::max();
    float32 high = std::numeric_limits<float32>::lowest();
    for (uint32 i = 0; i < part.Count; ++i)
    {
        low = std::min(low, part.Points[i][1]);
        high = std::max(high, part.Points[i][1]);
    }
    return high - ground > agent.Climb && low - ground < agent.Height;
}

// The rows (or columns) of `count`, `spacing` apart from `origin`, that [low, high] reaches, and
// `extraBelow` more below: false when it reaches none.
bool CellSpan(float32 low, float32 high, float32 origin, float32 spacing, uint32 count, uint32 extraBelow,
              uint32& outFirst, uint32& outLast)
{
    const float32 first = std::floor((low - origin) / spacing) - static_cast<float32>(extraBelow);
    const float32 last = std::floor((high - origin) / spacing);
    if (last < 0.0f || first >= static_cast<float32>(count))
        return false;
    outFirst = static_cast<uint32>(std::max(first, 0.0f));
    outLast = static_cast<uint32>(std::min(last, static_cast<float32>(count - 1)));
    return true;
}

// Blocks every cell with ground that a static triangle steeper than MaxSlope stands in (a wall, a
// riser, the side or the underside of a solid), wherever in the cell it stands: the ground is read at
// cell centres only, and a wall between two centres covers neither. A cell is the rectangle
// WorldToCell maps to it, the hex grid's rows offset by half a cell. The test is exact for a box; a
// sphere or a capsule is the collector's low-poly hull and a mesh is its triangles.
void BlockObstructedCells(Pathfinding::GridMap& grid, const NavigationTriangles& triangles,
                          const std::vector<uint8>& hasGround, const AgentClearance& agent)
{
    const auto& settings = grid.GetSettings();
    float32 firstX = 0.0f, firstZ = 0.0f, nextRowX = 0.0f, nextRowZ = 0.0f;
    grid.CellToWorld(0, 0, firstX, firstZ);
    grid.CellToWorld(0, 1, nextRowX, nextRowZ);
    const float32 rowSpacing = nextRowZ - firstZ;
    // A face on a cell border, as a box's side on a grid line, belongs to the cell the box is in: the
    // rectangle is shrunk by 1e-4 of a cell, so a face within that of a grid line, as a fence mesh
    // with no thickness laid exactly on one, belongs to neither cell and blocks nothing.
    const float32 halfX = 0.5f * settings.CellSize * (1.0f - 1.0e-4f);
    const float32 halfZ = 0.5f * rowSpacing * (1.0f - 1.0e-4f);
    const float32 minUpward = std::cos(settings.MaxSlope * kDegreesToRadians);

    for (uint32 t = 0; t < triangles.TriangleCount(); ++t)
    {
        ClippedTriangle triangle;
        triangle.Count = 3;
        for (uint32 corner = 0; corner < 3; ++corner)
        {
            const float32* vertex = &triangles.Vertices[static_cast<size_t>(triangles.Indices[t * 3 + corner]) * 3];
            std::copy(vertex, vertex + 3, triangle.Points[corner]);
        }
        const float32* a = triangle.Points[0];
        const float32* b = triangle.Points[1];
        const float32* c = triangle.Points[2];
        const float32 normalX = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
        const float32 normalY = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
        const float32 normalZ = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        const float32 length = std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
        // Ground within MaxSlope is read at the centres; a degenerate triangle has no face.
        if (!(length > 0.0f) || normalY >= minUpward * length)
            continue;

        const float32 minX = std::min({a[0], b[0], c[0]}), maxX = std::max({a[0], b[0], c[0]});
        const float32 minZ = std::min({a[2], b[2], c[2]}), maxZ = std::max({a[2], b[2], c[2]});
        const float32 low = std::min({a[1], b[1], c[1]}), high = std::max({a[1], b[1], c[1]});
        uint32 firstRow = 0, lastRow = 0, firstColumn = 0, lastColumn = 0;
        // One column more below covers the hex grid's odd rows, offset by half a cell.
        if (!CellSpan(minZ, maxZ, firstZ - 0.5f * rowSpacing, rowSpacing, settings.Depth, 0, firstRow, lastRow) ||
            !CellSpan(minX, maxX, settings.OriginX, settings.CellSize, settings.Width, 1, firstColumn, lastColumn))
            continue;

        for (uint32 cz = firstRow; cz <= lastRow; ++cz)
        {
            for (uint32 cx = firstColumn; cx <= lastColumn; ++cx)
            {
                if (!hasGround[grid.CellIndex(cx, cz)] || grid.IsCellBlocked(cx, cz))
                    continue;
                // The whole triangle under a step or over a head: no part of it is in the way. This
                // passes the floor under the cells, the commonest steep face, without clipping it.
                const float32 ground = grid.GetCellHeight(cx, cz);
                if (high - ground <= agent.Climb || low - ground >= agent.Height)
                    continue;
                float32 centreX = 0.0f, centreZ = 0.0f;
                grid.CellToWorld(cx, cz, centreX, centreZ);
                if (maxX <= centreX - halfX || minX >= centreX + halfX || maxZ <= centreZ - halfZ ||
                    minZ >= centreZ + halfZ)
                    continue;
                ClippedTriangle part = triangle;
                ClipToSide(part, 0, centreX - halfX, 1.0f);
                ClipToSide(part, 0, centreX + halfX, -1.0f);
                ClipToSide(part, 2, centreZ - halfZ, 1.0f);
                ClipToSide(part, 2, centreZ + halfZ, -1.0f);
                if (part.Count > 0 && Obstructs(part, ground, agent))
                    grid.SetCellBlocked(cx, cz, true);
            }
        }
    }
}

// Blocks every walkable cell whose ground differs from a neighbour's by more than MaxSlope allows
// over the distance between their centres; per-neighbour distances keep diagonals and hex cells
// right. A neighbour with no ground has no height to compare.
void BlockSteepCells(Pathfinding::GridMap& grid, const std::vector<uint8>& hasGround)
{
    const auto& settings = grid.GetSettings();
    const float32 maxSlopeTangent = std::tan(settings.MaxSlope * kDegreesToRadians);
    for (uint32 cz = 0; cz < settings.Depth; ++cz)
    {
        for (uint32 cx = 0; cx < settings.Width; ++cx)
        {
            if (grid.IsCellBlocked(cx, cz))
                continue;
            const float32 height = grid.GetCellHeight(cx, cz);
            float32 cellX = 0.0f, cellZ = 0.0f;
            grid.CellToWorld(cx, cz, cellX, cellZ);

            uint32 neighbourX[8], neighbourZ[8];
            const uint32 neighbours = grid.GetNeighborCells(cx, cz, neighbourX, neighbourZ, 8);
            for (uint32 i = 0; i < neighbours; ++i)
            {
                if (!hasGround[grid.CellIndex(neighbourX[i], neighbourZ[i])])
                    continue;
                float32 x = 0.0f, z = 0.0f;
                grid.CellToWorld(neighbourX[i], neighbourZ[i], x, z);
                const float32 distance = std::sqrt((x - cellX) * (x - cellX) + (z - cellZ) * (z - cellZ));
                if (std::abs(grid.GetCellHeight(neighbourX[i], neighbourZ[i]) - height) > maxSlopeTangent * distance)
                {
                    grid.SetCellBlocked(cx, cz, true);
                    break;
                }
            }
        }
    }
}

} // namespace

std::unique_ptr<Pathfinding::GridMap> BakeNavigationGrid(const Pathfinding::GridSettings& settings,
                                                         const NavigationTerrainSource& terrain,
                                                         const NavigationStaticGeometry& geometry,
                                                         float32 stepHeight, float32 agentHeight,
                                                         Mathematics::Vector3& outWaitPoint)
{
    auto grid = std::make_unique<Pathfinding::GridMap>(settings);
    const TriangleBuckets buckets(*grid, geometry.Triangles);
    std::vector<uint8> hasGround(grid->GetCellCount(), 0);

    for (uint32 cz = 0; cz < settings.Depth; ++cz)
    {
        for (uint32 cx = 0; cx < settings.Width; ++cx)
        {
            float32 x = 0.0f, z = 0.0f;
            grid->CellToWorld(cx, cz, x, z);
            float32 ground = buckets.GroundAt(x, z);
            if (terrain.Query.ContainsXZ(x, z))
            {
                TerrainECS::SurfaceHeightSample sample;
                if (!terrain.Query.SampleHeight(Mathematics::Vector3(x, 0.0f, z),
                                                TerrainECS::SurfaceHeightPolicy::RequireFinest, sample))
                {
                    outWaitPoint = Mathematics::Vector3(x, 0.0f, z);
                    return nullptr;
                }
                ground = std::max(ground, sample.Height);
            }
            if (ground == kNoGround)
            {
                grid->SetCellBlocked(cx, cz, true);
                continue;
            }
            grid->SetCellHeight(cx, cz, ground);
            hasGround[grid->CellIndex(cx, cz)] = 1;
        }
    }
    const float32 climb = Pathfinding::WalkableClimb(stepHeight, settings.MaxSlope, settings.CellSize);
    BlockObstructedCells(*grid, geometry.Triangles, hasGround, {climb, agentHeight});
    BlockSteepCells(*grid, hasGround);
    return grid;
}

} // namespace GameEngine::PathfindingECS
