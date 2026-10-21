#include "Pathfinding/NavigationWorld.h"

#include "AStarSolverPool.h"
#include "Pathfinding/SolverScratchStatistics.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <limits>
#include <queue>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine::Pathfinding
{

namespace
{
// One counter for every NavigationWorld in the process. A handle a destroyed
// or sibling instance handed out can never name a map created later by another
// instance, so a stale handle held across a service rebuild resolves to nothing
// instead of an unrelated map. Relaxed: uniqueness needs no ordering.
std::atomic<uint32> s_NextMapGeneration{1};

uint32 NextMapGeneration()
{
    return s_NextMapGeneration.fetch_add(1, std::memory_order_relaxed);
}

// Spans one SolveGridPaths call: a path-solve window on every grid map (each
// holding that map's reservation lock shared on the calling thread across the
// join, so the solves read through GridMap::ReadView without a lock), then the
// active-job count raised; undone in reverse order on every exit, a solve
// that throws included. Release on the decrement pairs with the acquire load
// in HasActivePathJobs.
class PathSolveWindow
{
public:
    PathSolveWindow(std::span<GridMap* const> grids, std::atomic<uint32>& activePathJobs)
        : m_Grids(grids), m_ActivePathJobs(activePathJobs)
    {
        for (const GridMap* grid : m_Grids)
            grid->OpenPathSolveWindow();
        m_ActivePathJobs.fetch_add(1, std::memory_order_relaxed);
    }

    ~PathSolveWindow()
    {
        m_ActivePathJobs.fetch_sub(1, std::memory_order_release);
        for (const GridMap* grid : m_Grids)
            grid->ClosePathSolveWindow();
    }

    PathSolveWindow(const PathSolveWindow&) = delete;
    PathSolveWindow& operator=(const PathSolveWindow&) = delete;

private:
    std::span<GridMap* const> m_Grids;
    std::atomic<uint32>& m_ActivePathJobs;
};

} // anonymous namespace

NavigationWorld::NavigationWorld() : m_GridSolvers(std::make_unique<AStarSolverPool>()) {}
NavigationWorld::~NavigationWorld() = default;

NavigationWorld::MapEntry* NavigationWorld::GetMapEntry(NavMapHandle handle)
{
    if (handle.Index >= m_Maps.size())
        return nullptr;
    auto& entry = m_Maps[handle.Index];
    if (!entry.Active || entry.Generation != handle.Generation)
        return nullptr;
    return &entry;
}

const NavigationWorld::MapEntry* NavigationWorld::GetMapEntry(NavMapHandle handle) const
{
    if (handle.Index >= m_Maps.size())
        return nullptr;
    const auto& entry = m_Maps[handle.Index];
    if (!entry.Active || entry.Generation != handle.Generation)
        return nullptr;
    return &entry;
}

void NavigationWorld::AssertNoPathSolves() const
{
    assert(m_ActivePathJobs.load(std::memory_order_relaxed) == 0 &&
           "NavigationWorld: the map table was written while path solves read it (SolveGridPaths); "
           "add, remove and replace maps outside NavigationPathfindingSystem's update");
}

NavMapHandle NavigationWorld::AddGridMap(const GridSettings& settings)
{
    AssertNoPathSolves();
    uint32 index = 0;
    if (!m_FreeMapSlots.empty())
    {
        index = m_FreeMapSlots.back();
        m_FreeMapSlots.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_Maps.size());
        m_Maps.emplace_back();
    }

    MapEntry& entry = m_Maps[index];
    entry.Map = std::make_unique<GridMap>(settings);
    entry.Type = MapType::Grid;
    entry.Active = true;
    entry.Generation = NextMapGeneration();

    return NavMapHandle{index, entry.Generation};
}

NavMapHandle NavigationWorld::AddDetourNavMap()
{
    AssertNoPathSolves();
    uint32 index = 0;
    if (!m_FreeMapSlots.empty())
    {
        index = m_FreeMapSlots.back();
        m_FreeMapSlots.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_Maps.size());
        m_Maps.emplace_back();
    }

    MapEntry& entry = m_Maps[index];
    entry.Map = std::make_unique<DetourNavMap>();
    entry.Type = MapType::DetourNavMesh;
    entry.Active = true;
    entry.Generation = NextMapGeneration();

    return NavMapHandle{index, entry.Generation};
}

void NavigationWorld::RemoveMap(NavMapHandle handle)
{
    AssertNoPathSolves();
    MapEntry* entry = GetMapEntry(handle);
    if (!entry)
        return;

    entry->Map.reset();
    entry->Active = false;
    m_FreeMapSlots.push_back(handle.Index);
    if (entry->Type == MapType::Grid)
        ReleaseSolverScratch();
}

