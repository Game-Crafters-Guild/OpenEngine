#pragma once
#include "Types/Types.h"

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
 */
struct NavigationPathfindingStatistics
{
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

    // Totals since the system was created; read twice and subtract for a rate.
    uint64 TotalNewRequests = 0;
    uint64 TotalSolved = 0;
};

} // namespace GameEngine::PathfindingECS
