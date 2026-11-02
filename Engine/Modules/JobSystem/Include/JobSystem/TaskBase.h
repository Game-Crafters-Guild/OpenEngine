#pragma once

#include "JobSystem/Types.h"

namespace JobSystem
{

/**
 * @brief Base of every envelope in the pool's queues: one virtual (Execute), the task id and three capability
 * bits. A bare envelope is 24 bytes plus its closure, so every in-tree closure fits a 128-byte slab
 * (TaskSlabPool.h). The graph path uses Detail::SubmitGraphTask (TaskEnvelopes.h).
 */
class TaskBase
{
  public:
    virtual ~TaskBase() = default;
    virtual void Execute() = 0;

    /** @brief Unique task identifier (0 for bare fire-and-forget envelopes). */
    TaskId GetTaskId() const { return m_TaskId; }

    /** @brief True when the task is registered in the dependency graph. */
    bool RequiresDependencyManagement() const
    {
        return (m_Flags & kFlagDependencyManaged) != 0;
    }

    /** @brief True when the task participates in TaskHandle/TaskData tracking. */
    bool NeedsTaskData() const { return (m_Flags & kFlagNeedsTaskData) != 0; }

    /**
     * @brief True only for Detail::HandleTask envelopes (the Submit(F&&) path), which carry their TaskData
     * inline. Only HandleTask sets the flag, so a true value licenses the static_cast<Detail::HandleTask*> in
     * ExecuteTaskOptimized and the shutdown drain.
     */
    bool HasInlineTaskData() const { return (m_Flags & kFlagInlineTaskData) != 0; }

  protected:
    enum TaskFlags : uint8
    {
        kFlagNone = 0,
        kFlagNeedsTaskData = 1 << 0,
        kFlagDependencyManaged = 1 << 1,
        kFlagInlineTaskData = 1 << 2,
    };

    TaskBase(TaskId taskId, uint8 flags) : m_TaskId(taskId), m_Flags(flags) {}

  private:
    TaskId m_TaskId;
    uint8 m_Flags;
};

} // namespace JobSystem