GridMap* NavigationWorld::ReplaceGridMap(NavMapHandle handle, std::unique_ptr<GridMap> grid)
{
    AssertNoPathSolves();
    MapEntry* entry = GetMapEntry(handle);
    if (!entry || entry->Type != MapType::Grid || !grid)
        return nullptr;
    entry->Map = std::move(grid);
    ReleaseSolverScratch();
    return static_cast<GridMap*>(entry->Map.get());
}

SolverScratchStatistics NavigationWorld::GetSolverScratchStatistics() const
{
    return m_GridSolvers->Statistics();
}

void NavigationWorld::ReleaseSolverScratch()
{
    uint32 maxCells = 0;
    for (const MapEntry& entry : m_Maps)
    {
        if (entry.Active && entry.Type == MapType::Grid)
            maxCells = std::max(maxCells, static_cast<const GridMap*>(entry.Map.get())->GetCellCount());
    }
    m_GridSolvers->ReleaseScratchAbove(maxCells);
}

GridMap* NavigationWorld::GetGridMap(NavMapHandle handle)
{
    MapEntry* entry = GetMapEntry(handle);
    if (!entry || entry->Type != MapType::Grid)
        return nullptr;
    return static_cast<GridMap*>(entry->Map.get());
}

DetourNavMap* NavigationWorld::GetDetourNavMap(NavMapHandle handle)
{
    MapEntry* entry = GetMapEntry(handle);
    if (!entry || entry->Type != MapType::DetourNavMesh)
        return nullptr;
    return static_cast<DetourNavMap*>(entry->Map.get());
}

INavigationMap* NavigationWorld::GetMap(NavMapHandle handle)
{
    MapEntry* entry = GetMapEntry(handle);
    if (!entry)
        return nullptr;
    return entry->Map.get();
}

PathStatus NavigationWorld::FindPathOnMap(const MapEntry& entry, const PathRequest& request, PathHandle& outPath)
{
    if (entry.Type != MapType::Grid)
        return entry.Map->FindPath(request, m_PathBuffer, outPath);
    const AStarSolverPool::Lease lease = m_GridSolvers->Acquire();
    return static_cast<const GridMap&>(*entry.Map).FindPath(request, lease.Solver(), m_PathBuffer, outPath);
}

PathStatus NavigationWorld::FindPath(NavMapHandle map, const PathRequest& request, PathHandle& outPath)
{
    const MapEntry* entry = GetMapEntry(map);
    if (!entry)
        return PathStatus::Failed;
    return FindPathOnMap(*entry, request, outPath);
}

void NavigationWorld::SolveGridPathLane(std::span<GridPathSolve> solves, std::atomic<size_t>& next)
{
    if (next.load(std::memory_order_relaxed) >= solves.size())
        return;
    const AStarSolverPool::Lease lease = m_GridSolvers->Acquire();
    for (size_t i = next.fetch_add(1, std::memory_order_relaxed); i < solves.size();
         i = next.fetch_add(1, std::memory_order_relaxed))
    {
        GridPathSolve& solve = solves[i];
        const GridMap* grid = GetGridMap(solve.Map);
        if (!grid)
        {
            solve.Points.clear();
            solve.Status = PathStatus::Failed;
            continue;
        }
        solve.Status = grid->FindPathPoints(solve.Request, lease.Solver(), solve.Points);
    }
}

void NavigationWorld::SolveGridPaths(JobSystem::WorkStealingThreadPool* pool, std::span<GridPathSolve> solves)
{
    if (solves.empty())
        return;

    m_SolveGrids.clear();
    for (MapEntry& entry : m_Maps)
    {
        if (entry.Active && entry.Type == MapType::Grid)
            m_SolveGrids.push_back(static_cast<GridMap*>(entry.Map.get()));
    }

    const PathSolveWindow window(m_SolveGrids, m_ActivePathJobs);
    std::atomic<size_t> next{0};
    const size_t lanes = pool ? std::min(solves.size(), pool->GetWorkerCount() + 1) : 1;
    JobSystem::ParallelFor(pool, lanes, [this, solves, &next](size_t) { SolveGridPathLane(solves, next); });
}

