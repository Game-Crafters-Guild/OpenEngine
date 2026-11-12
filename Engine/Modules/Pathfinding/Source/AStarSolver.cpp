#include "AStarSolver.h"
#include "HexGridHelpers.h"
#include "Mathematics/Angle.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

namespace GameEngine::Pathfinding
{

static constexpr float32 kSqrt2 = 1.41421356f;
static constexpr float32 kReservationPenalty = 3.0f;
static constexpr float32 kInfinity = std::numeric_limits<float32>::max();

struct AStarNode
{
    uint32 CellIndex;
    float32 FCost;
};

struct NodeCompare
{
    bool operator()(const AStarNode& a, const AStarNode& b) const { return a.FCost > b.FCost; }
};

namespace
{

template <class T>
uint64 CapacityBytes(const std::vector<T>& buffer)
{
    return static_cast<uint64>(buffer.capacity()) * sizeof(T);
}

template <class T>
void FreeBuffer(std::vector<T>& buffer)
{
    std::vector<T>().swap(buffer);
}

// True when an agent of `agentCellRadius` cells fits on the cell: the square of cells around it is on the
// grid and open. A zero radius tests the cell only.
bool IsFootprintClear(const GridMap::ReadView& grid, uint32 cellX, uint32 cellZ, uint32 agentCellRadius)
{
    if (agentCellRadius == 0)
        return !grid.IsCellBlocked(cellX, cellZ);
    const auto& settings = grid.GetSettings();
    const int32 radius = static_cast<int32>(agentCellRadius);
    for (int32 offsetZ = -radius; offsetZ <= radius; ++offsetZ)
    {
        for (int32 offsetX = -radius; offsetX <= radius; ++offsetX)
        {
            const int32 footprintX = static_cast<int32>(cellX) + offsetX;
            const int32 footprintZ = static_cast<int32>(cellZ) + offsetZ;
            if (footprintX < 0 || footprintX >= static_cast<int32>(settings.Width) || footprintZ < 0 ||
                footprintZ >= static_cast<int32>(settings.Depth))
                return false;
            if (grid.IsCellBlocked(static_cast<uint32>(footprintX), static_cast<uint32>(footprintZ)))
                return false;
        }
    }
    return true;
}

// True when the step from a cell at `fromHeight` onto (cellX, cellZ), `baseCost` cells across, is within the
// grid's step height and slope. It reads the same in both directions.
bool IsStepWithinLimits(const GridMap::ReadView& grid, float32 fromHeight, uint32 cellX, uint32 cellZ, float32 baseCost)
{
    const auto& settings = grid.GetSettings();
    const float32 heightDiff = std::abs(grid.GetCellHeight(cellX, cellZ) - fromHeight);
    if (heightDiff > settings.MaxStepHeight)
        return false;
    const float32 horizontalDist = baseCost * settings.CellSize;
    return horizontalDist <= 0.0f || Mathematics::ToDegrees(std::atan2(heightDiff, horizontalDist)) <= settings.MaxSlope;
}

} // namespace

uint64 AStarSolver::HeldBytes() const
{
    return CapacityBytes(m_GScore) + CapacityBytes(m_Parent) + CapacityBytes(m_CellFlags) + CapacityBytes(m_VisitStamp);
}

void AStarSolver::ReleaseScratchAbove(uint32 maxCells)
{
    if (m_GScore.capacity() <= maxCells)
        return;
    // A solve resizes the buffers before it stamps a cell, and a resize stamps the cells it adds 0,
    // which is never the current generation: emptied buffers need no other reset.
    FreeBuffer(m_GScore);
    FreeBuffer(m_Parent);
    FreeBuffer(m_CellFlags);
    FreeBuffer(m_VisitStamp);
}

GridSolveEnd AStarSolver::Solve(const GridMap::ReadView& grid, uint32 startIndex, uint32 endIndex,
                                 uint32 excludeAgentId, float32 currentTime,
                                 uint32 agentCellRadius,
                                 std::vector<uint32>& outPathCellIndices, uint32& outExpansions)
{
    outPathCellIndices.clear();
    outExpansions = 0;

    if (startIndex == endIndex)
    {
        outPathCellIndices.push_back(startIndex);
        return GridSolveEnd::Reached;
    }

    const uint32 cellCount = grid.GetCellCount();
    if (startIndex >= cellCount || endIndex >= cellCount)
        return GridSolveEnd::Refused;

    uint32 startX = 0, startZ = 0, endX = 0, endZ = 0;
    grid.CellFromIndex(startIndex, startX, startZ);
    grid.CellFromIndex(endIndex, endX, endZ);
    if (grid.IsCellBlocked(startX, startZ) || grid.IsCellBlocked(endX, endZ))
        return GridSolveEnd::Refused;
    // A goal outside the start's region cannot be reached: fail here, before the scratch is sized,
    // instead of expanding every cell the start reaches.
    if (!grid.InSameRegion(startIndex, endIndex))
        return GridSolveEnd::OtherRegion;

    // The regions see only static blocking and heights, so dynamic obstacles, slopes or the agent's width
    // can still shut in a goal they join to the start. A footprint that does not fit on the goal cell
    // never reaches it.
    if (!IsFootprintClear(grid, endX, endZ, agentCellRadius))
        return GridSolveEnd::Unreachable;

    // Every cell is expanded at most once (the closed set), so a path that exists is found however
    // far it detours. The flood from the goal (ExpandGoalSide) ends a solve to a shut-in goal after
    // about twice the cells the goal's enclosure holds.
    const uint32 maxExpansions = cellCount;

    // Growth reserves exactly cellCount, so capacity is the largest grid solved on: ReleaseScratchAbove
    // compares it with a grid's cell count, and geometric growth would leave more than any grid needs.
    if (cellCount > m_GScore.capacity())
    {
        m_GScore.reserve(cellCount);
        m_Parent.reserve(cellCount);
        m_CellFlags.reserve(cellCount);
        m_VisitStamp.reserve(cellCount);
    }
    m_GScore.resize(cellCount);
    m_Parent.resize(cellCount);
    m_CellFlags.resize(cellCount);
    m_VisitStamp.resize(cellCount);
    BeginGeneration();

    std::vector<AStarNode> openList;
    openList.reserve(256);

    Visit(startIndex);
    m_GScore[startIndex] = 0.0f;
    openList.push_back({startIndex, Heuristic(grid, startIndex, endIndex)});

    // The flood from the goal: the cells it reached in order, the next one to expand, and whether it
    // still runs. It starts at the first expansion past kStartSideOnlyExpansions, so a short solve
    // does not allocate it.
    std::vector<uint32> goalSideCells;
    size_t goalSideNext = 0;
    bool goalSideRuns = true;

    // Local counters: m_CellFlags is a uint8 buffer, so its stores may alias a counter behind a reference.
    uint32 expansions = 0;
    uint32 goalSideExpansions = 0;
    while (!openList.empty() && expansions < maxExpansions)
    {
        std::pop_heap(openList.begin(), openList.end(), NodeCompare{});
        AStarNode current = openList.back();
        openList.pop_back();

        if (current.CellIndex == endIndex)
        {
            uint32 idx = endIndex;
            while (idx != UINT32_MAX)
            {
                outPathCellIndices.push_back(idx);
                assert(outPathCellIndices.size() <= cellCount &&
                       "AStarSolver: the parent chain loops; a cell on it was not visited in this generation");
                idx = m_Parent[idx];
            }
            std::reverse(outPathCellIndices.begin(), outPathCellIndices.end());
            outExpansions = expansions + goalSideExpansions;
            return GridSolveEnd::Reached;
        }

        if (m_CellFlags[current.CellIndex] & kClosedFlag)
            continue;
        m_CellFlags[current.CellIndex] |= kClosedFlag;
        ++expansions;

        if (goalSideRuns && expansions > kStartSideOnlyExpansions)
        {
            if (goalSideCells.empty())
            {
                Visit(endIndex);
                m_CellFlags[endIndex] |= kGoalSideFlag;
                goalSideCells.push_back(endIndex);
            }
            ++goalSideExpansions;
            const GoalSideStep step = ExpandGoalSide(grid, agentCellRadius, goalSideCells, goalSideNext);
            if (step == GoalSideStep::Exhausted)
            {
                outExpansions = expansions + goalSideExpansions;
                return GridSolveEnd::Unreachable;
            }
            goalSideRuns = step == GoalSideStep::Continue;
        }

        uint32 neighbors[8];
        float32 baseCosts[8];
        uint32 neighborCount = 0;
        GetNeighbors(grid, current.CellIndex, neighbors, baseCosts, neighborCount);

        uint32 cx = 0, cz = 0;
        grid.CellFromIndex(current.CellIndex, cx, cz);
        const float32 currentHeight = grid.GetCellHeight(cx, cz);

        for (uint32 slot = 0; slot < neighborCount; ++slot)
        {
            const uint32 neighbor = neighbors[slot];
            if (IsClosed(neighbor))
                continue;

            uint32 nx = 0, nz = 0;
            grid.CellFromIndex(neighbor, nx, nz);

            if (!IsFootprintClear(grid, nx, nz, agentCellRadius) ||
                !IsStepWithinLimits(grid, currentHeight, nx, nz, baseCosts[slot]))
                continue;

            float32 moveCost = baseCosts[slot] * grid.GetCellCost(nx, nz);

            const CellReservation reservation = grid.Reservation(nx, nz);
            if (reservation.IsActive(currentTime) && reservation.AgentId != excludeAgentId)
            {
                // Occupancy is a tax, not a wall. Movement still refuses
                // another agent's body cell. Dest stays searchable so a
                // dest-allowed close can finish.
                moveCost *= (1.0f + kReservationPenalty);
            }

            const float32 tentativeG = m_GScore[current.CellIndex] + moveCost;
            if (tentativeG < GScore(neighbor))
            {
                if (IsGoalSide(neighbor))
                    goalSideRuns = false;
                Visit(neighbor);
                m_GScore[neighbor] = tentativeG;
                m_Parent[neighbor] = current.CellIndex;
                const float32 fCost = tentativeG + Heuristic(grid, neighbor, endIndex);
                openList.push_back({neighbor, fCost});
                std::push_heap(openList.begin(), openList.end(), NodeCompare{});
            }
        }
    }

    outExpansions = expansions + goalSideExpansions;
    return GridSolveEnd::Unreachable;
}

// Expands the oldest cell of the flood from the goal. A cell joins the flood by a step the search from the
// start can take the other way: the same diagonal rule, step limits and footprint test. A cell with a finite
// GScore is on the start's side and ends the flood before the footprint test: the start cell has no such test.
AStarSolver::GoalSideStep AStarSolver::ExpandGoalSide(const GridMap::ReadView& grid, uint32 agentCellRadius,
                                                      std::vector<uint32>& goalSideCells, size_t& goalSideNext)
{
    if (goalSideNext == goalSideCells.size())
        return GoalSideStep::Exhausted;
    const uint32 cell = goalSideCells[goalSideNext++];

    uint32 neighbors[8];
    float32 baseCosts[8];
    uint32 neighborCount = 0;
    GetNeighbors(grid, cell, neighbors, baseCosts, neighborCount);

    uint32 cx = 0, cz = 0;
    grid.CellFromIndex(cell, cx, cz);
    const float32 cellHeight = grid.GetCellHeight(cx, cz);

    for (uint32 slot = 0; slot < neighborCount; ++slot)
    {
        const uint32 neighbor = neighbors[slot];
        if (IsGoalSide(neighbor))
            continue;
        uint32 nx = 0, nz = 0;
        grid.CellFromIndex(neighbor, nx, nz);
        if (!IsStepWithinLimits(grid, cellHeight, nx, nz, baseCosts[slot]))
            continue;
        if (GScore(neighbor) < kInfinity)
            return GoalSideStep::MetStart;
        if (!IsFootprintClear(grid, nx, nz, agentCellRadius))
            continue;
        Visit(neighbor);
        m_CellFlags[neighbor] |= kGoalSideFlag;
        goalSideCells.push_back(neighbor);
    }
    return goalSideNext == goalSideCells.size() ? GoalSideStep::Exhausted : GoalSideStep::Continue;
}

void AStarSolver::BeginGeneration()
{
    ++m_Generation;
    if (m_Generation == 0)
    {
        // The stamps wrapped: a stamp left from 65,535 solves ago would read as current.
        std::fill(m_VisitStamp.begin(), m_VisitStamp.end(), uint16{0});
        m_Generation = 1;
    }
}

void AStarSolver::Visit(uint32 cellIndex)
{
    if (m_VisitStamp[cellIndex] == m_Generation)
        return;
    m_VisitStamp[cellIndex] = m_Generation;
    m_GScore[cellIndex] = kInfinity;
    m_Parent[cellIndex] = UINT32_MAX;
    m_CellFlags[cellIndex] = 0;
}

bool AStarSolver::IsClosed(uint32 cellIndex) const
{
    return m_VisitStamp[cellIndex] == m_Generation && (m_CellFlags[cellIndex] & kClosedFlag) != 0;
}

bool AStarSolver::IsGoalSide(uint32 cellIndex) const
{
    return m_VisitStamp[cellIndex] == m_Generation && (m_CellFlags[cellIndex] & kGoalSideFlag) != 0;
}

float32 AStarSolver::GScore(uint32 cellIndex) const
{
    return m_VisitStamp[cellIndex] == m_Generation ? m_GScore[cellIndex] : kInfinity;
}

void AStarSolver::GetNeighbors(const GridMap::ReadView& grid, uint32 cellIndex,
                                uint32* outNeighbors, float32* outBaseCosts, uint32& outCount) const
{
    outCount = 0;
    uint32 cx = 0, cz = 0;
    grid.CellFromIndex(cellIndex, cx, cz);
    const auto& settings = grid.GetSettings();

    if (settings.Type == GridType::Square)
    {
        // Cardinals first (indices 0-3), then diagonals (indices 4-7).
        static constexpr int32 dx[] = {0, 0, 1, -1, 1, -1, 1, -1};
        static constexpr int32 dz[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        static constexpr float32 costs[] = {1.0f, 1.0f, 1.0f, 1.0f, kSqrt2, kSqrt2, kSqrt2, kSqrt2};
        // For each diagonal, the two cardinal neighbors that must be walkable
        // to prevent corner-cutting through blocked cells.
        // Diagonal (1,-1) needs (1,0)[idx 2] and (0,-1)[idx 0]
        // Diagonal (-1,-1) needs (-1,0)[idx 3] and (0,-1)[idx 0]
        // Diagonal (1,1) needs (1,0)[idx 2] and (0,1)[idx 1]
        // Diagonal (-1,1) needs (-1,0)[idx 3] and (0,1)[idx 1]
        static constexpr uint32 diagCardinalA[] = {2, 3, 2, 3};
        static constexpr uint32 diagCardinalB[] = {0, 0, 1, 1};

        // Track which cardinal directions are walkable for the corner-cut check.
        bool cardinalWalkable[4] = {false, false, false, false};

        // Add cardinal neighbors (always eligible)
        for (uint32 i = 0; i < 4; ++i)
        {
            const int32 nx = static_cast<int32>(cx) + dx[i];
            const int32 nz = static_cast<int32>(cz) + dz[i];
            if (nx >= 0 && nx < static_cast<int32>(settings.Width) &&
                nz >= 0 && nz < static_cast<int32>(settings.Depth))
            {
                cardinalWalkable[i] = !grid.IsCellBlocked(static_cast<uint32>(nx), static_cast<uint32>(nz));
                outNeighbors[outCount] = grid.CellIndex(static_cast<uint32>(nx), static_cast<uint32>(nz));
                outBaseCosts[outCount] = costs[i];
                ++outCount;
            }
        }

        // Add diagonal neighbors only if both adjacent cardinal cells are walkable.
        for (uint32 i = 4; i < 8; ++i)
        {
            const uint32 diagIdx = i - 4;
            if (!cardinalWalkable[diagCardinalA[diagIdx]] || !cardinalWalkable[diagCardinalB[diagIdx]])
                continue;

            const int32 nx = static_cast<int32>(cx) + dx[i];
            const int32 nz = static_cast<int32>(cz) + dz[i];
            if (nx >= 0 && nx < static_cast<int32>(settings.Width) &&
                nz >= 0 && nz < static_cast<int32>(settings.Depth))
            {
                outNeighbors[outCount] = grid.CellIndex(static_cast<uint32>(nx), static_cast<uint32>(nz));
                outBaseCosts[outCount] = costs[i];
                ++outCount;
            }
        }
    }
    else
    {
        int32 dx[6], dz[6];
        uint32 hexCount = 0;
        HexGrid::GetNeighborOffsets(cz, dx, dz, hexCount);

        for (uint32 i = 0; i < hexCount; ++i)
        {
            const int32 nx = static_cast<int32>(cx) + dx[i];
            const int32 nz = static_cast<int32>(cz) + dz[i];
            if (nx >= 0 && nx < static_cast<int32>(settings.Width) &&
                nz >= 0 && nz < static_cast<int32>(settings.Depth))
            {
                outNeighbors[outCount] = grid.CellIndex(static_cast<uint32>(nx), static_cast<uint32>(nz));
                outBaseCosts[outCount] = 1.0f;
                ++outCount;
            }
        }
    }
}

float32 AStarSolver::Heuristic(const GridMap::ReadView& grid, uint32 from, uint32 to) const
{
    uint32 fx = 0, fz = 0, tx = 0, tz = 0;
    grid.CellFromIndex(from, fx, fz);
    grid.CellFromIndex(to, tx, tz);

    if (grid.GetSettings().Type == GridType::Square)
    {
        const float32 dx = std::abs(static_cast<float32>(fx) - static_cast<float32>(tx));
        const float32 dz = std::abs(static_cast<float32>(fz) - static_cast<float32>(tz));
        return (dx + dz) + (kSqrt2 - 2.0f) * std::min(dx, dz);
    }
    else
    {
        return HexGrid::Distance(fx, fz, tx, tz);
    }
}

} // namespace GameEngine::Pathfinding
