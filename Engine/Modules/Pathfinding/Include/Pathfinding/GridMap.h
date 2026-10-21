#pragma once

#include "Pathfinding/INavigationMap.h"
#include "Pathfinding/PathfindingTypes.h"

#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace GameEngine::Pathfinding
{

class AStarSolver;

class GridMap : public INavigationMap
{
public:
    // The cells from (MinX, MinZ) to (MaxX, MaxZ), both corners included.
    struct CellRange
    {
        uint32 MinX = 0;
        uint32 MinZ = 0;
        uint32 MaxX = 0;
        uint32 MaxZ = 0;

        bool Contains(uint32 cellX, uint32 cellZ) const
        {
            return cellX >= MinX && cellX <= MaxX && cellZ >= MinZ && cellZ <= MaxZ;
        }
    };

    // Read-only access to the cell arrays for path solves: the settings by
    // value and raw pointers to the five arrays, with inline accessors and no
    // lock. A view is valid only inside a path-solve window, which holds the
    // reservation lock shared and keeps every writer out (see
    // OpenPathSolveWindow); take one per window with View().
    class ReadView
    {
    public:
        const GridSettings& GetSettings() const { return m_Settings; }
        uint32 GetCellCount() const { return m_Settings.Width * m_Settings.Depth; }
        uint32 CellIndex(uint32 cellX, uint32 cellZ) const { return cellZ * m_Settings.Width + cellX; }
        void CellFromIndex(uint32 index, uint32& outCellX, uint32& outCellZ) const
        {
            outCellX = index % m_Settings.Width;
            outCellZ = index / m_Settings.Width;
        }
        bool IsValidCell(uint32 cellX, uint32 cellZ) const
        {
            return cellX < m_Settings.Width && cellZ < m_Settings.Depth;
        }
        // Static or dynamic; a cell off the grid counts as blocked.
        bool IsCellBlocked(uint32 cellX, uint32 cellZ) const
        {
            if (!IsValidCell(cellX, cellZ))
                return true;
            const uint32 index = CellIndex(cellX, cellZ);
            return m_Blocked[index] != 0 || m_DynamicBlocked[index] != 0;
        }
        float32 GetCellHeight(uint32 cellX, uint32 cellZ) const
        {
            return IsValidCell(cellX, cellZ) ? m_Heights[CellIndex(cellX, cellZ)] : 0.0f;
        }
        float32 GetCellCost(uint32 cellX, uint32 cellZ) const
        {
            return IsValidCell(cellX, cellZ) ? m_Costs[CellIndex(cellX, cellZ)] : 1.0f;
        }
        CellReservation Reservation(uint32 cellX, uint32 cellZ) const
        {
            return IsValidCell(cellX, cellZ) ? m_Reservations[CellIndex(cellX, cellZ)] : CellReservation{};
        }

        bool WorldToCell(float32 worldX, float32 worldZ, uint32& outCellX, uint32& outCellZ) const;
        void CellToWorld(uint32 cellX, uint32 cellZ, float32& outWorldX, float32& outWorldZ) const;
        // First blocked cell on the grid line from start to end, corners
        // included; GridMap::Raycast's contract.
        bool Raycast(float32 startX, float32 startZ, float32 endX, float32 endZ,
                     float32& hitX, float32& hitY, float32& hitZ) const;
        // True when the start's cell is open and a disc of `radius` moved
        // from start to end stays on the grid and at least `radius` from
        // every blocked cell's rectangle. A zero radius tests the line as
        // Raycast does. Blocked cells inside `openCells` count as open; it
        // applies to a radius above zero only.
        bool IsSegmentClear(float32 startX, float32 startZ, float32 endX, float32 endZ,
                            float32 radius, const CellRange* openCells = nullptr) const;

    private:
        friend class GridMap;
        explicit ReadView(const GridMap& map);

        GridSettings m_Settings;
        const float32* m_Costs;
        const float32* m_Heights;
        const uint8* m_Blocked;
        const uint8* m_DynamicBlocked;
        const CellReservation* m_Reservations;
    };

    explicit GridMap(const GridSettings& settings);

    // INavigationMap. Opens a path-solve window for the one solve.
    // GridMap::FindPath called directly, on a standalone grid map or on a
    // world's grid map through GetGridMap/GetMap, solves on the calling
    // thread's solver (static thread_local). That scratch lives as long as the
    // thread, and the world neither frees nor reports it. Solve through
    // NavigationWorld::FindPath or SolveGridPaths to use the world's solvers.
    PathStatus FindPath(const PathRequest& request, PathBuffer& pathBuffer, PathHandle& outPath) const override;
    // True when the point's cell is open and a disc of `radius` around the
    // point stays on the grid and at least `radius` from every blocked cell's
    // rectangle (ReadView::IsSegmentClear with start and end at the point).
    bool IsPointNavigable(float32 x, float32 y, float32 z, float32 radius) const override;
    bool GetClosestNavigablePoint(float32 x, float32 y, float32 z, float32 searchRadius,
                                   float32& outX, float32& outY, float32& outZ) const override;
    bool Raycast(float32 startX, float32 startY, float32 startZ,
                 float32 endX, float32 endY, float32 endZ,
                 float32& hitX, float32& hitY, float32& hitZ) const override;
    // ReadView::IsSegmentClear on this map. Like Raycast it reads only the
    // blocked arrays, so it needs no path-solve window.
    bool IsSegmentClear(float32 startX, float32 startZ, float32 endX, float32 endZ,
                        float32 radius, const CellRange* openCells = nullptr) const;

    // Configuration
    const GridSettings& GetSettings() const;
    uint32 GetCellCount() const;
    uint32 GetTopologyVersion() const;

    // Cell data
    float32 GetCellCost(uint32 cellX, uint32 cellZ) const;
    void SetCellCost(uint32 cellX, uint32 cellZ, float32 cost);
    bool IsCellBlocked(uint32 cellX, uint32 cellZ) const;
    void SetCellBlocked(uint32 cellX, uint32 cellZ, bool blocked);
    float32 GetCellHeight(uint32 cellX, uint32 cellZ) const;
    void SetCellHeight(uint32 cellX, uint32 cellZ, float32 height);

    // Dynamic obstacle blocking (cleared and rebuilt each frame)
    void SetCellDynamicBlocked(uint32 cellX, uint32 cellZ, bool blocked);
    void ClearAllDynamicBlocked();
    bool IsCellDynamicBlocked(uint32 cellX, uint32 cellZ) const;

    // Reservations (for grid-based obstacle avoidance)
    void ReserveCell(uint32 cellX, uint32 cellZ, uint32 agentId, float32 expirationTime,
                     bool occupancy = false);
    void ClearCellReservation(uint32 cellX, uint32 cellZ);
    void ClearReservationsForAgent(uint32 agentId);
    CellReservation GetCellReservation(uint32 cellX, uint32 cellZ) const;
    void ClearExpiredReservations(float32 currentTime);
    bool IsOccupiedByOther(uint32 cellX, uint32 cellZ, uint32 excludeAgentId,
                           float32 currentTime) const;
    // Live agents that currently have at least one stamped cell. Peak
    // concurrent, not lifetime spawned ids.
    uint32 GetReservedAgentCount() const;

    // Coordinate conversion
    bool WorldToCell(float32 worldX, float32 worldZ, uint32& outCellX, uint32& outCellZ) const;
    void CellToWorld(uint32 cellX, uint32 cellZ, float32& outWorldX, float32& outWorldZ) const;

    // Cells covered either side of the centre by an agent of this radius: zero
    // while the agent fits inside one cell, otherwise ceil(radius / cellSize).
    // Stamping a body and testing for one share this single definition, so they
    // cannot disagree about which cells a footprint covers.
    uint32 FootprintRadiusInCells(float32 agentRadius) const;

    // Index helpers
    uint32 CellIndex(uint32 cellX, uint32 cellZ) const;
    void CellFromIndex(uint32 index, uint32& outCellX, uint32& outCellZ) const;
    bool IsValidCell(uint32 cellX, uint32 cellZ) const;

    // Get neighbor cell coordinates for the given cell.
    // Returns the count of valid neighbors written to outCellX/outCellZ arrays.
    // Arrays must be at least 8 elements (max 8 for square, 6 for hex).
    uint32 GetNeighborCells(uint32 cellX, uint32 cellZ,
                            uint32* outCellX, uint32* outCellZ, uint32 maxNeighbors) const;

    // A path-solve window spans path solves that read this map through a
    // ReadView, from one thread (FindPath) or from several
    // (NavigationWorld::SolveGridPaths). Opening it takes the reservation lock
    // shared, once for the whole window, so a reservation writer on another
    // thread waits for the close; the other arrays are not synchronised, so
    // every writer asserts (Debug, DebugFast) that no window is open. Each open
    // is matched by one close on the same thread, and a thread does not open a
    // second window on a map while it holds one (the shared lock is not
    // recursive).
    void OpenPathSolveWindow() const;
    void ClosePathSolveWindow() const;
    // The cells as a ReadView; asserts (Debug, DebugFast) that a path-solve
    // window is open.
    ReadView View() const;

    // Debug data (raw arrays, size = Width * Depth)
    const float32* GetCostData() const;
    const uint8* GetBlockedData() const;

private:
    // NavigationWorld solves its grid maps on solvers it owns.
    friend class NavigationWorld;

    // FindPath on `solver`'s scratch.
    PathStatus FindPath(const PathRequest& request, AStarSolver& solver, PathBuffer& pathBuffer,
                        PathHandle& outPath) const;
    // FindPath on `solver`'s scratch, without the PathBuffer and without a
    // window of its own: called inside an open path-solve window, it reads
    // through View(), writes a Complete path's points to outPoints (cleared
    // first) and nothing else, so solves on several threads, each on its own
    // solver, may run at once inside one window.
    PathStatus FindPathPoints(const PathRequest& request, AStarSolver& solver, std::vector<PathPoint>& outPoints) const;

    GridSettings m_Settings;
    std::vector<float32> m_Costs;
    std::vector<float32> m_Heights;
    std::vector<uint8> m_Blocked;
    std::vector<uint8> m_DynamicBlocked;
    std::vector<CellReservation> m_Reservations;

    // Per-agent cell indices so ClearReservationsForAgent is O(stamped cells),
    // not O(Width*Depth). Keyed by AgentId so ReserveCell is O(1) lookup,
    // not a linear scan of every live agent. Stale indices are skipped if
    // the cell was stolen. Erased on clear so size is live agents.
    std::unordered_map<uint32, std::vector<uint32>> m_AgentCells;

    // Guards m_Reservations and m_AgentCells. Writers (ReserveCell,
    // ClearCellReservation, ClearReservationsForAgent, ClearExpiredReservations)
    // take a unique_lock; readers (GetCellReservation / IsOccupiedByOther)
    // take a shared_lock per call, and a path-solve window holds one shared
    // lock for all of its solves' reads.
    mutable std::shared_mutex m_ReservationMutex;
    std::atomic<uint32> m_TopologyVersion{0};
    mutable std::atomic<uint32> m_PathSolveWindows{0};

    void AssertNoPathSolveWindow() const;
};

} // namespace GameEngine::Pathfinding
