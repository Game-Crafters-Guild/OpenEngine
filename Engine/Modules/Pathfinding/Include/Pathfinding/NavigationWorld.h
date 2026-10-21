#pragma once

#include "Pathfinding/PathfindingTypes.h"
#include "Pathfinding/PathBuffer.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/ObstacleAvoidance.h"

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
} // namespace JobSystem

namespace GameEngine::Pathfinding
{

class AStarSolverPool;
struct SolverScratchStatistics;

// Thread safety: NavigationWorld is NOT internally synchronized for map/link
// management. Map add/remove, NavLink mutations and map content writes happen
// on one thread (the main thread). PathBuffer operations ARE mutex-protected.
// The only reads from other threads are SolveGridPaths' solves, which run and
// finish inside that call: nothing may write a map or the map table while it
// runs (HasActivePathJobs() is true then, and every GridMap writer and every
// map add, remove or replace asserts). Grid path solves of the world run on
// A* solvers the world owns, one per solve running at once; GridMap::FindPath
// called directly, on any grid map, uses the calling thread's solver.
class NavigationWorld
{
public:
    NavigationWorld();
    ~NavigationWorld();

    // Map management. Removing or replacing a grid map frees the scratch of
    // the world's idle grid A* solvers beyond what the largest grid map left
    // needs.
    NavMapHandle AddGridMap(const GridSettings& settings);
    NavMapHandle AddDetourNavMap();
    void RemoveMap(NavMapHandle handle);
    // Replaces the grid `handle` names with `grid`, keeping the handle, so whatever is bound to the
    // grid stays bound across a re-bake. Null, leaving the old grid in place, when `handle` names no
    // grid.
    GridMap* ReplaceGridMap(NavMapHandle handle, std::unique_ptr<GridMap> grid);

    // This world's grid A* solver scratch, read by a game's memory budget and by the editor's
    // navigation statistics. Takes the solvers' mutex.
    SolverScratchStatistics GetSolverScratchStatistics() const;

    GridMap* GetGridMap(NavMapHandle handle);
    DetourNavMap* GetDetourNavMap(NavMapHandle handle);
    INavigationMap* GetMap(NavMapHandle handle);

    // Synchronous path query (blocks)
    PathStatus FindPath(NavMapHandle map, const PathRequest& request, PathHandle& outPath);

    // Solves every grid request in `solves` before it returns: one unit of
    // JobSystem::ParallelFor per lane on `pool`, at most one lane per worker
    // plus the calling thread, each lane on one of the world's solvers and
    // claiming solves one at a time, so a long solve does not hold back the
    // rest; a null pool solves them on the caller in order. Each solve writes only
    // its own Status and Points (Failed for a handle that names no grid map)
    // and allocates nothing in the PathBuffer, so the results do not depend
    // on the pool or on timing. Opens a path-solve window on every grid map
    // for the call, which takes each map's reservation lock once on the
    // calling thread; the solves read through GridMap::ReadView, unlocked.
    void SolveGridPaths(JobSystem::WorkStealingThreadPool* pool, std::span<GridPathSolve> solves);

    // Cross-map pathfinding via NavLinks (blocks): finds the cheapest map
    // sequence over the links, solves each segment and joins them into one
    // path. Failed when either end is on no map or no sequence or segment
    // solves.
    PathStatus FindPathAcrossLinks(const PathRequest& request, PathHandle& outPath);

    // True while SolveGridPaths runs solves; false at every other time,
    // including between frames. A false return orders every finished solve's
    // map reads before the caller's subsequent writes (acquire load paired
    // with the release decrement at the end of the call).
    bool HasActivePathJobs() const;

    // NavLink management
    NavLinkId AddNavLink(const NavLink& link);
    void RemoveNavLink(NavLinkId id);
    void AutoGenerateBoundaryLinks(NavMapHandle mapA, NavMapHandle mapB);

    // NavLink queries
    const NavLink* GetNavLink(NavLinkId id) const;
    uint32 GetActiveNavLinkCount() const;

    using NavLinkVisitor = std::function<bool(NavLinkId, const NavLink&)>;
    void ForEachNavLink(const NavLinkVisitor& visitor) const;

    // Path buffer access
    PathBuffer& GetPathBuffer();
    const PathBuffer& GetPathBuffer() const;

    // Crowd manager for Detour-based obstacle avoidance
    CrowdManager& GetCrowdManager();

private:
    enum class MapType : uint8 { Grid, DetourNavMesh };
    struct MapEntry
    {
        std::unique_ptr<INavigationMap> Map;
        MapType Type = MapType::Grid;
        bool Active = false;
        uint32 Generation = 0;
    };

    struct LinkEntry
    {
        NavLink Link;
        bool Active = false;
        uint32 Generation = 0;
    };

    MapEntry* GetMapEntry(NavMapHandle handle);
    const MapEntry* GetMapEntry(NavMapHandle handle) const;
    void AssertNoPathSolves() const;
    // One lane of SolveGridPaths: leases one of the world's solvers, then claims solves from `next`
    // one at a time until none are left, writing only the results of the solves it claims.
    void SolveGridPathLane(std::span<GridPathSolve> solves, std::atomic<size_t>& next);
    // A path on one map; a grid map solves on one of the world's solvers.
    PathStatus FindPathOnMap(const MapEntry& entry, const PathRequest& request, PathHandle& outPath);
    // Frees the idle solvers' scratch above what the largest active grid map needs.
    void ReleaseSolverScratch();

    std::vector<MapEntry> m_Maps;
    std::vector<uint32> m_FreeMapSlots;

    std::vector<LinkEntry> m_Links;
    std::vector<uint32> m_FreeLinkSlots;
    uint32 m_NextLinkGeneration = 1;

    PathBuffer m_PathBuffer;
    CrowdManager m_CrowdManager;
    std::unique_ptr<AStarSolverPool> m_GridSolvers;

    // Nonzero while SolveGridPaths runs. Raised before the first solve starts;
    // lowered (release) after the last one has finished.
    std::atomic<uint32> m_ActivePathJobs{0};
    // The grid maps SolveGridPaths opens its window on, refilled per call; a
    // member so that a call allocates nothing once it has grown.
    std::vector<GridMap*> m_SolveGrids;
};

} // namespace GameEngine::Pathfinding
