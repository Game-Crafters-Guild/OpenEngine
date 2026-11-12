#pragma once

#include "JobSystem/Types.h"
#include <cstdint>

namespace JobSystem {

/**
 * @brief Task execution status enumeration
 */
enum class TaskStatus : uint8 {
    Pending = 0,    // Task is waiting to be executed
    Running = 1,    // Task is currently being executed
    Completed = 2,  // Task completed successfully
    Failed = 3,     // Task failed during execution
    Cancelled = 4   // Task was cancelled before completion
};

/**
 * @brief Pool scheduling class for Submit(F&&) / EnqueueWork / EnqueueWorkBatch.
 *
 * Orders dequeue between the pool's three lanes: Awaited, then Normal, then
 * Background. Normal is the default and the hot path.
 * Awaited is for a job that a thread is blocked waiting for (a frame thread
 * that waits for one pipeline build). Every consumer polls its lane first, so
 * a Normal or Background flood delays it only by the remainder of the bodies
 * already running. Keep it for short jobs with a waiter: an Awaited flood
 * postpones Normal work.
 * Background rides a dedicated global lane that every consumer polls last; it
 * runs on idle capacity, and a saturating Normal flood postpones it
 * indefinitely (there is no aging). The class orders dequeue only: shutdown,
 * cancellation and the never-drop contracts are identical for every class
 * (see WorkStealingThreadPool's class comment).
 */
enum class JobPriority : uint8 {
    Normal = 0,
    Background = 1,
    Awaited = 2
};

// The number of JobPriority values: one pool lane each.
inline constexpr size_t kJobPriorityCount = 3;

} // namespace JobSystem
