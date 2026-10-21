#pragma once

#include "Pathfinding/GridMap.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::Pathfinding
{

// A* solver with reusable scratch memory. Each instance owns its own working
// buffers, so concurrent solves on different AStarSolver instances are safe.
// For best performance, reuse the same solver across multiple Solve() calls
// (the buffers grow to the largest grid solved on and are reused without
// reallocation).
class AStarSolver
{
public:
    // Solves A* on the grid through a view taken inside a path-solve window.
    // Working memory is owned by this solver instance. Thread-safe as long as
    // each thread uses its own AStarSolver.
    bool Solve(const GridMap::ReadView& grid, uint32 startIndex, uint32 endIndex,
               uint32 excludeAgentId, float32 currentTime,
               uint32 agentCellRadius,
               std::vector<uint32>& outPathCellIndices);

    // The bytes the buffers hold, by capacity.
    uint64 HeldBytes() const;
    // Frees the buffers when they hold more than `maxCells` cells. Not
    // concurrent with Solve on this instance; the next Solve regrows them.
    void ReleaseScratchAbove(uint32 maxCells);

private:
    void GetNeighbors(const GridMap::ReadView& grid, uint32 cellIndex,
                      uint32* outNeighbors, float32* outBaseCosts, uint32& outCount) const;
    float32 Heuristic(const GridMap::ReadView& grid, uint32 from, uint32 to) const;
    void BeginGeneration();
    void Visit(uint32 cellIndex);
    bool IsClosed(uint32 cellIndex) const;
    float32 GScore(uint32 cellIndex) const;

    // Reusable working buffers: they grow to exactly the largest grid solved on
    // and shrink only to empty, in ReleaseScratchAbove.
    // OpenList is stack-local in Solve() since it's cleared each call.
    // A cell's GScore, Parent and Closed belong to the current solve only when
    // its m_VisitStamp equals m_Generation; any other stamp reads as unvisited,
    // so a solve does not clear the buffers. Generation 0 is never current:
    // cells added by a resize are stamped 0.
    std::vector<float32> m_GScore;
    std::vector<uint32> m_Parent;
    std::vector<uint8> m_Closed;
    std::vector<uint16> m_VisitStamp;
    uint16 m_Generation = 0;
};

} // namespace GameEngine::Pathfinding