PathStatus NavigationWorld::FindPathAcrossLinks(const PathRequest& request, PathHandle& outPath)
{
    // Build adjacency graph from NavLinks
    // Key: NavMapHandle, Value: list of (targetMapHandle, linkEntry index)
    std::unordered_map<NavMapHandle, std::vector<std::pair<NavMapHandle, uint32>>, NavMapHandleHash> adjacency;

    for (uint32 i = 0; i < static_cast<uint32>(m_Links.size()); ++i)
    {
        const auto& linkEntry = m_Links[i];
        if (!linkEntry.Active)
            continue;

        adjacency[linkEntry.Link.SourceMap].push_back({linkEntry.Link.TargetMap, i});
        if (linkEntry.Link.Bidirectional)
        {
            adjacency[linkEntry.Link.TargetMap].push_back({linkEntry.Link.SourceMap, i});
        }
    }

    // Find which map contains the start point
    NavMapHandle startMap{};
    NavMapHandle endMap{};

    for (uint32 i = 0; i < static_cast<uint32>(m_Maps.size()); ++i)
    {
        const auto& entry = m_Maps[i];
        if (!entry.Active)
            continue;

        NavMapHandle handle{i, entry.Generation};

        // The map that holds the point: on a mesh the radius is how far from the mesh the point may
        // lie; on a grid it is a clearance, and the point's own open cell is what holds it.
        const float32 holdRadius = entry.Type == MapType::Grid ? 0.0f : request.AgentRadius;
        if (!startMap.IsValid() && entry.Map->IsPointNavigable(request.StartX, request.StartY, request.StartZ, holdRadius))
        {
            startMap = handle;
        }
        if (!endMap.IsValid() && entry.Map->IsPointNavigable(request.EndX, request.EndY, request.EndZ, holdRadius))
        {
            endMap = handle;
        }
        if (startMap.IsValid() && endMap.IsValid())
            break;
    }

    if (!startMap.IsValid() || !endMap.IsValid())
        return PathStatus::Failed;

    // If same map, direct path
    if (startMap == endMap)
    {
        const MapEntry* entry = GetMapEntry(startMap);
        if (!entry)
            return PathStatus::Failed;
        return FindPathOnMap(*entry, request, outPath);
    }

    // Dijkstra to find cheapest map sequence from startMap to endMap,
    // weighted by NavLink::TraversalCost.
    struct DijkNode
    {
        NavMapHandle Map;
        uint32 ParentIndex;
        uint32 LinkIndex;
        float32 Cost;
    };

    std::vector<DijkNode> nodes;
    std::unordered_map<NavMapHandle, float32, NavMapHandleHash> bestCost;

    nodes.push_back({startMap, UINT32_MAX, UINT32_MAX, 0.0f});
    bestCost[startMap] = 0.0f;

    // Simple priority queue via linear scan (few maps expected)
    uint32 endNodeIndex = UINT32_MAX;

    for (uint32 front = 0; front < static_cast<uint32>(nodes.size()); ++front)
    {
        // Find unvisited node with lowest cost (linear scan — fine for small graph)
        uint32 best = UINT32_MAX;
        float32 bestVal = std::numeric_limits<float32>::max();
        for (uint32 k = front; k < static_cast<uint32>(nodes.size()); ++k)
        {
            if (nodes[k].Cost < bestVal)
            {
                bestVal = nodes[k].Cost;
                best = k;
            }
        }
        if (best == UINT32_MAX)
            break;
        // Swap to front position
        if (best != front)
            std::swap(nodes[front], nodes[best]);

        const DijkNode& cur = nodes[front];

        if (cur.Map == endMap)
        {
            endNodeIndex = front;
            break;
        }

        auto it = adjacency.find(cur.Map);
        if (it == adjacency.end())
            continue;

        for (const auto& [targetMap, linkIdx] : it->second)
        {
            float32 linkCost = m_Links[linkIdx].Link.TraversalCost;
            float32 newCost = cur.Cost + linkCost;

            auto costIt = bestCost.find(targetMap);
            if (costIt != bestCost.end() && costIt->second <= newCost)
                continue;

            bestCost[targetMap] = newCost;
            nodes.push_back({targetMap, front, linkIdx, newCost});
        }
    }

    if (endNodeIndex == UINT32_MAX)
        return PathStatus::Failed;

    // Reconstruct map sequence and link sequence
    struct Segment
    {
        NavMapHandle Map;
        uint32 LinkIndex;
    };

    std::vector<Segment> mapSequence;
    uint32 nodeIdx = endNodeIndex;
    while (nodeIdx != UINT32_MAX)
    {
        mapSequence.push_back({nodes[nodeIdx].Map, nodes[nodeIdx].LinkIndex});
        nodeIdx = nodes[nodeIdx].ParentIndex;
    }
    std::reverse(mapSequence.begin(), mapSequence.end());

    // Solve path within each segment and concatenate
    std::vector<PathPoint> allPoints;

    for (uint32 s = 0; s < static_cast<uint32>(mapSequence.size()); ++s)
    {
        const MapEntry* entry = GetMapEntry(mapSequence[s].Map);
        if (!entry)
            return PathStatus::Failed;

        PathRequest segRequest;
        if (s == 0)
        {
            segRequest.StartX = request.StartX;
            segRequest.StartY = request.StartY;
            segRequest.StartZ = request.StartZ;
        }
        else
        {
            // Start from the link target point
            const auto& link = m_Links[mapSequence[s].LinkIndex].Link;
            if (link.TargetMap == mapSequence[s].Map)
            {
                segRequest.StartX = link.TargetX;
                segRequest.StartY = link.TargetY;
                segRequest.StartZ = link.TargetZ;
            }
            else
            {
                segRequest.StartX = link.SourceX;
                segRequest.StartY = link.SourceY;
                segRequest.StartZ = link.SourceZ;
            }
        }

        if (s == static_cast<uint32>(mapSequence.size()) - 1)
        {
            segRequest.EndX = request.EndX;
            segRequest.EndY = request.EndY;
            segRequest.EndZ = request.EndZ;
        }
        else
        {
            // End at the link source point on this map
            uint32 nextLinkIdx = mapSequence[s + 1].LinkIndex;
            const auto& link = m_Links[nextLinkIdx].Link;
            if (link.SourceMap == mapSequence[s].Map)
            {
                segRequest.EndX = link.SourceX;
                segRequest.EndY = link.SourceY;
                segRequest.EndZ = link.SourceZ;
            }
            else
            {
                segRequest.EndX = link.TargetX;
                segRequest.EndY = link.TargetY;
                segRequest.EndZ = link.TargetZ;
            }
        }

        segRequest.AgentRadius = request.AgentRadius;
        segRequest.AgentHeight = request.AgentHeight;
        segRequest.ExcludeAgentId = request.ExcludeAgentId;
        segRequest.CurrentTime = request.CurrentTime;
        segRequest.SmoothPath = request.SmoothPath;

        PathHandle segPath;
        PathStatus segStatus = FindPathOnMap(*entry, segRequest, segPath);
        if (segStatus == PathStatus::Failed)
            return PathStatus::Failed;

        // Append points, skipping the first point of subsequent segments to avoid duplicates
        uint32 pointCount = m_PathBuffer.GetPointCount(segPath);
        if (pointCount > 0)
        {
            std::vector<PathPoint> segPoints(pointCount);
            m_PathBuffer.CopyPoints(segPath, segPoints.data(), pointCount);
            uint32 startIdx = (s > 0) ? 1 : 0;
            for (uint32 p = startIdx; p < pointCount; ++p)
            {
                allPoints.push_back(segPoints[p]);
            }
        }

        m_PathBuffer.FreePath(segPath);
    }

    if (allPoints.empty())
        return PathStatus::Failed;

    outPath = m_PathBuffer.AllocatePath(allPoints.data(), static_cast<uint32>(allPoints.size()));
    return PathStatus::Complete;
}

