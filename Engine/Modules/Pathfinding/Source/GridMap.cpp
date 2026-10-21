#include "Pathfinding/GridMap.h"
#include "AStarSolver.h"
#include "HexGridHelpers.h"
#include "Logger/Logger.h"
#include "PathSmoother.h"
#include "Pathfinding/PathBuffer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <queue>

namespace GameEngine::Pathfinding
{

namespace
{

// One path-solve window on one map for the scope; closed on every exit, a
// solve that throws included.
class ScopedPathSolveWindow
{
public:
    explicit ScopedPathSolveWindow(const GridMap& map)
        : m_Map(map)
    {
        m_Map.OpenPathSolveWindow();
    }

    ~ScopedPathSolveWindow()
    {
        m_Map.ClosePathSolveWindow();
    }

    ScopedPathSolveWindow(const ScopedPathSolveWindow&) = delete;
    ScopedPathSolveWindow& operator=(const ScopedPathSolveWindow&) = delete;

private:
    const GridMap& m_Map;
};

// An axis-aligned rectangle in the XZ plane, in world metres.
struct CellRectangle
{
    float32 MinX = 0.0f, MinZ = 0.0f, MaxX = 0.0f, MaxZ = 0.0f;
};

float32 PointRectangleDistanceSq(float32 x, float32 z, const CellRectangle& rectangle)
{
    const float32 dx = std::max({rectangle.MinX - x, 0.0f, x - rectangle.MaxX});
    const float32 dz = std::max({rectangle.MinZ - z, 0.0f, z - rectangle.MaxZ});
    return dx * dx + dz * dz;
}

float32 PointSegmentDistanceSq(float32 x, float32 z, float32 ax, float32 az, float32 bx, float32 bz)
{
    const float32 dx = bx - ax, dz = bz - az;
    const float32 lengthSq = dx * dx + dz * dz;
    const float32 t = lengthSq > 0.0f ? std::clamp(((x - ax) * dx + (z - az) * dz) / lengthSq, 0.0f, 1.0f) : 0.0f;
    const float32 ex = ax + dx * t - x, ez = az + dz * t - z;
    return ex * ex + ez * ez;
}

// True when the segment from a to b has a point inside the rectangle or on its border.
bool SegmentTouchesRectangle(float32 ax, float32 az, float32 bx, float32 bz, const CellRectangle& rectangle)
{
    float32 enter = 0.0f, leave = 1.0f;
    const float32 delta[2] = {bx - ax, bz - az};
    const float32 start[2] = {ax, az};
    const float32 low[2] = {rectangle.MinX, rectangle.MinZ};
    const float32 high[2] = {rectangle.MaxX, rectangle.MaxZ};
    for (uint32 axis = 0; axis < 2; ++axis)
    {
        if (delta[axis] == 0.0f)
        {
            if (start[axis] < low[axis] || start[axis] > high[axis])
                return false;
            continue;
        }
        float32 t0 = (low[axis] - start[axis]) / delta[axis];
        float32 t1 = (high[axis] - start[axis]) / delta[axis];
        if (t0 > t1)
            std::swap(t0, t1);
        enter = std::max(enter, t0);
        leave = std::min(leave, t1);
        if (enter > leave)
            return false;
    }
    return true;
}

// The squared distance between the segment from a to b and the rectangle: zero when they touch,
// otherwise the nearest of the segment's ends to the rectangle and the rectangle's corners to the
// segment.
float32 SegmentRectangleDistanceSq(float32 ax, float32 az, float32 bx, float32 bz, const CellRectangle& rectangle)
{
    if (SegmentTouchesRectangle(ax, az, bx, bz, rectangle))
        return 0.0f;
    return std::min({PointRectangleDistanceSq(ax, az, rectangle), PointRectangleDistanceSq(bx, bz, rectangle),
                     PointSegmentDistanceSq(rectangle.MinX, rectangle.MinZ, ax, az, bx, bz),
                     PointSegmentDistanceSq(rectangle.MaxX, rectangle.MinZ, ax, az, bx, bz),
                     PointSegmentDistanceSq(rectangle.MinX, rectangle.MaxZ, ax, az, bx, bz),
                     PointSegmentDistanceSq(rectangle.MaxX, rectangle.MaxZ, ax, az, bx, bz)});
}

} // namespace

GridMap::ReadView::ReadView(const GridMap& map)
    : m_Settings(map.m_Settings)
    , m_Costs(map.m_Costs.data())
    , m_Heights(map.m_Heights.data())
    , m_Blocked(map.m_Blocked.data())
    , m_DynamicBlocked(map.m_DynamicBlocked.data())
    , m_Reservations(map.m_Reservations.data())
{
}

GridMap::GridMap(const GridSettings& settings)
    : m_Settings(settings)
{
    if (m_Settings.Width == 0)
    {
        Logger::Log::Error("[GridMap] Width is 0, defaulting to 1");
        m_Settings.Width = 1;
    }
    if (m_Settings.Depth == 0)
    {
        Logger::Log::Error("[GridMap] Depth is 0, defaulting to 1");
        m_Settings.Depth = 1;
    }
    if (m_Settings.CellSize <= 0.0f)
    {
        Logger::Log::Error("[GridMap] CellSize is {} (must be > 0), defaulting to 1.0", m_Settings.CellSize);
        m_Settings.CellSize = 1.0f;
    }

    // Overflow check
    const uint64 cellCount64 = static_cast<uint64>(m_Settings.Width) * static_cast<uint64>(m_Settings.Depth);
    if (cellCount64 > static_cast<uint64>(UINT32_MAX))
    {
        Logger::Log::Error("[GridMap] Width*Depth overflows uint32 ({}x{}), clamping to 1x1",
                           m_Settings.Width, m_Settings.Depth);
        m_Settings.Width = 1;
        m_Settings.Depth = 1;
    }

    const uint32 cellCount = m_Settings.Width * m_Settings.Depth;
    m_Costs.resize(cellCount, 1.0f);
    m_Heights.resize(cellCount, 0.0f);
    m_Blocked.resize(cellCount, 0);
    m_DynamicBlocked.resize(cellCount, 0);
    m_Reservations.resize(cellCount);
}

void GridMap::OpenPathSolveWindow() const
{
    m_ReservationMutex.lock_shared();
    m_PathSolveWindows.fetch_add(1, std::memory_order_relaxed);
}

void GridMap::ClosePathSolveWindow() const
{
    m_PathSolveWindows.fetch_sub(1, std::memory_order_relaxed);
    m_ReservationMutex.unlock_shared();
}

GridMap::ReadView GridMap::View() const
{
    assert(m_PathSolveWindows.load(std::memory_order_relaxed) != 0 &&
           "GridMap: a read view was taken outside a path-solve window; open one with OpenPathSolveWindow "
           "(GridMap::FindPath and NavigationWorld::SolveGridPaths do)");
    return ReadView(*this);
}

// Every writer calls this before it takes the reservation lock, so in an
// asserting build a write inside a window fails here before it could block on
// the window's shared lock. No test in such a build can therefore observe that
// the window takes the lock or that a solve takes none. Both hold by
// construction: a solve reads only through a ReadView, which has no locking
// member, and the thread that opened the window runs only its own
// ParallelFor units before it parks, so no other job takes a second shared
// lock on that thread.
void GridMap::AssertNoPathSolveWindow() const
{
    assert(m_PathSolveWindows.load(std::memory_order_relaxed) == 0 &&
           "GridMap: a grid map was written while path solves read it (NavigationWorld::SolveGridPaths); "
           "write maps outside NavigationPathfindingSystem's update");
}

const GridSettings& GridMap::GetSettings() const
{
    return m_Settings;
}

uint32 GridMap::GetCellCount() const
{
    return ReadView(*this).GetCellCount();
}

uint32 GridMap::GetTopologyVersion() const
{
    return m_TopologyVersion;
}

uint32 GridMap::CellIndex(uint32 cellX, uint32 cellZ) const
{
    return ReadView(*this).CellIndex(cellX, cellZ);
}

void GridMap::CellFromIndex(uint32 index, uint32& outCellX, uint32& outCellZ) const
{
    ReadView(*this).CellFromIndex(index, outCellX, outCellZ);
}

bool GridMap::IsValidCell(uint32 cellX, uint32 cellZ) const
{
    return ReadView(*this).IsValidCell(cellX, cellZ);
}

float32 GridMap::GetCellCost(uint32 cellX, uint32 cellZ) const
{
    return ReadView(*this).GetCellCost(cellX, cellZ);
}

void GridMap::SetCellCost(uint32 cellX, uint32 cellZ, float32 cost)
{
    AssertNoPathSolveWindow();
    if (!IsValidCell(cellX, cellZ))
        return;
    m_Costs[CellIndex(cellX, cellZ)] = cost;
    ++m_TopologyVersion;
}

bool GridMap::IsCellBlocked(uint32 cellX, uint32 cellZ) const
{
    return ReadView(*this).IsCellBlocked(cellX, cellZ);
}

void GridMap::SetCellBlocked(uint32 cellX, uint32 cellZ, bool blocked)
{
    AssertNoPathSolveWindow();
    if (!IsValidCell(cellX, cellZ))
        return;
    m_Blocked[CellIndex(cellX, cellZ)] = blocked ? 1 : 0;
    ++m_TopologyVersion;
}

float32 GridMap::GetCellHeight(uint32 cellX, uint32 cellZ) const
{
    return ReadView(*this).GetCellHeight(cellX, cellZ);
}

void GridMap::SetCellHeight(uint32 cellX, uint32 cellZ, float32 height)
{
    AssertNoPathSolveWindow();
    if (!IsValidCell(cellX, cellZ))
        return;
    m_Heights[CellIndex(cellX, cellZ)] = height;
    ++m_TopologyVersion;
}

void GridMap::SetCellDynamicBlocked(uint32 cellX, uint32 cellZ, bool blocked)
{
    AssertNoPathSolveWindow();
    if (!IsValidCell(cellX, cellZ))
        return;
    m_DynamicBlocked[CellIndex(cellX, cellZ)] = blocked ? 1 : 0;
}

void GridMap::ClearAllDynamicBlocked()
{
    AssertNoPathSolveWindow();
    std::memset(m_DynamicBlocked.data(), 0, m_DynamicBlocked.size());
}

bool GridMap::IsCellDynamicBlocked(uint32 cellX, uint32 cellZ) const
{
    if (!IsValidCell(cellX, cellZ))
        return false;
    return m_DynamicBlocked[CellIndex(cellX, cellZ)] != 0;
}

void GridMap::ReserveCell(uint32 cellX, uint32 cellZ, uint32 agentId, float32 expirationTime,
                          bool occupancy)
{
    AssertNoPathSolveWindow();
    if (!IsValidCell(cellX, cellZ))
        return;
    std::unique_lock lock(m_ReservationMutex);
    const uint32 index = CellIndex(cellX, cellZ);
    auto& reservation = m_Reservations[index];
    const bool sameOwner = reservation.AgentId == agentId;
    reservation.AgentId = agentId;
    reservation.ExpirationTime = expirationTime;
    if (sameOwner)
        reservation.Occupancy = (reservation.Occupancy != 0 || occupancy) ? 1 : 0;
    else
        reservation.Occupancy = occupancy ? 1 : 0;
    if (agentId == 0)
        return;

    auto& cells = m_AgentCells[agentId];
    if (std::find(cells.begin(), cells.end(), index) == cells.end())
        cells.push_back(index);
}

void GridMap::ClearCellReservation(uint32 cellX, uint32 cellZ)
{
    AssertNoPathSolveWindow();
    if (!IsValidCell(cellX, cellZ))
        return;
    std::unique_lock lock(m_ReservationMutex);
    auto& reservation = m_Reservations[CellIndex(cellX, cellZ)];
    reservation.AgentId = 0;
    reservation.ExpirationTime = 0.0f;
    reservation.Occupancy = 0;
}

CellReservation GridMap::GetCellReservation(uint32 cellX, uint32 cellZ) const
{
    std::shared_lock lock(m_ReservationMutex);
    return ReadView(*this).Reservation(cellX, cellZ);
}

void GridMap::ClearReservationsForAgent(uint32 agentId)
{
    AssertNoPathSolveWindow();
    if (agentId == 0)
        return;
    std::unique_lock lock(m_ReservationMutex);
    auto it = m_AgentCells.find(agentId);
    if (it == m_AgentCells.end())
        return;
    for (uint32 index : it->second)
    {
        if (index < m_Reservations.size() && m_Reservations[index].AgentId == agentId)
        {
            m_Reservations[index].AgentId = 0;
            m_Reservations[index].ExpirationTime = 0.0f;
            m_Reservations[index].Occupancy = 0;
        }
    }
    m_AgentCells.erase(it);
}

void GridMap::ClearExpiredReservations(float32 currentTime)
{
    AssertNoPathSolveWindow();
    std::unique_lock lock(m_ReservationMutex);
    for (auto it = m_AgentCells.begin(); it != m_AgentCells.end();)
    {
        auto& cells = it->second;
        size_t write = 0;
        for (uint32 index : cells)
        {
            if (index >= m_Reservations.size())
                continue;
            auto& reservation = m_Reservations[index];
            if (reservation.AgentId != it->first)
                continue;
            if (reservation.ExpirationTime <= currentTime)
            {
                reservation.AgentId = 0;
                reservation.ExpirationTime = 0.0f;
                reservation.Occupancy = 0;
                continue;
            }
            cells[write++] = index;
        }
        cells.resize(write);
        if (cells.empty())
            it = m_AgentCells.erase(it);
        else
            ++it;
    }

    // Orphans (cleared from a list without zeroing, or stolen then abandoned).
    for (auto& reservation : m_Reservations)
    {
        if (reservation.AgentId != 0 && reservation.ExpirationTime <= currentTime)
        {
            reservation.AgentId = 0;
            reservation.ExpirationTime = 0.0f;
            reservation.Occupancy = 0;
        }
    }
}

bool GridMap::IsOccupiedByOther(uint32 cellX, uint32 cellZ, uint32 excludeAgentId,
                                float32 currentTime) const
{
    if (!IsValidCell(cellX, cellZ))
        return false;
    std::shared_lock lock(m_ReservationMutex);
    const auto& reservation = m_Reservations[CellIndex(cellX, cellZ)];
    return reservation.Occupancy != 0 && reservation.IsActive(currentTime) &&
           reservation.AgentId != excludeAgentId;
}

uint32 GridMap::GetReservedAgentCount() const
{
    std::shared_lock lock(m_ReservationMutex);
    return static_cast<uint32>(m_AgentCells.size());
}

bool GridMap::ReadView::WorldToCell(float32 worldX, float32 worldZ, uint32& outCellX, uint32& outCellZ) const
{
    if (m_Settings.Type == GridType::Square)
    {
        const float32 localX = worldX - m_Settings.OriginX;
        const float32 localZ = worldZ - m_Settings.OriginZ;

        if (localX < 0.0f || localZ < 0.0f)
            return false;

        const int32 cellX = static_cast<int32>(std::floor(localX / m_Settings.CellSize));
        const int32 cellZ = static_cast<int32>(std::floor(localZ / m_Settings.CellSize));

        if (cellX < 0 || cellX >= static_cast<int32>(m_Settings.Width) ||
            cellZ < 0 || cellZ >= static_cast<int32>(m_Settings.Depth))
        {
            return false;
        }

        outCellX = static_cast<uint32>(cellX);
        outCellZ = static_cast<uint32>(cellZ);
        return true;
    }
    else
    {
        int32 col = 0, row = 0;
        HexGrid::WorldToHex(worldX, worldZ, m_Settings.CellSize,
                            m_Settings.OriginX, m_Settings.OriginZ, col, row);

        if (col < 0 || col >= static_cast<int32>(m_Settings.Width) ||
            row < 0 || row >= static_cast<int32>(m_Settings.Depth))
        {
            return false;
        }

        outCellX = static_cast<uint32>(col);
        outCellZ = static_cast<uint32>(row);
        return true;
    }
}

void GridMap::ReadView::CellToWorld(uint32 cellX, uint32 cellZ, float32& outWorldX, float32& outWorldZ) const
{
    if (m_Settings.Type == GridType::Square)
    {
        outWorldX = m_Settings.OriginX + (static_cast<float32>(cellX) + 0.5f) * m_Settings.CellSize;
        outWorldZ = m_Settings.OriginZ + (static_cast<float32>(cellZ) + 0.5f) * m_Settings.CellSize;
    }
    else
    {
        HexGrid::HexToWorld(cellX, cellZ, m_Settings.CellSize,
                            m_Settings.OriginX, m_Settings.OriginZ, outWorldX, outWorldZ);
    }
}

bool GridMap::WorldToCell(float32 worldX, float32 worldZ, uint32& outCellX, uint32& outCellZ) const
{
    return ReadView(*this).WorldToCell(worldX, worldZ, outCellX, outCellZ);
}

void GridMap::CellToWorld(uint32 cellX, uint32 cellZ, float32& outWorldX, float32& outWorldZ) const
{
    ReadView(*this).CellToWorld(cellX, cellZ, outWorldX, outWorldZ);
}

uint32 GridMap::FootprintRadiusInCells(float32 agentRadius) const
{
    if (agentRadius <= m_Settings.CellSize * 0.5f)
        return 0;
    return static_cast<uint32>(std::ceil(agentRadius / m_Settings.CellSize));
}

uint32 GridMap::GetNeighborCells(uint32 cellX, uint32 cellZ,
                                 uint32* outCellX, uint32* outCellZ, uint32 maxNeighbors) const
{
    uint32 count = 0;
    if (m_Settings.Type == GridType::Hex)
    {
        int32 dx[6], dz[6];
        uint32 hexCount = 0;
        HexGrid::GetNeighborOffsets(cellZ, dx, dz, hexCount);
        for (uint32 i = 0; i < hexCount && count < maxNeighbors; ++i)
        {
            const int32 nx = static_cast<int32>(cellX) + dx[i];
            const int32 nz = static_cast<int32>(cellZ) + dz[i];
            if (nx >= 0 && nx < static_cast<int32>(m_Settings.Width) &&
                nz >= 0 && nz < static_cast<int32>(m_Settings.Depth))
            {
                outCellX[count] = static_cast<uint32>(nx);
                outCellZ[count] = static_cast<uint32>(nz);
                ++count;
            }
        }
    }
    else
    {
        static constexpr int32 kDx[] = {0, 0, 1, -1, 1, -1, 1, -1};
        static constexpr int32 kDz[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        for (uint32 i = 0; i < 8 && count < maxNeighbors; ++i)
        {
            const int32 nx = static_cast<int32>(cellX) + kDx[i];
            const int32 nz = static_cast<int32>(cellZ) + kDz[i];
            if (nx >= 0 && nx < static_cast<int32>(m_Settings.Width) &&
                nz >= 0 && nz < static_cast<int32>(m_Settings.Depth))
            {
                outCellX[count] = static_cast<uint32>(nx);
                outCellZ[count] = static_cast<uint32>(nz);
                ++count;
            }
        }
    }
    return count;
}

PathStatus GridMap::FindPath(const PathRequest& request, PathBuffer& pathBuffer, PathHandle& outPath) const
{
    // One solver per thread, so its scratch is reused across that thread's solves instead of being
    // allocated per solve. A solve never yields, so it runs to completion on the thread it started on.
    static thread_local AStarSolver solver;
    return FindPath(request, solver, pathBuffer, outPath);
}

PathStatus GridMap::FindPath(const PathRequest& request, AStarSolver& solver, PathBuffer& pathBuffer,
                             PathHandle& outPath) const
{
    std::vector<PathPoint> worldPoints;
    PathStatus status = PathStatus::Failed;
    {
        const ScopedPathSolveWindow window(*this);
        status = FindPathPoints(request, solver, worldPoints);
    }
    if (status == PathStatus::Complete)
        outPath = pathBuffer.AllocatePath(worldPoints.data(), static_cast<uint32>(worldPoints.size()));
    return status;
}

PathStatus GridMap::FindPathPoints(const PathRequest& request, AStarSolver& solver,
                                   std::vector<PathPoint>& outPoints) const
{
    outPoints.clear();
    const ReadView view = View();
    uint32 startCellX = 0, startCellZ = 0;
    uint32 endCellX = 0, endCellZ = 0;

    if (!view.WorldToCell(request.StartX, request.StartZ, startCellX, startCellZ))
        return PathStatus::Failed;
    if (!view.WorldToCell(request.EndX, request.EndZ, endCellX, endCellZ))
        return PathStatus::Failed;

    const uint32 startIndex = view.CellIndex(startCellX, startCellZ);
    const uint32 endIndex = view.CellIndex(endCellX, endCellZ);

    uint32 agentCellRadius = 0;
    const float32 cellSize = view.GetSettings().CellSize;
    if (request.AgentRadius > cellSize * 0.5f)
    {
        agentCellRadius = static_cast<uint32>(std::ceil(request.AgentRadius / cellSize));
    }

    std::vector<uint32> cellPath;
    if (!solver.Solve(view, startIndex, endIndex, request.ExcludeAgentId, request.CurrentTime, agentCellRadius, cellPath))
        return PathStatus::Failed;

    outPoints.reserve(cellPath.size());

    for (uint32 cellIndex : cellPath)
    {
        uint32 cx = 0, cz = 0;
        view.CellFromIndex(cellIndex, cx, cz);

        float32 worldX = 0.0f, worldZ = 0.0f;
        view.CellToWorld(cx, cz, worldX, worldZ);

        PathPoint point;
        point.X = worldX;
        point.Y = view.GetCellHeight(cx, cz);
        point.Z = worldZ;
        outPoints.push_back(point);
    }

    if (request.SmoothPath && outPoints.size() > 2)
    {
        PathSmoother::Smooth(view, outPoints, request.AgentRadius);
    }

    return PathStatus::Complete;
}

// Reads only the blocked arrays, as Raycast does.
bool GridMap::IsPointNavigable(float32 x, float32 /*y*/, float32 z, float32 radius) const
{
    return ReadView(*this).IsSegmentClear(x, z, x, z, radius);
}

bool GridMap::GetClosestNavigablePoint(float32 x, float32 /*y*/, float32 z,
                                        float32 searchRadius,
                                        float32& outX, float32& outY, float32& outZ) const
{
    uint32 startCellX = 0, startCellZ = 0;

    // Clamp the query point to grid bounds for the BFS start
    float32 clampedX = std::max(m_Settings.OriginX, std::min(x, m_Settings.OriginX + m_Settings.Width * m_Settings.CellSize - 0.01f));
    float32 clampedZ = std::max(m_Settings.OriginZ, std::min(z, m_Settings.OriginZ + m_Settings.Depth * m_Settings.CellSize - 0.01f));

    if (!WorldToCell(clampedX, clampedZ, startCellX, startCellZ))
        return false;

    // If the start cell is navigable, return its center
    if (!IsCellBlocked(startCellX, startCellZ))
    {
        CellToWorld(startCellX, startCellZ, outX, outZ);
        outY = GetCellHeight(startCellX, startCellZ);
        return true;
    }

    // BFS spiral outward using a bounded visited set (only allocate within search region)
    const uint32 maxRadius = static_cast<uint32>(std::ceil(searchRadius / m_Settings.CellSize));
    const uint32 maxRadiusSq = maxRadius * maxRadius;

    // Compute BFS region bounds clamped to grid
    const uint32 minBfsX = (startCellX > maxRadius) ? (startCellX - maxRadius) : 0;
    const uint32 minBfsZ = (startCellZ > maxRadius) ? (startCellZ - maxRadius) : 0;
    const uint32 maxBfsX = std::min(startCellX + maxRadius, m_Settings.Width - 1);
    const uint32 maxBfsZ = std::min(startCellZ + maxRadius, m_Settings.Depth - 1);
    const uint32 regionW = maxBfsX - minBfsX + 1;
    const uint32 regionD = maxBfsZ - minBfsZ + 1;

    std::vector<uint8> visited(regionW * regionD, 0);
    auto regionIndex = [&](uint32 cx, uint32 cz) { return (cz - minBfsZ) * regionW + (cx - minBfsX); };

    struct BfsEntry
    {
        uint32 CellX;
        uint32 CellZ;
    };

    std::queue<BfsEntry> frontier;
    frontier.push({startCellX, startCellZ});
    visited[regionIndex(startCellX, startCellZ)] = 1;

    while (!frontier.empty())
    {
        BfsEntry current = frontier.front();
        frontier.pop();

        // Use GetNeighborCells to get correct neighbors for both square and hex grids
        uint32 neighX[8], neighZ[8];
        uint32 neighCount = GetNeighborCells(current.CellX, current.CellZ, neighX, neighZ, 8);

        for (uint32 i = 0; i < neighCount; ++i)
        {
            const uint32 unx = neighX[i];
            const uint32 unz = neighZ[i];

            if (unx < minBfsX || unx > maxBfsX || unz < minBfsZ || unz > maxBfsZ)
                continue;

            const uint32 rIdx = regionIndex(unx, unz);

            if (visited[rIdx])
                continue;
            visited[rIdx] = 1;

            // Circular distance check (squared cell distance)
            const uint32 dx = (unx > startCellX) ? (unx - startCellX) : (startCellX - unx);
            const uint32 dz = (unz > startCellZ) ? (unz - startCellZ) : (startCellZ - unz);
            if (dx * dx + dz * dz > maxRadiusSq)
                continue;

            if (!IsCellBlocked(unx, unz))
            {
                CellToWorld(unx, unz, outX, outZ);
                outY = GetCellHeight(unx, unz);
                return true;
            }

            frontier.push({unx, unz});
        }
    }

    return false;
}

// Reads only the blocked and height arrays, which no writer touches inside a
// path-solve window, so it needs no window of its own.
bool GridMap::Raycast(float32 startX, float32 /*startY*/, float32 startZ,
                      float32 endX, float32 /*endY*/, float32 endZ,
                      float32& hitX, float32& hitY, float32& hitZ) const
{
    return ReadView(*this).Raycast(startX, startZ, endX, endZ, hitX, hitY, hitZ);
}

bool GridMap::IsSegmentClear(float32 startX, float32 startZ, float32 endX, float32 endZ,
                             float32 radius, const CellRange* openCells) const
{
    return ReadView(*this).IsSegmentClear(startX, startZ, endX, endZ, radius, openCells);
}

bool GridMap::ReadView::IsSegmentClear(float32 startX, float32 startZ, float32 endX, float32 endZ,
                                        float32 radius, const CellRange* openCells) const
{
    assert((radius > 0.0f || openCells == nullptr) && "GridMap: Raycast's line test has no open cells");
    uint32 cellX = 0, cellZ = 0;
    if (!WorldToCell(startX, startZ, cellX, cellZ))
        return false;
    if (IsCellBlocked(cellX, cellZ) && !(openCells && openCells->Contains(cellX, cellZ)))
        return false;
    if (radius <= 0.0f)
    {
        float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
        return !Raycast(startX, startZ, endX, endZ, hitX, hitY, hitZ);
    }

    // Cell (x, z) is the rectangle WorldToCell maps to it: centred on CellToWorld, a cell wide and a
    // row apart, odd rows of a hex grid shifted by `oddRowShift`. Off the grid counts as blocked.
    float32 firstX = 0.0f, firstZ = 0.0f, nextRowX = 0.0f, nextRowZ = 0.0f;
    CellToWorld(0, 0, firstX, firstZ);
    CellToWorld(0, 1, nextRowX, nextRowZ);
    const float32 cellSize = m_Settings.CellSize;
    const float32 rowSpacing = nextRowZ - firstZ;
    const float32 oddRowShift = nextRowX - firstX;
    const float32 radiusSq = radius * radius;
    const float32 deltaZ = endZ - startZ;

    const int32 firstRow = static_cast<int32>(std::floor((std::min(startZ, endZ) - radius - m_Settings.OriginZ) / rowSpacing));
    const int32 lastRow = static_cast<int32>(std::floor((std::max(startZ, endZ) + radius - m_Settings.OriginZ) / rowSpacing));
    for (int32 row = firstRow; row <= lastRow; ++row)
    {
        // The stretch of the segment within `radius` of the row's band, widened by `radius` along X:
        // every cell of the row the swept disc can reach lies under it.
        const float32 bandMinZ = m_Settings.OriginZ + static_cast<float32>(row) * rowSpacing - radius;
        const float32 bandMaxZ = bandMinZ + rowSpacing + 2.0f * radius;
        float32 enter = 0.0f, leave = 1.0f;
        if (deltaZ != 0.0f)
        {
            enter = std::max(0.0f, std::min((bandMinZ - startZ) / deltaZ, (bandMaxZ - startZ) / deltaZ));
            leave = std::min(1.0f, std::max((bandMinZ - startZ) / deltaZ, (bandMaxZ - startZ) / deltaZ));
        }
        else if (startZ < bandMinZ || startZ > bandMaxZ)
        {
            continue;
        }
        if (enter > leave)
            continue;
        const float32 enterX = startX + (endX - startX) * enter;
        const float32 leaveX = startX + (endX - startX) * leave;
        const float32 shift = (row & 1) != 0 ? oddRowShift : 0.0f;
        const int32 firstColumn = static_cast<int32>(std::floor((std::min(enterX, leaveX) - radius - m_Settings.OriginX - shift) / cellSize));
        const int32 lastColumn = static_cast<int32>(std::floor((std::max(enterX, leaveX) + radius - m_Settings.OriginX - shift) / cellSize));

        for (int32 column = firstColumn; column <= lastColumn; ++column)
        {
            const bool onGrid = row >= 0 && column >= 0 && row < static_cast<int32>(m_Settings.Depth) &&
                                column < static_cast<int32>(m_Settings.Width);
            if (onGrid && (!IsCellBlocked(static_cast<uint32>(column), static_cast<uint32>(row)) ||
                           (openCells && openCells->Contains(static_cast<uint32>(column), static_cast<uint32>(row)))))
                continue;
            CellRectangle cell;
            cell.MinX = m_Settings.OriginX + shift + static_cast<float32>(column) * cellSize;
            cell.MaxX = cell.MinX + cellSize;
            cell.MinZ = m_Settings.OriginZ + static_cast<float32>(row) * rowSpacing;
            cell.MaxZ = cell.MinZ + rowSpacing;
            if (SegmentRectangleDistanceSq(startX, startZ, endX, endZ, cell) < radiusSq)
                return false;
        }
    }
    return true;
}

bool GridMap::ReadView::Raycast(float32 startX, float32 startZ, float32 endX, float32 endZ,
                                float32& hitX, float32& hitY, float32& hitZ) const
{
    uint32 startCellX = 0, startCellZ = 0;
    uint32 endCellX = 0, endCellZ = 0;

    if (!WorldToCell(startX, startZ, startCellX, startCellZ))
        return false;
    if (!WorldToCell(endX, endZ, endCellX, endCellZ))
        return false;

    // Bresenham line algorithm
    int32 x0 = static_cast<int32>(startCellX);
    int32 z0 = static_cast<int32>(startCellZ);
    const int32 x1 = static_cast<int32>(endCellX);
    const int32 z1 = static_cast<int32>(endCellZ);

    const int32 dx = std::abs(x1 - x0);
    const int32 dz = std::abs(z1 - z0);
    const int32 sx = (x0 < x1) ? 1 : -1;
    const int32 sz = (z0 < z1) ? 1 : -1;
    int32 err = dx - dz;

    while (true)
    {
        if (x0 >= 0 && x0 < static_cast<int32>(m_Settings.Width) &&
            z0 >= 0 && z0 < static_cast<int32>(m_Settings.Depth))
        {
            const uint32 ux = static_cast<uint32>(x0);
            const uint32 uz = static_cast<uint32>(z0);

            if (IsCellBlocked(ux, uz))
            {
                CellToWorld(ux, uz, hitX, hitZ);
                hitY = GetCellHeight(ux, uz);
                return true;
            }
        }

        if (x0 == x1 && z0 == z1)
            break;

        const int32 e2 = 2 * err;
        const bool stepX = (e2 > -dz);
        const bool stepZ = (e2 < dx);

        // If both X and Z step simultaneously (diagonal move), check the two
        // intermediate cardinal cells to prevent the ray cutting through wall corners.
        if (stepX && stepZ)
        {
            const int32 adjX = x0 + sx;
            const int32 adjZ = z0 + sz;
            bool xBlocked = (adjX >= 0 && adjX < static_cast<int32>(m_Settings.Width) &&
                             z0 >= 0 && z0 < static_cast<int32>(m_Settings.Depth) &&
                             IsCellBlocked(static_cast<uint32>(adjX), static_cast<uint32>(z0)));
            bool zBlocked = (x0 >= 0 && x0 < static_cast<int32>(m_Settings.Width) &&
                             adjZ >= 0 && adjZ < static_cast<int32>(m_Settings.Depth) &&
                             IsCellBlocked(static_cast<uint32>(x0), static_cast<uint32>(adjZ)));
            if (xBlocked || zBlocked)
            {
                // Hit a wall corner — report the current cell as the hit point
                if (x0 >= 0 && x0 < static_cast<int32>(m_Settings.Width) &&
                    z0 >= 0 && z0 < static_cast<int32>(m_Settings.Depth))
                {
                    CellToWorld(static_cast<uint32>(x0), static_cast<uint32>(z0), hitX, hitZ);
                    hitY = GetCellHeight(static_cast<uint32>(x0), static_cast<uint32>(z0));
                    return true;
                }
            }
        }

        if (stepX)
        {
            err -= dz;
            x0 += sx;
        }
        if (stepZ)
        {
            err += dx;
            z0 += sz;
        }
    }

    return false;
}

const float32* GridMap::GetCostData() const
{
    return m_Costs.data();
}

const uint8* GridMap::GetBlockedData() const
{
    return m_Blocked.data();
}

} // namespace GameEngine::Pathfinding
