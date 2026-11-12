#pragma once

#include "JobSystem/TaskTypes.h"

#include <cstddef>
#include <functional>
#include <limits>

namespace JobSystem
{

/**
 * @brief The options of one ParallelFor call: how many helpers it may
 * publish, on which lane, what stops it, what each unit a helper runs must
 * acquire first, and how many helpers it keeps queued while it runs. The
 * defaults give every pool worker a helper on the Normal lane, with no stop
 * poll, no admission and no helpers published after the start.
 *
 * Every pointer refers to the caller's frame; ParallelFor reads it only while
 * a unit of the run can still start, so the pointees need to live only for
 * the call.
 */
struct ParallelForOptions
{
    /** @brief The Helpers value that asks for one helper per pool worker. */
    static constexpr size_t kAllWorkers = std::numeric_limits<size_t>::max();

    /**
     * @brief The pool tasks published to claim units beside the caller;
     * kAllWorkers (the default) asks for the pool's worker count. The
     * request is clamped to 0 when the run has at most one unit, else to
     * min(units - 1, WorkStealingThreadPool::kStackBatch); 0 runs every unit
     * on the caller unless HelpersToKeepQueued publishes more.
     */
    size_t Helpers = kAllWorkers;

    /**
     * @brief Background publishes the helpers to the Background lane. Any
     * other value gives them the class of the pool body running on the
     * forking thread (Normal outside a body), so a Background body's helpers
     * stay Background and an Awaited body's helpers stay Awaited.
     */
    JobPriority HelperPriority = JobPriority::Normal;

    /**
     * @brief Polled by the caller only, before each unit it claims; true
     * stops the run (no further unit starts anywhere; units in flight
     * finish) and ParallelFor returns false. A body that must stop helpers
     * in the middle of their work returns false itself.
     */
    const std::function<bool()>* Stop = nullptr;

    /**
     * @brief A per-unit budget for helpers: TryAcquire before each unit a
     * helper runs, Release after it. The caller's own units take no
     * admission.
     */
    struct UnitAdmission
    {
        std::function<bool()> TryAcquire;
        std::function<void()> Release;
    };

    /**
     * @brief Admission for helper units, or null for none. A helper refused
     * ends and leaves the units to the caller and the other helpers. A helper
     * admitted runs one unit, releases, and re-publishes itself at the end of
     * its lane while units are left and the run is open, so the lane's other
     * jobs run between units; never once the pool's shutdown gate is set. A
     * throw from either hook stops the run like a unit's throw.
     */
    const UnitAdmission* Admission = nullptr;

    /**
     * @brief Polled by the caller before each unit it claims, after Stop: how many
     * helpers to keep queued. The caller publishes the missing ones, one batch at
     * most, within the units left and the pool's budget. Null: no helper after the start.
     */
    const std::function<size_t()>* HelpersToKeepQueued = nullptr;
};

} // namespace JobSystem