bool NavigationWorld::HasActivePathJobs() const
{
    return m_ActivePathJobs.load(std::memory_order_acquire) != 0;
}

NavLinkId NavigationWorld::AddNavLink(const NavLink& link)
{
    uint32 index = 0;
    if (!m_FreeLinkSlots.empty())
    {
        index = m_FreeLinkSlots.back();
        m_FreeLinkSlots.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_Links.size());
        m_Links.emplace_back();
    }

    LinkEntry& entry = m_Links[index];
    entry.Link = link;
    entry.Active = true;
    entry.Generation = m_NextLinkGeneration++;

    return NavLinkId{index, entry.Generation};
}

void NavigationWorld::RemoveNavLink(NavLinkId id)
{
    if (id.Index >= m_Links.size())
        return;

    LinkEntry& entry = m_Links[id.Index];
    if (!entry.Active || entry.Generation != id.Generation)
        return;

    entry.Active = false;
    m_FreeLinkSlots.push_back(id.Index);
}

const NavLink* NavigationWorld::GetNavLink(NavLinkId id) const
{
    if (id.Index >= m_Links.size())
        return nullptr;
    const auto& entry = m_Links[id.Index];
    if (!entry.Active || entry.Generation != id.Generation)
        return nullptr;
    return &entry.Link;
}

