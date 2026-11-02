#pragma once
#include "Pathfinding/PathfindingTypes.h"
#include "Types/Types.h"

#include <array>

namespace GameEngine::PathfindingECS
{

/**
 * What NavigationPathfindingSystem's solve budget did, for the newest Update and
 * in total since the system was created.
 *
 * A request is an agent with HasDestination and either PathDirty or
 * ReplanRequested set. It joins the queue the first Update it is seen, keeps
 * its place until it is solved, and leaves the queue when solved or when
 * HasDestination, or both request flags, are cleared. Waits are
 * counted from the Update a request joined: a request solved in the Update it
 * joined waited 0 frames.
 *
 * The grid solve fields count the solves on grid maps
 * (NavigationWorld::SolveGridPaths); a solve on a Detour map is not counted.
 */
struct NavigationPathfindingStatistics
{
    /// One grid solve and the Update that ran it.
    struct RankedGridSolve
    {
        uint64 Frame = 0;
        float32 Seconds = 0.0f; ///< System time of the Update, the sum of its deltaTime arguments.
        Pathfinding::GridSolveRecord Record;
    };

    /// The most grid solves WorstGridSolves keeps.
    static constexpr uint32 kWorstGridSolveCount = 8;
    /// How long, in seconds of system time, a grid solve stays in WorstGridSolves.
    static constexpr float32 kWorstGridSolveSeconds = 8.0f;

    // Updates run so far.
    uint64 Frames = 0;

    // The newest Update.
    uint32 NewRequests = 0;        ///< Requests that joined the queue.
    uint32 Solved = 0;             ///< Requests solved.
    uint32 Backlog = 0;            ///< Requests left queued for a later Update.
    uint32 OldestWaitFrames = 0;   ///< How long the oldest request still queued has waited; 0 when none is.
    float32 OldestWaitSeconds = 0.0f;
    uint32 LongestSolvedWaitFrames = 0; ///< The longest wait among the requests solved.
    float32 LongestSolvedWaitSeconds = 0.0f;
    uint32 GridSolves = 0;                  ///< Grid solves among the requests solved.
    uint64 GridSolveExpansions = 0;         ///< Cells the grid solves closed, summed.
    uint32 MostGridSolveExpansions = 0;     ///< The most cells one grid solve closed.
    uint32 SlowestGridSolveMicroseconds = 0;
    std::array<uint32, Pathfinding::kGridSolveEndCount> GridSolvesByEnd{}; ///< Indexed by GridSolveEnd.

    // Totals since the system was created; read twice and subtract for a rate.
    uint64 TotalNewRequests = 0;
    uint64 TotalSolved = 0;
    uint64 TotalGridSolveExpansions = 0;
    std::array<uint64, Pathfinding::kGridSolveEndCount> TotalGridSolvesByEnd{};

    // The grid solves that closed the most cells, most first; on a tie the slower solve ranks first.
    // A full list drops its last entry. An entry leaves kWorstGridSolveSeconds after its Update.
    // Only the first WorstGridSolveCount entries are valid.
    uint32 WorstGridSolveCount = 0;
    std::array<RankedGridSolve, kWorstGridSolveCount> WorstGridSolves{};
};

} // namespace GameEngine::PathfindingECS
