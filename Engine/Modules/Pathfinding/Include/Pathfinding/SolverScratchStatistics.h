#pragma once
#include "Types/Types.h"

namespace GameEngine::Pathfinding
{

/**
 * One NavigationWorld's grid A* solver scratch, as
 * NavigationWorld::GetSolverScratchStatistics reports it.
 *
 * The world keeps one solver per grid path solve running at once, reused
 * across solves; a solver's scratch (11 bytes per cell) grows to the largest
 * grid it has solved on. Removing or replacing a grid map frees the scratch of
 * the world's idle solvers that hold more than its largest remaining grid map
 * needs (the next solve regrows it), and destroying the world frees all of it.
 */
struct SolverScratchStatistics
{
    uint32 Solvers = 0;                ///< The world's solvers: the most grid path solves it has run at once.
    uint32 SolversHoldingScratch = 0;  ///< Solvers holding any scratch.
    uint64 HeldBytes = 0;              ///< Scratch bytes the solvers hold, by capacity, as of each one's last solve or release.
    uint64 SolverHighWaterBytes = 0;   ///< The most scratch one of the world's solvers has held.
};

} // namespace GameEngine::Pathfinding