uint32 NavigationWorld::GetActiveNavLinkCount() const
{
    uint32 count = 0;
    for (const auto& entry : m_Links)
    {
        if (entry.Active)
            ++count;
    }
    return count;
}

void NavigationWorld::ForEachNavLink(const NavLinkVisitor& visitor) const
{
    for (uint32 i = 0; i < static_cast<uint32>(m_Links.size()); ++i)
    {
        const auto& entry = m_Links[i];
        if (!entry.Active)
            continue;
        NavLinkId id{i, entry.Generation};
        if (!visitor(id, entry.Link))
            return;
    }
}

void NavigationWorld::AutoGenerateBoundaryLinks(NavMapHandle mapA, NavMapHandle mapB)
{
    GridMap* gridA = GetGridMap(mapA);
    GridMap* gridB = GetGridMap(mapB);

    if (!gridA || !gridB)
        return;

    const auto& settingsA = gridA->GetSettings();
    const auto& settingsB = gridB->GetSettings();

    // Check all boundary cells of map A against boundary cells of map B
    // A boundary cell is on the edge of the grid

    // For each edge cell on map A, compute its world position and check if
    // it's adjacent to an edge cell on map B (within tolerance of one cell size)
    const float32 tolerance = std::max(settingsA.CellSize, settingsB.CellSize) * 0.75f;

    // Collect boundary cells from map A
    struct BoundaryCell
    {
        uint32 CellX;
        uint32 CellZ;
        float32 WorldX;
        float32 WorldZ;
    };

    auto collectBoundaryCells = [](const GridMap* grid, const GridSettings& settings) -> std::vector<BoundaryCell>
    {
        std::vector<BoundaryCell> cells;
        cells.reserve((settings.Width + settings.Depth) * 2);

        for (uint32 x = 0; x < settings.Width; ++x)
        {
            for (uint32 z = 0; z < settings.Depth; ++z)
            {
                if (x == 0 || x == settings.Width - 1 || z == 0 || z == settings.Depth - 1)
                {
                    if (grid->IsCellBlocked(x, z))
                        continue;

                    float32 worldX = 0.0f, worldZ = 0.0f;
                    grid->CellToWorld(x, z, worldX, worldZ);
                    cells.push_back({x, z, worldX, worldZ});
                }
            }
        }
        return cells;
    };

    auto boundaryCellsA = collectBoundaryCells(gridA, settingsA);
    auto boundaryCellsB = collectBoundaryCells(gridB, settingsB);

    // Sort B cells by WorldX to enable binary-search narrowing: O(N*log(M) + M*log(M))
    std::sort(boundaryCellsB.begin(), boundaryCellsB.end(),
              [](const BoundaryCell& a, const BoundaryCell& b) { return a.WorldX < b.WorldX; });

    const float32 toleranceSq = tolerance * tolerance;

    for (const auto& cellA : boundaryCellsA)
    {
        // Find the range of B cells whose WorldX is within tolerance of cellA.WorldX
        const float32 minX = cellA.WorldX - tolerance;
        const float32 maxX = cellA.WorldX + tolerance;

        auto lower = std::lower_bound(boundaryCellsB.begin(), boundaryCellsB.end(), minX,
                                       [](const BoundaryCell& cell, float32 val) { return cell.WorldX < val; });

        for (auto it = lower; it != boundaryCellsB.end() && it->WorldX <= maxX; ++it)
        {
            const float32 dx = cellA.WorldX - it->WorldX;
            const float32 dz = cellA.WorldZ - it->WorldZ;
            const float32 distSq = dx * dx + dz * dz;

            if (distSq <= toleranceSq)
            {
                NavLink link;
                link.SourceMap = mapA;
                link.SourceX = cellA.WorldX;
                link.SourceY = gridA->GetCellHeight(cellA.CellX, cellA.CellZ);
                link.SourceZ = cellA.WorldZ;
                link.TargetMap = mapB;
                link.TargetX = it->WorldX;
                link.TargetY = gridB->GetCellHeight(it->CellX, it->CellZ);
                link.TargetZ = it->WorldZ;
                link.TraversalCost = 1.0f;
                link.Bidirectional = true;

                AddNavLink(link);
            }
        }
    }
}

PathBuffer& NavigationWorld::GetPathBuffer()
{
    return m_PathBuffer;
}

const PathBuffer& NavigationWorld::GetPathBuffer() const
{
    return m_PathBuffer;
}

CrowdManager& NavigationWorld::GetCrowdManager()
{
    return m_CrowdManager;
}

} // namespace GameEngine::Pathfinding
