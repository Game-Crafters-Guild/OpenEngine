#pragma once

// CompileConcurrencyGate: a global admission gate that bounds how many shader
// compile tasks run concurrently on the job pool.
//
// Cold scene/model loads submit one prewarm task per material (each compiling a
// material's full variant set sequentially) plus per-material base compiles and
// draw-time variant misses. Left unbounded, ~one-task-per-material saturates
// every logical core with 10-30s shaderc compiles, so the (non-worker) main
// thread and the extraction/streaming workers starve and the editor stops
// responding to IPC for tens of seconds.
//
// The gate caps the number of in-flight compile tasks at a value fixed when it is
// created, which leaves two pool workers to the frame (ComputeShaderCompileConcurrencyCap). It
// deliberately does NOT block a worker to enforce the cap (that would convert
// compile oversubscription into worker-pool occupancy by sleepers and starve
// extraction of worker lanes). Instead it admits up to `cap` tasks to the pool
// and holds the remainder in a CPU-side pending queue, draining it as running
// tasks finish. No worker ever sleeps waiting for a slot, and the submitting
// (main) thread never blocks — Submit returns immediately whether the task ran
// now or was queued.
//
// Owned by MaterialSystem; every MaterialSystem::SubmitTrackedPrewarm call
// routes through it. That covers the cold-load background shaderc work — variant
// prewarm, base compile, and the publish-gate variant-miss warm-up. It does NOT
// cover MaterialCompiler::RequestCompile (the editor material-inspector preview
// build), which compiles straight through BuildMaterialToShaderPackage by
// design: that path is user-paced and low-volume, not a cold-load burst.

#include "JobSystem/TaskTypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

namespace GameEngine
{
namespace Engine::Renderer
{

// Concurrency cap for shader compiles: the physical cores minus two (at least two) or the
// GE_SHADER_COMPILE_CAP value, then at most workerCount - 2 and at least 1, so two pool workers
// stay free for the frame's fork-join. A workerCount of 0 (no pool) skips the pool bound.
uint32_t ComputeShaderCompileConcurrencyCap(size_t workerCount);

class CompileConcurrencyGate
{
  public:
    // Enqueues `work` onto the pool. `priority` selects the pool lane; the
    // gate keeps it for queued work, so bulk warm-up stays on the Background lane when it is dispatched.
    using DispatchFn = std::function<void(std::function<void()>, JobSystem::JobPriority)>;

    CompileConcurrencyGate(uint32_t cap, DispatchFn dispatch)
        : m_Cap(cap == 0 ? 1 : cap), m_Dispatch(std::move(dispatch))
    {
    }

    // Admit `work` if fewer than `cap` tasks are running, else hold it in the
    // pending queue for its lane. Admitted work is dispatched via the injected
    // executor and, on completion, frees its slot and pumps the next pending
    // task (Normal lane first, as the pool). Awaited work has a waiting thread: it takes
    // no slot and goes to the executor at once. Never runs `work` inline on the calling
    // thread. If the dispatch executor throws while admitting (task-envelope
    // allocation failure), the reserved slot is released, any concurrently
    // stranded pending job is pumped, and the exception propagates.
    void Submit(std::function<void()> work, JobSystem::JobPriority priority);

    // Shutdown: destroys every queued task unrun, outside the lock, so its guards release their
    // accounting. A later Submit destroys its work unrun. Running tasks finish and admit nothing.
    // Returns the count of queued tasks destroyed.
    size_t Close();

    uint32_t Cap() const { return m_Cap; }
    uint32_t Running() const
    {
        std::lock_guard lock(m_Mutex);
        return m_Running;
    }
    // Peak concurrent running count observed — the invariant the stress test
    // asserts (never exceeds Cap()).
    uint32_t HighWater() const
    {
        std::lock_guard lock(m_Mutex);
        return m_HighWater;
    }
    size_t PendingCount() const
    {
        std::lock_guard lock(m_Mutex);
        return m_PendingBackground.size() + m_PendingNormal.size();
    }

  private:
    // Wrap `work` so its completion releases the slot and pumps the queue, then
    // hand it to the dispatch executor. May throw if the executor throws.
    void DispatchTracked(std::function<void()> work, JobSystem::JobPriority priority);
    // Called from a dispatched task's completion: release one slot and promote
    // the next pending job (if any). noexcept — runs from a destructor context.
    void OnJobFinished() noexcept;

    const uint32_t m_Cap;
    DispatchFn m_Dispatch;

    mutable std::mutex m_Mutex;
    uint32_t m_Running = 0;
    uint32_t m_HighWater = 0;
    // Two lanes in the pool's order: Normal holds the compiles and pipeline builds a load or a
    // frame waits on, Background holds bulk project warm-up. OnJobFinished drains Normal first,
    // so a slot does not go to Background work while Normal work waits in the gate.
    std::deque<std::function<void()>> m_PendingBackground;
    std::deque<std::function<void()>> m_PendingNormal;
    // Set by Close: Submit destroys its work instead of queueing or admitting it.
    bool m_Closed = false;
};

} // namespace Engine::Renderer
} // namespace GameEngine
