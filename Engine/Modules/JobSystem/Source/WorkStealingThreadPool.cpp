#include "JobSystem/WorkStealingThreadPool.h"
#include "BlockingThreads.h"
#include "JobChannelState.h"
#include "JobSystem/TaskDependencyGraph.h"
#include "JobSystem/TaskSlabPool.h"
#include "Memory/ScratchScope.h"
#include "Platform/Thread.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace JobSystem {

// MSVC pads an over-aligned class, and every struct that holds one by value, and reports C4324, an error in this
// build; test fixtures hold pools by value. So the pool's line groups use char padding, not alignas.
static_assert(alignof(WorkStealingThreadPool) <= alignof(std::max_align_t),
              "WorkStealingThreadPool must not be over-aligned");

namespace
{
    // Adaptive spin-before-sleep defaults (overridable via env / test hook).
    // A spin window of 0 disables the spin path entirely.
    constexpr uint32 kDefaultSpinWindowUs = 100;
    constexpr uint32 kDefaultMaxSpinners = 2;

    // Spin-loop shape: the steal ring is polled only every Nth iteration (it
    // touches other workers' queue cache lines); the pause ramp caps at
    // kSpinPauseMax back-to-back pause instructions; a scheduler yield is
    // inserted periodically so a spinner never monopolizes a hyperthread pair.
    constexpr uint32 kSpinStealPeriod = 8;
    constexpr uint32 kSpinPauseMax = 32;
    constexpr uint32 kSpinYieldPeriod = 64;

    // Workers per word of the idle mask.
    constexpr size_t kIdleMaskBits = 64;

    // Inline-mode stack bound: nested execute-at-publish frames allowed on a
    // thread before further tasks are deferred to the trampoline queue. Deep
    // enough for short chains; joins explicitly drain deferred work so reaching
    // the bound cannot strand a child. The bound fits the 1 MiB wasm stack.
    constexpr int kMaxInlineExecutionDepth = 64;

    // The occupancy names of the compute classes. Static literals, so a
    // reader compares by pointer.
    constexpr const char kNormalJobName[] = "Normal";
    constexpr const char kAwaitedJobName[] = "Awaited";
    constexpr const char kBackgroundJobName[] = "Background";

    struct DeferredInlineTask
    {
        WorkStealingThreadPool* Pool;
        UniquePtr<TaskBase> Task;
        // The class given at publish (or the channel), which the deferred
        // execution runs the body as.
        const char* Channel;
        JobPriority Class;
    };

    struct InlineTrampoline
    {
        int Depth = 0;
        std::deque<DeferredInlineTask> Deferred;
    };

    InlineTrampoline& GetInlineTrampoline()
    {
        // The stack budget belongs to the thread. Each pool still owns its
        // queued envelopes and must consume them before it is destroyed.
        thread_local InlineTrampoline trampoline;
        return trampoline;
    }

    uint32 ReadEnvU32(const char* name, uint32 defaultValue)
    {
        if (const char* env = std::getenv(name))
        {
            char* end = nullptr;
            const unsigned long v = std::strtoul(env, &end, 10);
            if (end != env)
            {
                return static_cast<uint32>(v);
            }
        }
        return defaultValue;
    }

    inline void CpuRelax()
    {
#if defined(_MSC_VER)
#if defined(_M_ARM64) || defined(_M_ARM)
        __yield();
#else
        _mm_pause();
#endif
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        asm volatile("yield");
#else
        std::this_thread::yield();
#endif
    }
} // namespace

// Thread-local storage for worker ID and fast access.
// s_CurrentPool is never cleared on thread exit: compute workers and blocking
// threads terminate at their join and are never recycled across pools.
thread_local size_t WorkStealingThreadPool::s_CurrentWorkerId = SIZE_MAX;
thread_local WorkStealingThreadPool::Worker* WorkStealingThreadPool::s_CurrentWorker = nullptr;
thread_local WorkStealingThreadPool* WorkStealingThreadPool::s_CurrentPool = nullptr;
thread_local uint32* WorkStealingThreadPool::s_PublishSink = nullptr;
thread_local WorkStealingThreadPool::ThreadJobState* WorkStealingThreadPool::s_JobState = nullptr;
thread_local WorkStealingThreadPool::ThreadJobState WorkStealingThreadPool::s_OwnJobState;

namespace
{
    // What the occupancy slot and GetCurrentJobForTests name a body by.
    template <typename Context>
    const char* JobName(const Context& context)
    {
        if (context.Channel != nullptr)
        {
            return context.Channel;
        }
        switch (context.Class) {
            case JobPriority::Awaited:
                return kAwaitedJobName;
            case JobPriority::Background:
                return kBackgroundJobName;
            case JobPriority::Normal:
                break;
        }
        return kNormalJobName;
    }
} // namespace

namespace
{
    template <typename Slot>
    JobSystemStatistics::Occupancy ReadOccupancy(const Slot& slot, JobSystemStatistics::ThreadKind kind,
                                                 size_t index)
    {
        JobSystemStatistics::Occupancy entry;
        entry.Kind = kind;
        entry.Index = static_cast<uint32>(index);
        entry.Running = slot.Running.load(std::memory_order_relaxed);
        // Only Background and channel jobs carry a start time (JobBodyScope).
        const bool timed =
            entry.Running != nullptr && entry.Running != kNormalJobName && entry.Running != kAwaitedJobName;
        entry.SinceNs = timed ? slot.SinceNs.load(std::memory_order_relaxed) : 0;
        return entry;
    }

    const char* LimitName(AvailableProcessors::Limit limit)
    {
        return limit == AvailableProcessors::Limit::AffinityMask ? "the process affinity mask" : "the machine";
    }
} // namespace

uint64 WorkStealingThreadPool::SteadyClockNs() {
    return static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

WorkStealingThreadPool::JobBodyScope::JobBodyScope(JobContext context)
    : m_State(CurrentJobState()), m_Enclosing(m_State) {
    // Background and channel jobs are the ones that run long enough for a
    // start time to matter; a Normal job (a 1 us ECS task) never reads the
    // clock. A body nested in a body of the same context (a tagged job its
    // Wait runs) keeps the enclosing start: the slot reports how long the
    // outermost body of that context has run, as the hang watchdog reads it.
    const bool timed = context.Channel != nullptr || context.Class == JobPriority::Background;
    const bool sameContext = m_Enclosing.Depth > 0 && m_Enclosing.Context.Channel == context.Channel &&
                             m_Enclosing.Context.Class == context.Class;
    m_State.Context = context;
    m_State.SinceNs = !timed ? 0 : sameContext ? m_Enclosing.SinceNs : SteadyClockNs();
    ++m_State.Depth;
    if (OccupancySlot* slot = m_State.Slot) {
        if (timed && !sameContext) {
            slot->SinceNs.store(m_State.SinceNs, std::memory_order_relaxed);
        }
        slot->Running.store(JobName(context), std::memory_order_relaxed);
    }
}

WorkStealingThreadPool::JobBodyScope::~JobBodyScope() {
    m_State.Context = m_Enclosing.Context;
    m_State.SinceNs = m_Enclosing.SinceNs;
    m_State.Depth = m_Enclosing.Depth;
    if (OccupancySlot* slot = m_State.Slot) {
        if (m_Enclosing.Depth == 0) {
            slot->Running.store(nullptr, std::memory_order_relaxed);
            return;
        }
        if (m_Enclosing.SinceNs != 0) {
            slot->SinceNs.store(m_Enclosing.SinceNs, std::memory_order_relaxed);
        }
        slot->Running.store(JobName(m_Enclosing.Context), std::memory_order_relaxed);
    }
}

WorkStealingThreadPool::JobContext WorkStealingThreadPool::CallerContext() {
    const ThreadJobState& state = CurrentJobState();
    return state.Depth > 0 ? state.Context : ComputeContext(JobPriority::Normal);
}

bool WorkStealingThreadPool::IsExecutingJob() {
    return CurrentJobState().Depth > 0;
}

const char* WorkStealingThreadPool::GetCurrentJobForTests() {
    const ThreadJobState& state = CurrentJobState();
    return state.Depth > 0 ? JobName(state.Context) : nullptr;
}

WorkStealingThreadPool::ThreadJobState& WorkStealingThreadPool::CurrentJobState() {
    ThreadJobState* state = s_JobState;
    return state != nullptr ? *state : s_OwnJobState;
}

// CleanupTaskData() pushes here without a lock; the same thread drains at its next GetOrCreateTaskData()
// or when its share reaches kPendingCleanupBatch, so the registry mutex stays off the completion path.
// An idle worker keeps up to kPendingCleanupBatch - 1 finished tasks until ReleaseFinishedTasks or that drain.
//
// Entries are TAGGED with their owning pool and drained ONLY by it. One
// thread legitimately produces cleanups for several pools (sequential test
// pools; cross-pool submits — CrossPoolSubmitTest), and the untagged drain
// used to clear the WHOLE buffer, silently dropping foreign pools' ids and
// leaking their registry entries until pool destruction. The pool pointer is
// an identity token ONLY — never dereferenced — so residue for a destroyed
// pool is inert: Shutdown() purges the shutting-down thread's own entries,
// ids are never reused across pools (m_NextTaskId is process-global), and an
// address-aliased later pool can therefore never match a stale id's entry in
// its registry.
struct PendingCleanupEntry
{
    const WorkStealingThreadPool* Pool;
    TaskId Id;
};
thread_local std::vector<PendingCleanupEntry> t_pendingCleanup;

// Static task ID counter
std::atomic<TaskId> WorkStealingThreadPool::m_NextTaskId{1};

WorkStealingThreadPool::WorkStealingThreadPool(size_t numThreads, size_t blockingThreadBudget) {
    // offsetof on a class that is not standard-layout is conditionally supported: clang and GCC warn, and support it.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
    constexpr size_t queuesOffset = offsetof(WorkStealingThreadPool, m_Queues);
    // The reference layout from m_Queues to m_Counters: the pool's timings are measured at its offsets. Lead puts Queues
    // at m_Queues's offset modulo alignof(std::max_align_t), so the copy pads as the pool does at any pointer size.
    // m_PadKeepsCounterPlacement puts m_Counters where this layout does, a full line after the park handles.
    struct MeasuredLayout
    {
        char Lead[alignof(std::max_align_t) + queuesOffset % alignof(std::max_align_t)];
        LaneQueues Queues;
        std::condition_variable Condition;
        std::mutex Mutex;
        CounterLines Counters;
    };
    constexpr size_t handlesEnd = offsetof(WorkStealingThreadPool, m_IdleMaskWords) + sizeof(m_IdleMaskWords);
    constexpr size_t counterOffset = offsetof(WorkStealingThreadPool, m_Counters);
    constexpr size_t measuredQueuesToCounters = offsetof(MeasuredLayout, Counters) - offsetof(MeasuredLayout, Queues);
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    static_assert(counterOffset == queuesOffset + measuredQueuesToCounters,
                  "m_PadKeepsCounterPlacement no longer puts m_Counters at #7045's offset");
    static_assert(handlesEnd + GameEngine::kFalseSharingSeparation <= counterOffset + offsetof(CounterLines, QueuedTasks),
                  "the park handles and the first counter share a line");

    if (numThreads == 0) {
        // Inline mode: no workers, no wake protocol; submissions execute on
        // the submitting thread at the publish points.
        m_InlineMode = true;
        m_DependencyGraph = MakeUnique<TaskDependencyGraph>();
        Logger::Log::Info("JobSystem: WorkStealingThreadPool in inline mode (0 workers)");
        return;
    }

    // Env knobs are read once, before any worker starts. Tests override via
    // SetSpinConfigForTest instead of mutating process env.
    m_SpinWindowUs.store(ReadEnvU32("GE_JOB_SPIN_US", kDefaultSpinWindowUs), std::memory_order_relaxed);
    m_MaxSpinners.store(ReadEnvU32("GE_JOB_MAX_SPINNERS", kDefaultMaxSpinners), std::memory_order_relaxed);

    m_DependencyGraph = MakeUnique<TaskDependencyGraph>();
    m_BlockingThreads = MakeUnique<Detail::BlockingThreads>(*this, blockingThreadBudget);

    // The slabs fork tasks can hold at once on this pool, provisioned before
    // the first fork so that no fork takes a heap allocation: the helper and
    // queue-visit envelopes (kMaxUnstartedForkTasksPerWorker unstarted in
    // each lane and one running, per worker), a run block for
    // each of them, and the forking caller's block. Each further caller
    // forking at the same time adds one block. A visit a refused Run
    // publishes takes its slab from the freelist's stock.
    const size_t claimHelperEnvelopes = (kJobPriorityCount * kMaxUnstartedForkTasksPerWorker + 1) * numThreads;
    Detail::ProvisionTaskSlabs(2 * claimHelperEnvelopes + 1);
    // Every record exists before any thread that writes one: the workers
    // start below, blocking threads on a channel's first dispatch.
    m_BlockingRecordCount = blockingThreadBudget;
    m_ThreadRecords.reset(new GameEngine::FalseSharingPadded<ThreadRecord>[numThreads + blockingThreadBudget]);
    for (size_t i = 0; i < numThreads + blockingThreadBudget; ++i) {
        ThreadRecord& record = m_ThreadRecords[i].Value;
        record.State.Slot = &record.Occupancy;
    }

    // Initialize workers using unique_ptr to avoid copy constructor issues.
    // Workers take *this for their global-lane ConsumerTokens; every lane
    // is fully constructed before this line.
    workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        workers.emplace_back(MakeUnique<Worker>(*this));
    }

    // The wake state exists before any worker thread starts.
    m_ParkSlotCount = numThreads;
    m_ParkSlots = MakeUnique<GameEngine::FalseSharingPadded<ParkSlot>[]>(numThreads);
    m_IdleMaskWords = (numThreads + kIdleMaskBits - 1) / kIdleMaskBits;
    m_IdleMask = MakeUnique<GameEngine::FalseSharingPadded<std::atomic<uint64>>[]>(m_IdleMaskWords);

    // Start worker threads with dependency-aware loop
    for (size_t i = 0; i < numThreads; ++i) {
        workers[i]->Thread = std::thread(&WorkStealingThreadPool::DependencyAwareWorkerLoop, this, i);
    }

    const AvailableProcessors available = AvailableProcessors::Query();
    Logger::Log::Info("JobSystem: WorkStealingThreadPool initialized with {} worker threads; the process may run on {} "
                      "processors (limited by {})",
                      numThreads, available.Count, LimitName(available.LimitedBy));
}

WorkStealingThreadPool::~WorkStealingThreadPool() {
#ifndef NDEBUG
    {
        std::lock_guard<std::mutex> lock(m_ChannelsMutex);
        assert(m_Channels.empty() &&
               "JobSystem: a JobChannel outlives its pool; its owning service must destroy it first");
    }
#endif
    Shutdown();
}

TaskHandle WorkStealingThreadPool::SubmitToGraph(UniquePtr<TaskBase> task,
                                                  std::span<const TaskId> dependencies) {
    if (m_ShutdownRequested.load()) {
        return TaskHandle(); // Return invalid handle
    }

    const TaskId taskId = task->GetTaskId();

    // The TaskData must exist before the task is published: completion on a
    // worker bridges status into it via ProcessGraphActions.
    TaskHandle handle(taskId, this);

    // Brand the id graph-managed BEFORE the handle can escape (it escapes at
    // return): LinkSubmitDependency dispatches on this bit, not on graph-node
    // presence — see TaskData::GraphManaged for the cascade-retire window
    // this closes.
    if (auto data = handle.GetTaskData()) {
        data->GraphManaged.store(true, std::memory_order_relaxed);
    }

    // Only Detail::SubmitGraphTask reaches this path; the other envelope
    // types never enter the graph (Submit(F&&) / EnqueueWork own them).

    // F9 submit-side critical section: node creation, edge registration
    // (absent dependency = already terminal, F8), readiness check, and
    // parking are ONE atomic step under the graph mutex. If the last
    // dependency completes concurrently, exactly one side observes the task
    // as ready — either Register returns it here, or the completer's
    // MarkCompleted extracts the parked payload (closes the B8
    // submit-vs-complete TOCTOU without any rescan).
    if (UniquePtr<TaskBase> ready = m_DependencyGraph->Register(std::move(task), dependencies)) {
        EnqueueTask(std::move(ready));
    }

    // F13e: same race as above, with one more shape — the payload can be
    // PARKED in the graph (never queued, invisible to any drain) after
    // Shutdown()'s CancelAllPending already ran. AbandonSubmitDuringShutdown's
    // direct arbitration covers it: CancelPending retires the parked payload.
    if (IsShuttingDown()) {
        AbandonSubmitDuringShutdown(taskId);
    }

    return handle;
}

bool WorkStealingThreadPool::CancelTask(TaskId taskId) {
    // Lookup only — never create. An absent registry entry means the id was
    // never submitted here or the task is terminal with its deferred cleanup
    // already drained; both refuse cancellation.
    auto data = TryGetTaskData(taskId);
    if (!data) {
        return false;
    }

    return CancelPendingArbitrated(taskId, data);
}

bool WorkStealingThreadPool::CancelPendingArbitrated(TaskId taskId,
                                                     const TaskHandle::TaskDataRef& data) {
    // F12: the TaskData status atomic is the SINGLE cancel/execute arbiter.
    // The executing worker CASes Pending -> Running at dequeue; we CAS
    // Pending -> Cancelled here. Exactly one side wins; the winner owns the
    // task's terminal events.
    TaskStatus expected = TaskStatus::Pending;
    if (!data->Status.compare_exchange_strong(expected, TaskStatus::Cancelled,
                                              std::memory_order_seq_cst)) {
        return false; // running or already terminal — the other side fires
    }

    // We won: fire the Cancelled event set on THIS thread, outside every lock
    // (F10). For graph-tracked tasks the graph critical section retires the
    // parked payload (if any), cascades pending dependents, and erases the
    // nodes; lambda-path tasks are never graph-tracked and CancelPending is a
    // no-op for them.
    TaskGraphActions actions;
    m_DependencyGraph->CancelPending(taskId, actions);

    // The winner's own fire goes to the EXACT TaskData it arbitrated on —
    // never a by-id handle lookup. The losing worker can dequeue-drop the
    // envelope and queue its registry cleanup concurrently; once another
    // thread drains that cleanup, a by-id lookup re-materializes a fresh
    // Pending TaskData and the notify/callbacks would fire on the wrong
    // object, stranding real waiters. Cascaded dependents (below) are safe to
    // resolve by id: their envelopes sit in actions.RetiredTasks until after
    // the events fire, so their registry entries cannot be cleaned yet.
    std::erase_if(actions.Events, [taskId](const TaskGraphActions::StatusEvent& event) {
        return event.Id == taskId;
    });
    TaskHandle::FireCancelledOn(data);
    ProcessGraphActions(actions, taskId, String{}, nullptr);
    return true;
}

bool WorkStealingThreadPool::LinkSubmitDependency(const TaskHandle& dependency) {
    if (!dependency.IsValid()) {
        return false; // an invalid handle adds no edge
    }
    // A foreign pool's id can never complete in THIS graph — an edge to it
    // would park the dependent forever. Contract violation; skip the edge.
    assert(dependency.m_JobSystem == this &&
           "Submit dependency handle belongs to another pool");
    if (dependency.m_JobSystem != this) {
        return false;
    }
    const TaskHandle::TaskDataRef& data = dependency.m_Data;
    if (!data || data->IsDone()) {
        // F8: terminal dependencies add no edge — the dependent runs and
        // observes the outcome through its handle, exactly like an absent
        // dependency at Register. Skipping (rather than registering an id
        // with no tombstone) also keeps the F7 diagnostic quiet for
        // already-terminal lambda handles, which the graph never saw.
        return false;
    }

    const TaskId depId = dependency.GetId();
    // GRAPH task (branded in SubmitToGraph): add a plain edge and
    // never touch the mirror machinery — its completion reaches the graph
    // natively (CompleteGraphTask), and a bridged completion would steal the
    // executing worker's own MarkCompleted, muting its handle events. The
    // dependent's Register decides node-present (edge) vs node-absent
    // (terminal, F8) atomically under the graph mutex — the ordinary B8
    // shape. Dispatching on the BIT rather than probing the graph matters:
    // a cascade-retired graph task (failure/cancel cascade, shutdown
    // CancelAllPending) has NO node but stays Pending on its TaskData until
    // ProcessGraphActions flips it OUTSIDE the graph mutex — probing in
    // that window would create a mirror for a task whose completion path is
    // already spent, and the parked dependent would strand forever (nothing
    // ever completes a graph task's mirror). In the window, Register's
    // absent=terminal reading runs the dependent.
    if (data->GraphManaged.load(std::memory_order_relaxed)) {
        return true;
    }

    // NON-graph task (plain Submit(F&&) lambda): bridge it with a mirror
    // node. EnsureTracked returning false means an earlier Submit(fn, deps)
    // already mirrors this id — the creator arms the flag and resolves the
    // Dekker recheck; this caller only needs the edge.
    if (!m_DependencyGraph->EnsureTracked(depId)) {
        return true;
    }

    // A mirror was created. Bridge protocol (seq_cst Dekker, the
    // TaskData::HasCallbacks shape):
    //   linker:    store GraphLinked   -> load Status
    //   completer: exchange Status     -> load GraphLinked
    // At least one side observes the other: either the completer sees the
    // flag and bridges the mirror terminal, or we see the terminal status
    // and resolve it here. Both may fire — MarkCompleted/CancelPending on an
    // already-erased node returns empty actions, so double-bridging is
    // benign (the graph mutex serializes the two).
    data->GraphLinked.store(true, std::memory_order_seq_cst);

    switch (data->Status.load(std::memory_order_seq_cst)) {
    case TaskStatus::Completed:
        CompleteLinkedDependency(depId, true, String{}, nullptr);
        return false;
    case TaskStatus::Failed:
        // The fast path publishes Exception (SetException) and ErrorMessage
        // (inside MarkFailed) BEFORE MarkFailed's status exchange, and our
        // seq_cst load reads from that exchange — both fields are safely
        // readable here. (Only lambda deps reach this arm; graph-managed
        // ids returned above.)
        CompleteLinkedDependency(depId, false, data->ErrorMessage, data->Exception);
        return false;
    case TaskStatus::Cancelled: {
        // The winning canceller fired the dependency's own events already;
        // if its graph CancelPending ran before our mirror existed, the
        // mirror is dangling — retire it with the graph's cancel cascade,
        // suppressing the source's events (single-fire is the canceller's).
        TaskGraphActions actions;
        m_DependencyGraph->CancelPending(depId, actions);
        std::erase_if(actions.Events, [depId](const TaskGraphActions::StatusEvent& event) {
            return event.Id == depId;
        });
        ProcessGraphActions(actions, depId, String{}, nullptr);
        return false;
    }
    default:
        return true; // live (Pending/Running): the edge orders the dependent behind it
    }
}

void WorkStealingThreadPool::CompleteLinkedDependency(TaskId taskId, bool success,
                                                      const String& failureMessage,
                                                      std::exception_ptr failureException) {
    // No status pre-store (CompleteGraphTask's F9 preamble): the source's
    // TaskData is already terminal — that is exactly what licensed this call.
    TaskGraphActions actions = m_DependencyGraph->MarkCompleted(taskId, success);
    // The source's own terminal events already fired on its fast path
    // (MarkCompleted/MarkFailed) — only the graph-side effects (dependent
    // releases, failure cascades) are ours. Without the strip, ProcessGraph-
    // Actions would re-run the source's user callbacks.
    std::erase_if(actions.Events, [taskId](const TaskGraphActions::StatusEvent& event) {
        return event.Id == taskId;
    });
    ProcessGraphActions(actions, taskId, failureMessage, failureException);
}

void WorkStealingThreadPool::AbandonSubmitDuringShutdown(TaskId taskId) {
    // F13e: this submitter passed the gate, published, and then observed
    // shutdown — Shutdown()'s drain/graph-retire/registry-sweep may all have
    // completed before the publish landed, so nothing else will ever consume
    // the envelope or flip the TaskData. Self-drain first (consumes and
    // cancels the queued envelope, ours included, plus any other stranded
    // work), then arbitrate our own task directly: the drain only reaches
    // QUEUED envelopes, so a payload parked in the graph needs the explicit
    // CancelPendingArbitrated (whose CancelPending retires it). If a racing
    // drain or the registry sweep already fired our task, the CAS inside
    // loses and this is a no-op — single-fire is preserved.
    DrainQueuesOnThisThread();
    if (auto data = TryGetTaskData(taskId)) {
        CancelPendingArbitrated(taskId, data);
    }
}

namespace {
// A Submit task holds one envelope slab and one TaskData, so this queue keeps as many TaskData
// as the slab freelist keeps slabs. It holds void*, because TaskHandle::TaskData is private.
using TaskDataQueue = FreelistQueue<void*, kTaskSlabPoolCapacity>;

// The reset TaskData that wait for their next task. It is never destroyed, because a handle can
// release its TaskData during static destruction.
TaskDataQueue& TaskDataFreelist() {
    static auto* const s_Freelist = new TaskDataQueue(kTaskSlabPoolCapacity);
    return *s_Freelist;
}
} // namespace

TaskHandle::TaskDataRef TaskHandle::TaskDataRef::Acquire() {
    void* data = nullptr;
    if (TaskDataFreelist().try_dequeue(data)) {
        return TaskDataRef(static_cast<TaskData*>(data));
    }
    return TaskDataRef(MakeUnique<TaskData>().release());
}

void TaskHandle::TaskDataRef::Recycle(TaskData* data) noexcept {
    // The last owner runs this, so no other thread can read the fields. The reset can run user
    // destructors (the result, the callbacks).
    assert(data->Waiters.load(std::memory_order_relaxed) == 0 && "JobSystem: recycling a TaskData with a registered waiter");
    data->Status.store(TaskStatus::Pending, std::memory_order_relaxed);
    data->Result.reset();
    data->Exception = nullptr;
    data->ErrorMessage.clear();
    data->OnCompleteCallbacks.clear();
    data->OnCompleteWithHandleCallbacks.clear();
    data->OnFailureCallbacks.clear();
    data->HasCallbacks.store(false, std::memory_order_relaxed);
    data->GraphLinked.store(false, std::memory_order_relaxed);
    data->GraphManaged.store(false, std::memory_order_relaxed);
    data->Owners.store(1, std::memory_order_relaxed);
    if (!TaskDataFreelist().try_enqueue(data)) {
        const UniquePtr<TaskData> overflow(data); // The freelist is full: delete the TaskData.
    }
}

void WorkStealingThreadPool::TaskDataReleaseBatch::Add(TaskHandle::TaskDataRef data) {
    assert(Count < kPendingCleanupBatch &&
           "TaskDataReleaseBatch: a drain erases at most kPendingCleanupBatch entries; CleanupTaskData drains at that size");
    Entries[Count++] = std::move(data);
}

TaskHandle::TaskDataRef WorkStealingThreadPool::GetOrCreateTaskData(TaskId taskId) {
    TaskHandle::TaskDataRef result;
    TaskDataReleaseBatch released;
    {
        std::lock_guard<std::mutex> lock(m_TaskDataMutex);

        // Drain deferred cleanups under the same lock as the insert. This is
        // amortized free for callers that hit the lock anyway; the per-task
        // CleanupTaskData() fast path stays lock-free.
        DrainPendingCleanupLocked(released);

        auto it = m_TaskDataRegistry.find(taskId);
        if (it != m_TaskDataRegistry.end()) {
            result = it->second;
        } else {
            result = TaskHandle::TaskDataRef::Acquire();
            m_TaskDataRegistry[taskId] = result;
        }
    }
    return result;
}

void WorkStealingThreadPool::DrainPendingCleanupLocked(TaskDataReleaseBatch& released) {
    // Consume only this pool's entries; the entries of other pools stay for
    // their own drains (pointer equality only, see the buffer comment). No
    // user code runs in the predicate, so erase_if iterates a stable buffer.
    std::erase_if(t_pendingCleanup, [&](const PendingCleanupEntry& entry) {
        if (entry.Pool != this) {
            return false;
        }
        auto it = m_TaskDataRegistry.find(entry.Id);
        if (it == m_TaskDataRegistry.end()) {
            return true; // duplicate cleanup — a prior drain already erased it
        }
        released.Add(std::move(it->second));
        m_TaskDataRegistry.erase(it);
        return true;
    });
}

TaskHandle::TaskDataRef WorkStealingThreadPool::TryGetTaskData(TaskId taskId) const {
    std::lock_guard<std::mutex> lock(m_TaskDataMutex);
    auto it = m_TaskDataRegistry.find(taskId);
    return it != m_TaskDataRegistry.end() ? it->second : nullptr;
}

TaskId WorkStealingThreadPool::GenerateTaskId() {
    return m_NextTaskId.fetch_add(1, std::memory_order_relaxed);
}

size_t WorkStealingThreadPool::GetGraphTaskCountForTests() const {
    return m_DependencyGraph->GetTrackedTaskCountForTests();
}

void WorkStealingThreadPool::SetSpinConfigForTest(uint32 spinWindowUs, uint32 maxSpinners) {
    m_SpinWindowUs.store(spinWindowUs, std::memory_order_relaxed);
    m_MaxSpinners.store(maxSpinners, std::memory_order_relaxed);
}

uint32 WorkStealingThreadPool::GetSpinningCountForTests() const {
    return m_Counters.NumSpinning.load(std::memory_order_seq_cst);
}

uint32 WorkStealingThreadPool::GetIdleCountForTests() const {
    uint32 idle = 0;
    for (size_t i = 0; i < m_IdleMaskWords; ++i) {
        idle += static_cast<uint32>(std::popcount(m_IdleMask[i].Value.load(std::memory_order_seq_cst)));
    }
    return idle;
}

uint64 WorkStealingThreadPool::GetBatchNotifyCountForTests() const {
    return m_Counters.BatchNotifiesForTests.load(std::memory_order_relaxed);
}

size_t WorkStealingThreadPool::GetTaskDataRegistrySizeForTests() const {
    std::lock_guard<std::mutex> lock(m_TaskDataMutex);
    return m_TaskDataRegistry.size();
}

uint32 WorkStealingThreadPool::GetBackgroundQueuedForTests() const {
    return m_Counters.BackgroundQueued.load(std::memory_order_relaxed);
}

void WorkStealingThreadPool::SetSleepBackstopForTest(uint32 backstopUs) {
    m_SleepBackstopUs.store(backstopUs, std::memory_order_relaxed);
}

JobSystemStatistics WorkStealingThreadPool::GetStatistics() const {
    JobSystemStatistics stats;
    stats.ComputeWorkers = static_cast<uint32>(workers.size());
    stats.BlockingThreads = m_BlockingThreads ? m_BlockingThreads->ThreadCount() : 0;

    stats.Threads.resize(workers.size() + m_BlockingRecordCount);
    stats.Threads.resize(SnapshotOccupancy(stats.Threads));
    for (const JobSystemStatistics::Occupancy& thread : stats.Threads) {
        if (thread.Kind != JobSystemStatistics::ThreadKind::Worker || thread.Running == nullptr) {
            continue;
        }
        JobSystemStatistics::Lane& lane = thread.Running == kBackgroundJobName ? stats.Background
                                          : thread.Running == kAwaitedJobName  ? stats.Awaited
                                                                               : stats.Normal;
        ++lane.Running;
    }
    // m_Counters.QueuedTasks counts every lane; an occupancy counter can run ahead of
    // it during a producer's publish window, hence the floor.
    const uint32 queued = m_Counters.QueuedTasks.load(std::memory_order_relaxed);
    const uint32 awaited = m_Counters.AwaitedQueued.load(std::memory_order_relaxed);
    const uint32 background = m_Counters.BackgroundQueued.load(std::memory_order_relaxed);
    stats.Normal.Queued = queued > awaited + background ? queued - awaited - background : 0;
    stats.Awaited.Queued = awaited;
    stats.Background.Queued = background;

    // Strong references under the registry mutex, each channel read under
    // its own mutex outside it: a channel's mutex is a leaf.
    Vector<SharedPtr<Detail::JobChannelState>> channels;
    {
        std::lock_guard<std::mutex> lock(m_ChannelsMutex);
        channels.reserve(m_Channels.size());
        for (const WeakPtr<Detail::JobChannelState>& channel : m_Channels) {
            if (SharedPtr<Detail::JobChannelState> live = channel.lock()) {
                channels.push_back(std::move(live));
            }
        }
    }
    stats.Channels.reserve(channels.size());
    for (const SharedPtr<Detail::JobChannelState>& channel : channels) {
        stats.Channels.push_back(channel->Statistics());
    }

    stats.GlobalPushes = m_Census.GlobalPushes.load(std::memory_order_relaxed);
    stats.LocalPushes = m_Census.LocalPushes.load(std::memory_order_relaxed);
    stats.AwaitedPushes = m_Census.AwaitedPushes.load(std::memory_order_relaxed);
    stats.BackgroundPushes = m_Census.BackgroundPushes.load(std::memory_order_relaxed);
    stats.GlobalPops = m_Census.GlobalPops.load(std::memory_order_relaxed);
    stats.LocalPops = m_Census.LocalPops.load(std::memory_order_relaxed);
    stats.StealPops = m_Census.StealPops.load(std::memory_order_relaxed);
    stats.AwaitedPops = m_Census.AwaitedPops.load(std::memory_order_relaxed);
    stats.BackgroundPops = m_Census.BackgroundPops.load(std::memory_order_relaxed);
    stats.StealMisses = m_Census.StealMisses.load(std::memory_order_relaxed);
    stats.EmptyLoopsWithBacklog = m_Census.EmptyLoopsWithBacklog.load(std::memory_order_relaxed);
    stats.BackstopTimeouts = m_Census.BackstopTimeouts.load(std::memory_order_relaxed);
    stats.TasksExecuted = stats.GlobalPops + stats.LocalPops + stats.StealPops + stats.AwaitedPops +
                          stats.BackgroundPops + (m_BlockingThreads ? m_BlockingThreads->JobsRun() : 0);
    return stats;
}

size_t WorkStealingThreadPool::SnapshotOccupancy(std::span<JobSystemStatistics::Occupancy> out) const {
    if (!m_ThreadRecords) {
        return 0;
    }
    size_t written = 0;
    for (size_t i = 0; i < workers.size() && written < out.size(); ++i) {
        out[written++] = ReadOccupancy(m_ThreadRecords[i].Value.Occupancy, JobSystemStatistics::ThreadKind::Worker, i);
    }
    for (size_t i = 0; i < m_BlockingRecordCount && written < out.size(); ++i) {
        const OccupancySlot& slot = m_ThreadRecords[workers.size() + i].Value.Occupancy;
        if (slot.Live.load(std::memory_order_relaxed)) {
            out[written++] = ReadOccupancy(slot, JobSystemStatistics::ThreadKind::Blocking, i);
        }
    }
    return written;
}

void WorkStealingThreadPool::BindBlockingThread(size_t index) {
    s_CurrentPool = this;
    if (index < m_BlockingRecordCount) {
        ThreadRecord& record = m_ThreadRecords[workers.size() + index].Value;
        s_JobState = &record.State;
        record.Occupancy.Live.store(true, std::memory_order_relaxed);
    }
}

uint64 WorkStealingThreadPool::GetPublishedJobCount() const {
    return m_Census.GlobalPushes.load(std::memory_order_relaxed) +
           m_Census.LocalPushes.load(std::memory_order_relaxed) +
           m_Census.AwaitedPushes.load(std::memory_order_relaxed) +
           m_Census.BackgroundPushes.load(std::memory_order_relaxed);
}

WorkStealingThreadPool::PublishCountScope::PublishCountScope(uint32& sink)
    : m_Enclosing(s_PublishSink) {
    s_PublishSink = &sink;
}

WorkStealingThreadPool::PublishCountScope::~PublishCountScope() {
    s_PublishSink = m_Enclosing;
}

void WorkStealingThreadPool::CountPublishedOnThisThread(size_t count) {
    if (uint32* sink = s_PublishSink) {
        *sink += static_cast<uint32>(count);
    }
}

size_t WorkStealingThreadPool::GetCurrentWorkerId() const {
    return s_CurrentWorkerId;
}

bool WorkStealingThreadPool::IsComputeWorkerOf(const WorkStealingThreadPool* pool) {
    return pool != nullptr && s_CurrentPool == pool && s_CurrentWorkerId != SIZE_MAX;
}

size_t WorkStealingThreadPool::ReserveForkTasks(std::atomic<uint32>& unstarted, size_t wanted, size_t budget) {
    const size_t before = unstarted.fetch_add(static_cast<uint32>(wanted), std::memory_order_relaxed);
    const size_t granted = before >= budget ? 0 : std::min(wanted, budget - before);
    if (granted < wanted)
        unstarted.fetch_sub(static_cast<uint32>(wanted - granted), std::memory_order_relaxed);
    return granted;
}

#if GE_DEBUG_INSTRUMENTATION
namespace {
std::atomic<void (*)(WorkStealingThreadPool::TaggedVisitPoint)> g_TaggedVisitHook{nullptr};
} // namespace

void WorkStealingThreadPool::SetTaggedVisitHookForTests(void (*hook)(TaggedVisitPoint point)) {
    g_TaggedVisitHook.store(hook, std::memory_order_release);
}

namespace {
std::atomic<void (*)(WorkStealingThreadPool::WakePoint, size_t)> g_WakeHook{nullptr};

void CallWakeHook(WorkStealingThreadPool::WakePoint point, size_t workerId) {
    if (auto hook = g_WakeHook.load(std::memory_order_acquire)) {
        hook(point, workerId);
    }
}
} // namespace

void WorkStealingThreadPool::SetWakeHookForTests(void (*hook)(WakePoint point, size_t workerId)) {
    g_WakeHook.store(hook, std::memory_order_release);
}

WorkStealingThreadPool::WakeCountsForTests WorkStealingThreadPool::GetWakeCountsForTests() const {
    WakeCountsForTests counts;
    counts.Claims = m_WakeClaimsForTests.load(std::memory_order_relaxed);
    counts.Consumed = m_WakeConsumedForTests.load(std::memory_order_relaxed);
    counts.Merged = m_WakeMergedForTests.load(std::memory_order_relaxed);
    counts.StaleConsumed = m_WakeStaleForTests.load(std::memory_order_relaxed);
    for (size_t i = 0; i < m_ParkSlotCount; ++i) {
        ParkSlot& slot = m_ParkSlots[i].Value;
        std::lock_guard<std::mutex> lock(slot.Mutex);
        counts.FlagsSet += slot.Signaled ? 1 : 0;
    }
    return counts;
}
#endif

// A worker's visit to a tagged queue object, not to a counter: it holds the
// object alive (a holder, JobCounter::TaggedJobs::State) but never keeps it
// from being recycled, and when it runs it drains whatever counter's
// envelopes occupy the object at that moment; each envelope decrements its
// own counter. So a stale visit, published for a counter already joined,
// may run a later counter's job at the lane it was published at.
//
// A visit counts in the object's PendingVisits from its publish until it has
// proven the object empty, and a Run the budget refuses publishes a visit
// only when none is pending: every envelope runs on a worker without a
// waiter. The proof is a Dekker pair with that Run: the visit decrements
// PendingVisits and then reloads JobCount; the Run increments JobCount (its
// critical section), fences, and then loads PendingVisits. Either the Run
// sees no visit pending and publishes one, or the visit sees the Run's job
// and drains again.
//
// A budgeted visit holds one count of its lane's fork-task budget, given back
// as its first act, so the count bounds unstarted visits only. Move-only; an
// envelope destroyed without running gives its count back, stops counting
// itself pending and drops its hold.
class WorkStealingThreadPool::TaggedQueueVisit {
  public:
    TaggedQueueVisit(JobCounter::TaggedJobs& jobs, std::atomic<uint32>* unstarted)
        : m_Jobs(&jobs), m_Unstarted(unstarted) {
        jobs.PendingVisits.fetch_add(1, std::memory_order_relaxed);
        jobs.State.fetch_add(JobCounter::TaggedJobs::kOneHolder, std::memory_order_relaxed);
    }
    TaggedQueueVisit(TaggedQueueVisit&& other) noexcept
        : m_Jobs(std::exchange(other.m_Jobs, nullptr)), m_Unstarted(std::exchange(other.m_Unstarted, nullptr)) {}
    TaggedQueueVisit(const TaggedQueueVisit&) = delete;
    TaggedQueueVisit& operator=(const TaggedQueueVisit&) = delete;
    TaggedQueueVisit& operator=(TaggedQueueVisit&&) = delete;
    ~TaggedQueueVisit() {
        GiveBackUnstartedCount();
        if (m_Jobs) {
            m_Jobs->PendingVisits.fetch_sub(1, std::memory_order_relaxed);
            JobCounter::DropTaggedJobsHolder(std::exchange(m_Jobs, nullptr));
        }
    }

    void operator()() {
        GiveBackUnstartedCount();
        JobCounter::TaggedJobs& jobs = *m_Jobs;
        for (;;) {
            while (JobCounter::RunOneTagged(jobs)) {
            }
            jobs.PendingVisits.fetch_sub(1, std::memory_order_seq_cst);
#if GE_DEBUG_INSTRUMENTATION
            if (auto hook = g_TaggedVisitHook.load(std::memory_order_acquire))
                hook(TaggedVisitPoint::ProvedEmpty);
#endif
            if (jobs.JobCount.load(std::memory_order_seq_cst) == 0)
                break;
            jobs.PendingVisits.fetch_add(1, std::memory_order_relaxed);
        }
        // The hold drops after the visit's last touch of the object.
        JobCounter::DropTaggedJobsHolder(std::exchange(m_Jobs, nullptr));
    }

  private:
    void GiveBackUnstartedCount() {
        if (m_Unstarted)
            std::exchange(m_Unstarted, nullptr)->fetch_sub(1, std::memory_order_relaxed);
    }

    JobCounter::TaggedJobs* m_Jobs;
    std::atomic<uint32>* m_Unstarted;
};

void WorkStealingThreadPool::PublishTaggedQueueVisit(JobCounter::TaggedJobs& jobs, JobPriority lane) {
    std::atomic<uint32>& unstarted = m_Counters.UnstartedForkTasks[static_cast<size_t>(lane)];
    if (ReserveForkTasks(unstarted, 1, kMaxUnstartedForkTasksPerWorker * GetWorkerCount()) == 1) {
        EnqueueWork(TaggedQueueVisit(jobs, &unstarted), lane);
        return;
    }
    // Refused: the Run's side of the Dekker pair with a leaving visit
    // (TaggedQueueVisit), after the critical section's JobCount increment.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (jobs.PendingVisits.load(std::memory_order_seq_cst) == 0)
        EnqueueWork(TaggedQueueVisit(jobs, nullptr), lane);
}

void WorkStealingThreadPool::Wait(JobCounter& counter) {
    if (m_InlineMode)
        DrainInlineTasksOnThisThread();

    // Every zero observation that lets this function RETURN is followed by an
    // empty lock/unlock of the counter mutex (or happens under it): zero is
    // only ever published from inside a Decrement critical section, so the
    // lock orders the last notifier's completion before the caller can
    // destroy the (typically stack-owned) counter — the verbatim-binding
    // stack-lifetime rationale in JobCounter.h.
    auto synchronizedZero = [&counter] {
        if (!counter.IsZero()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(counter.m_Mutex);
        return true;
    };

    // Every waiter, on every thread, executes ONLY tasks tagged with this
    // counter (design decision D1). The pool queues and wake protocol stay
    // untouched. A thread that cannot block parks in reported slices below.
    const bool canBlock = GameEngine::Platform::CanBlockCurrentThread();
    JobCounter::TaggedJobsRef jobs;
    {
        std::lock_guard<std::mutex> lock(counter.m_Mutex);
        jobs = counter.m_Jobs;
    }
    for (;;) {
        if (synchronizedZero()) {
            return;
        }
        if (jobs) {
            // A tagged job run here is a body of the waiter's own current
            // context: the class of the body that waits, Normal outside one.
            JobBodyScope body(CallerContext());
            if (JobCounter::RunOneTagged(*jobs)) {
                continue;
            }
        }

        // Nothing findable (RunOneTagged returned false — tagged occupancy
        // provably zero at that instant): the remaining same-counter tasks
        // are in-flight on other threads, or a Run() is mid-publish. Park on
        // the COUNTER's condvar. The predicate re-reads m_Jobs under the
        // mutex (the queue can be created by a concurrent first Run()) and
        // tests the OCCUPANCY count only — the actual pop happens outside
        // the lock via RunOneTagged's retry loop on the next iteration, so
        // moodycamel's spurious dequeue failures can neither strand the park
        // nor spin under the mutex. Paired with Run()'s publish critical
        // section — occupancy increment, tagged enqueue, and notify all under
        // this same mutex — this closes the publish-vs-park window (§7
        // Hazard-B shape): either the predicate sees the fresh occupancy, or
        // the waiter is already atomically blocked when the publish notify
        // lands.
        {
            std::unique_lock<std::mutex> lock(counter.m_Mutex);
            ++counter.m_Waiters;
            const auto parkPredicate = [&counter, &jobs] {
                if (counter.m_Count.load(std::memory_order_acquire) == 0) {
                    return true;
                }
                jobs = counter.m_Jobs;
                return jobs && jobs->JobCount.load(std::memory_order_acquire) > 0;
            };
            if (canBlock) {
                // Keep the native wait free of timeout bookkeeping.
                counter.m_Cv.wait(lock, parkPredicate);
            } else {
                // A stalled host-thread wait stops frames and input. Report
                // progress directly even when the logger's worker is busy.
                int parkedIntervals = 0;
                while (!counter.m_Cv.wait_for(lock, std::chrono::seconds(10), parkPredicate)) {
                    ++parkedIntervals;
                    std::fprintf(stderr,
                                 "JobSystem: host-thread wait has lasted %ds with %d task(s) "
                                 "outstanding and none available for this waiter.\n",
                                 parkedIntervals * 10,
                                 static_cast<int>(counter.m_Count.load(std::memory_order_relaxed)));
                    std::fflush(stderr);
                }
            }
            --counter.m_Waiters;
        }
    }
}

void WorkStealingThreadPool::Shutdown() {
    if (m_ShutdownRequested.exchange(true)) {
        return; // Already shutdown
    }

    // Shutdown order (binding: each step depends on the previous):
    //   1. GATE: the flag above refuses Submit and JobChannel::Submit, makes
    //      EnqueueWork self-drain, sends ParallelFor down
    //      its caller-only path, and stops every blocking thread from
    //      taking further work (each finishes the job it holds).
    //   2. JOIN COMPUTE WORKERS: they stop taking new work (loop-top check),
    //      finish their in-flight task, and exit.
    //   3. RELEASE: one drain over the pool queues, every registered
    //      channel's FIFO and the blocking FIFO together (handle and graph
    //      tasks are Cancelled, then bare tasks EXECUTE: they release caller
    //      barriers, dropping them is a hang), then the graph retire
    //      (dependency-parked payloads, which no drain can see, are retired
    //      with Cancelled events) and the registry sweep (any remaining
    //      non-terminal TaskData is marked Cancelled and its waiters
    //      notified; GetOrCreateTaskData is the sole TaskData factory, so this
    //      reaches every TaskHandle::Wait whose TaskData existed at the
    //      sweep's snapshot). A blocking job may be parked on a compute handle
    //      or on another channel's handle; after this step every such handle
    //      is terminal or running on a blocking thread.
    //   4. JOIN BLOCKING THREADS: possible only now; joining them in step 2
    //      would wait forever on a blocking job parked on a handle that only
    //      step 3 releases.
    //   5. RELEASE AGAIN: a blocking job that finished in step 4 may have
    //      published pool work (and self-drained it, F13c/F13e) or released
    //      graph dependents.
    // A Submit racing the ENTIRE sequence can insert its TaskData after the
    // last sweep's snapshot; that case is closed on the submitter's side by
    // the post-publish re-check (self-drain + direct cancel), and a channel
    // reads the gate under the mutex of the FIFO it appends to, so its job
    // either precedes a drain or is handed back.

    // Wake every parked worker to stop. The flag store comes before each slot
    // lock, and a worker reads the flag under its slot lock before it blocks
    // (§7 Hazard B): it sees the flag, or it is blocked when the notify lands.
    for (size_t i = 0; i < m_ParkSlotCount; ++i) {
        ParkSlot& slot = m_ParkSlots[i].Value;
        {
            std::lock_guard<std::mutex> lock(slot.Mutex);
        }
        slot.Condition.notify_one();
    }
    if (m_BlockingThreads) {
        m_BlockingThreads->WakeForShutdown();
    }

    // Step 2: wait for all compute workers to finish.
    for (auto& worker : workers) {
        if (worker->Thread.joinable()) {
            worker->Thread.join();
        }
    }

    // Step 3.
    ReleaseQueuedAndParkedWork();

    // Steps 4 and 5.
    if (m_BlockingThreads) {
        m_BlockingThreads->Join();
        ReleaseQueuedAndParkedWork();
    }

#if GE_DEBUG_INSTRUMENTATION
    // F4: spinner tokens are RAII-guarded; with every worker joined, none may
    // remain armed.
    assert(m_Counters.NumSpinning.load(std::memory_order_seq_cst) == 0 &&
           "JobSystem: spinner token leaked past worker join");

    // F20 conservation tripwire: with the post-join drain done, every queue
    // must be empty and both counters zero.
    //
    // Racers that can still mutate the counters mid-observation: a publisher
    // between its counter increments (F5 puts them BEFORE the push) and its
    // push, or a racing self-drain between a physical dequeue and its
    // AccountDequeuedTask. Three disarms cover them:
    //  - m_CounterRacersInFlight sampled FIRST and LAST catches a racer that
    //    is INSIDE its bracket at either sample. It does NOT span the whole
    //    mutation window: a publisher's bracket ends at its push while the
    //    counters stay elevated until some drain consumes the task, and a
    //    racer can enter and exit entirely between the two samples.
    //  - m_CounterBracketEpoch (bumped on every bracket ENTRY) compared
    //    across the full read sequence catches exactly that fully-contained
    //    racer — e.g. a publish plus its complete self-drain landing between
    //    the counter snapshot and the second queue check.
    //  - Queue emptiness snapshotted on both sides of the counter reads
    //    catches a completed racy publish that nothing consumed.
    // A racy envelope left in a queue is freed by the queue destructors; its
    // waiters were woken by the registry sweep above (or by the racing
    // submitter's own F13e cancel).
    auto allQueuesEmpty = [this] {
        bool empty = m_Queues.Global.size_approx() == 0 && m_Queues.Awaited.size_approx() == 0 &&
                     m_Queues.Background.size_approx() == 0;
        for (const auto& worker : workers) {
            empty = empty && worker->LocalQueue.size_approx() == 0;
        }
        return empty;
    };
    const uint64 epochBefore = m_CounterBracketEpoch.load(std::memory_order_seq_cst);
    const uint32 racersBefore = m_CounterRacersInFlight.load(std::memory_order_seq_cst);
    const bool queuesEmptyBefore = allQueuesEmpty();
    [[maybe_unused]] const uint32 queuedSnapshot = m_Counters.QueuedTasks.load(std::memory_order_seq_cst);
    [[maybe_unused]] const uint32 awaitedSnapshot = m_Counters.AwaitedQueued.load(std::memory_order_seq_cst);
    [[maybe_unused]] const uint32 backgroundSnapshot =
        m_Counters.BackgroundQueued.load(std::memory_order_seq_cst);
    [[maybe_unused]] const size_t pendingSnapshot = m_Counters.PendingTasks.load(std::memory_order_seq_cst);
    const bool queuesEmptyAfter = allQueuesEmpty();
    const uint32 racersAfter = m_CounterRacersInFlight.load(std::memory_order_seq_cst);
    const uint64 epochAfter = m_CounterBracketEpoch.load(std::memory_order_seq_cst);
    if (racersBefore == 0 && racersAfter == 0 && epochBefore == epochAfter &&
        queuesEmptyBefore && queuesEmptyAfter) {
        assert(queuedSnapshot == 0 &&
               "JobSystem: m_Counters.QueuedTasks out of conservation (queues empty, no tasks in flight)");
        assert(awaitedSnapshot == 0 &&
               "JobSystem: m_Counters.AwaitedQueued out of conservation (queues empty, no tasks in flight)");
        assert(backgroundSnapshot == 0 &&
               "JobSystem: m_Counters.BackgroundQueued out of conservation (queues empty, no tasks in flight)");
        assert(pendingSnapshot == 0 &&
               "JobSystem: m_Counters.PendingTasks out of conservation (queues empty, no tasks in flight)");
    }
#endif

    // This thread's deferred-cleanup residue for THIS pool is meaningless now
    // (the registry dies with the pool) — purge it so sequential pools on one
    // thread never accumulate dead-pool entries. Residue held by OTHER
    // threads is inert by construction (pointer used for equality only, ids
    // never reused — see t_pendingCleanup).
    std::erase_if(t_pendingCleanup,
                  [this](const PendingCleanupEntry& entry) { return entry.Pool == this; });
}

void WorkStealingThreadPool::ReleaseQueuedAndParkedWork() {
    DrainQueuesOnThisThread();

    // Retire dependency-parked payloads with Cancelled events.
    {
        TaskGraphActions actions;
        m_DependencyGraph->CancelAllPending(actions);
        ProcessGraphActions(actions, /*sourceTaskId=*/0, String{}, nullptr);
    }

    // Registry sweep. Snapshot under the lock, fire outside it: the fire
    // takes the TaskData's own mutexes and runs user callbacks INLINE on this
    // (shutting-down) thread. Contract: task callbacks must not block on
    // other task handles; a callback that Wait()s here has no workers left to
    // complete its target.
    Vector<TaskHandle::TaskDataRef> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_TaskDataMutex);
        snapshot.reserve(m_TaskDataRegistry.size());
        for (const auto& entry : m_TaskDataRegistry) {
            snapshot.push_back(entry.second);
        }
    }
    for (auto& data : snapshot) {
        TaskStatus expected = TaskStatus::Pending;
        if (data && data->Status.compare_exchange_strong(expected, TaskStatus::Cancelled,
                                                         std::memory_order_seq_cst)) {
            // Fire on the snapshotted object, not a by-id lookup: a racing
            // self-drain can consume a stranded envelope and clean its
            // registry entry between our snapshot and this fire.
            TaskHandle::FireCancelledOn(data);
        }
    }
}

void WorkStealingThreadPool::DrainQueuesOnThisThread() {
#if GE_DEBUG_INSTRUMENTATION
    // A drain is a counter mutator that can be mid-flight (between a physical
    // dequeue and its AccountDequeuedTask) while another thread's Shutdown
    // tripwire samples — bracket it like the publishers (see
    // m_CounterRacersInFlight / m_CounterBracketEpoch; in-flight first, then
    // epoch).
    m_CounterRacersInFlight.fetch_add(1, std::memory_order_seq_cst);
    m_CounterBracketEpoch.fetch_add(1, std::memory_order_seq_cst);
    struct RacerGuard {
        std::atomic<uint32>& Counter;
        ~RacerGuard() { Counter.fetch_sub(1, std::memory_order_seq_cst); }
    } racerGuard{m_CounterRacersInFlight};
#endif

    // Two-phase per pass: dequeue EVERYTHING first, then cancel every
    // handle/graph envelope, and only then execute the bare tasks. The drain
    // runs bare tasks on the shutting-down thread, which is not a worker, so
    // such a task may Wait() on a handle task queued behind it (on a worker
    // that wait is a Debug assert, see TaskHandle::Wait). No concurrent
    // worker exists to complete that handle task, so executing bare tasks
    // inline as they are dequeued would wedge the drain forever. Cancelling
    // every handle/graph envelope first makes any such Wait() observe
    // Cancelled and return.
    //
    // Counter bookkeeping matches a worker dequeue exactly (F20):
    // m_Counters.QueuedTasks releases at the physical dequeue via AccountDequeuedTask,
    // m_Counters.PendingTasks after each task is disposed of (executed or
    // cancelled+freed) — the conservation tripwire must not be able to tell
    // drained tasks from executed ones, and each envelope is consumed once.
    //
    // Re-entrancy: a drained bare task that calls EnqueueWork publishes and
    // then self-drains recursively via the F13c re-check (this thread, fresh
    // pass) — the nested drain applies the same two phases to whatever it can
    // reach, and anything it misses is picked up by this loop's next pass.
    //
    // Channel jobs: the same pass empties every registered channel's FIFO and
    // the blocking threads' FIFO, each under its own mutex, beside the pool
    // queues, because a job of one FIFO may wait on a handle queued in
    // another; emptying them one after another would let phase 3 run a job
    // whose handle dependency is still queued. Those envelopes never touched
    // the pool's counters, so they skip AccountDequeuedTask and the
    // m_Counters.PendingTasks decrement (DrainedTask::PoolCounted). A blocking-FIFO
    // envelope holds its channel slot; destroying it releases the slot, and
    // with the gate set the release promotes nothing.

    // Loop until a full pass over every queue finds nothing AND the size
    // signals agree empty: a racing EnqueueWork (the F13c window) or Submit
    // (F13e) can publish between passes — that caller self-drains too, but
    // this side must not exit while it can still observe work.
    for (;;) {
        // Phase 1: dequeue everything reachable into a local list. Every
        // dequeue in this drain is TOKENLESS: all of a Worker's
        // ConsumerTokens (local queue AND the global lanes) are
        // thread-affine to that worker's thread and must never be used from
        // this one (same rule as the steal path). The tokenless heuristic's
        // #360 unfairness is irrelevant here — the drain loops until every
        // queue is empty.
        Vector<DrainedTask> drained;
        if (m_InlineMode) {
            auto& deferred = GetInlineTrampoline().Deferred;
            for (auto it = deferred.begin(); it != deferred.end();) {
                if (it->Pool == this) {
                    drained.push_back({std::move(it->Task), /*PoolCounted=*/false,
                                       JobContext{it->Channel, it->Class}});
                    it = deferred.erase(it);
                } else {
                    ++it;
                }
            }
        }
        TakeChannelJobs(drained);
        for (;;) {
            bool found = false;
            UniquePtr<TaskBase> task;
            // The Awaited and Background lanes drain ungated, with the
            // disposal and lane recording the Background lane's comment states.
            while (m_Queues.Awaited.try_dequeue(task)) {
                m_Counters.AwaitedQueued.fetch_sub(1, std::memory_order_relaxed);
                m_Census.AwaitedPops.fetch_add(1, std::memory_order_relaxed);
                AccountDequeuedTask(0);
                drained.push_back({std::move(task), /*PoolCounted=*/true, ComputeContext(JobPriority::Awaited)});
                found = true;
            }
            while (m_Queues.Global.try_dequeue(task)) {
                // Census pops count PHYSICAL dequeues from a lane, whichever
                // path consumes them — the push/pop conservation identity
                // must hold across a drain too (a participating Wait can
                // leave its no-op visit for the shutdown drain to consume).
                m_Census.GlobalPops.fetch_add(1, std::memory_order_relaxed);
                AccountDequeuedTask(0);
                drained.push_back({std::move(task), /*PoolCounted=*/true, ComputeContext(JobPriority::Normal)});
                found = true;
            }
            for (auto& worker : workers) {
                while (worker->LocalQueue.try_dequeue(task)) {
                    m_Census.LocalPops.fetch_add(1, std::memory_order_relaxed);
                    AccountDequeuedTask(0);
                    drained.push_back({std::move(task), /*PoolCounted=*/true, ComputeContext(JobPriority::Normal)});
                    found = true;
                }
            }
            // The background lane is drained ungated (the drain must reach
            // everything; the occupancy gate is a hot-scan optimization).
            // Disposal is priority-blind: phase 2/3 below dispatch on
            // envelope KIND — bare background tasks EXECUTE, background
            // handle tasks are Cancelled (see the class comment's shutdown
            // paragraph for the decision). The lane is recorded beside the
            // envelope, because phase 3 executes one mixed list and a bare
            // Background task still runs as Background there.
            while (m_Queues.Background.try_dequeue(task)) {
                m_Counters.BackgroundQueued.fetch_sub(1, std::memory_order_relaxed);
                m_Census.BackgroundPops.fetch_add(1, std::memory_order_relaxed);
                AccountDequeuedTask(0);
                drained.push_back({std::move(task), /*PoolCounted=*/true, ComputeContext(JobPriority::Background)});
                found = true;
            }

            if (!found) {
                bool empty = m_Queues.Global.size_approx() == 0 && m_Queues.Awaited.size_approx() == 0 &&
                             m_Queues.Background.size_approx() == 0;
                for (const auto& worker : workers) {
                    empty = empty && worker->LocalQueue.size_approx() == 0;
                }
                if (empty) {
                    break;
                }
            }
        }
        if (drained.empty()) {
            break;
        }

        // Phase 2: cancel handle/graph tasks, never execute them. The
        // arbitration CAS keeps this single-fire against a concurrent
        // CancelTask; a task the drain dequeues can no longer be claimed by
        // anything else, so losing the CAS just means the events already
        // fired.
        for (DrainedTask& entry : drained) {
            const bool bare =
                !entry.Task->NeedsTaskData() && !entry.Task->RequiresDependencyManagement();
            if (bare) {
                continue;
            }
            CancelUnrunEnvelope(std::move(entry.Task));
            if (entry.PoolCounted)
                m_Counters.PendingTasks.fetch_sub(1, std::memory_order_relaxed);
        }

        // Phase 3: execute the bare tasks. They are caller-barrier releasers
        // (ParallelFor helpers reference a blocked caller's
        // stack): dropping one hangs that caller forever (B7), and any
        // handle task they Wait() on is already terminal from phase 2.
        for (DrainedTask& entry : drained) {
            if (entry.Task) {
                ExecuteTaskOptimized(std::move(entry.Task), entry.Context);
                if (entry.PoolCounted)
                    m_Counters.PendingTasks.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }
}

void WorkStealingThreadPool::TakeChannelJobs(Vector<DrainedTask>& drained) {
    // Strong references under the registry mutex, the FIFOs outside it: each
    // channel's mutex is a leaf, never taken under another lock.
    Vector<SharedPtr<Detail::JobChannelState>> channels;
    {
        std::lock_guard<std::mutex> lock(m_ChannelsMutex);
        channels.reserve(m_Channels.size());
        for (const WeakPtr<Detail::JobChannelState>& channel : m_Channels) {
            if (SharedPtr<Detail::JobChannelState> live = channel.lock()) {
                channels.push_back(std::move(live));
            }
        }
    }
    Vector<Detail::ChannelJob> jobs;
    for (const SharedPtr<Detail::JobChannelState>& channel : channels) {
        channel->TakeQueued(jobs);
    }
    if (m_BlockingThreads) {
        m_BlockingThreads->TakeQueued(jobs);
    }
    for (Detail::ChannelJob& job : jobs) {
        drained.push_back({std::move(job.Envelope), /*PoolCounted=*/false, ChannelContext(job.Channel)});
    }
}

void WorkStealingThreadPool::CancelUnrunEnvelope(UniquePtr<TaskBase> task) {
    const TaskId taskId = task->GetTaskId();
    // Fast-path envelopes carry their TaskData inline; use it directly (one
    // fewer registry round-trip, and still correct if the registry entry is
    // already gone).
    TaskHandle::TaskDataRef data =
        task->HasInlineTaskData() ? static_cast<Detail::HandleTask*>(task.get())->GetTaskData()
                                  : TryGetTaskData(taskId);
    if (data) {
        CancelPendingArbitrated(taskId, data);
    }
    task.reset();            // envelope + closure freed here (F14)
    CleanupTaskData(taskId); // ...so the registry entry releases here
}

void WorkStealingThreadPool::DisposeEnvelopeAfterGate(UniquePtr<TaskBase> task, JobContext context) {
    if (task->NeedsTaskData() || task->RequiresDependencyManagement()) {
        CancelUnrunEnvelope(std::move(task));
        return;
    }
    ExecuteTaskOptimized(std::move(task), context);
}

void WorkStealingThreadPool::DispatchToBlockingThreads(UniquePtr<TaskBase> task, const char* channel) {
    if (m_InlineMode) {
        ExecuteInlineBounded(std::move(task), ChannelContext(channel));
        return;
    }
    if (UniquePtr<TaskBase> refused =
            m_BlockingThreads->Push({std::move(task), channel}, Detail::BlockingThreads::Spawn::IfNeeded)) {
        DisposeEnvelopeAfterGate(std::move(refused), ChannelContext(channel));
    }
}

void WorkStealingThreadPool::HandOnToBlockingThreads(UniquePtr<TaskBase> task, const char* channel) {
    // An inline pool never queues a channel job, so it never hands one on.
    assert(!m_InlineMode && "JobChannel: an inline pool queued a channel job");
    if (UniquePtr<TaskBase> refused =
            m_BlockingThreads->Push({std::move(task), channel}, Detail::BlockingThreads::Spawn::Never)) {
        DisposeEnvelopeAfterGate(std::move(refused), ChannelContext(channel));
    }
}

void WorkStealingThreadPool::RegisterChannel(const SharedPtr<Detail::JobChannelState>& channel,
                                             uint32 maxRunning) {
    {
        std::lock_guard<std::mutex> lock(m_ChannelsMutex);
        m_Channels.push_back(channel);
    }
    if (m_BlockingThreads) {
        m_BlockingThreads->AddDemand(static_cast<int64>(maxRunning));
    }
}

void WorkStealingThreadPool::UnregisterChannel(const Detail::JobChannelState* channel, uint32 maxRunning) {
    {
        std::lock_guard<std::mutex> lock(m_ChannelsMutex);
        std::erase_if(m_Channels, [channel](const WeakPtr<Detail::JobChannelState>& registered) {
            const SharedPtr<Detail::JobChannelState> live = registered.lock();
            return live == nullptr || live.get() == channel;
        });
    }
    if (m_BlockingThreads) {
        m_BlockingThreads->AddDemand(-static_cast<int64>(maxRunning));
    }
}

void WorkStealingThreadPool::ReleaseFinishedTasks() {
    // A held entry stays: a graph task's completer holds it after its terminal store and still finds it by id.
    // Under m_TaskDataMutex OwnerCount() == 1 is exact, because only the registry can mint a reference then.
    // The erase only drops a reference; the owner count's decrement orders the reset.
    Vector<TaskHandle::TaskDataRef> released;
    {
        std::lock_guard<std::mutex> lock(m_TaskDataMutex);
        for (auto it = m_TaskDataRegistry.begin(); it != m_TaskDataRegistry.end();) {
            const TaskStatus status = it->second->Status.load(std::memory_order_acquire);
            if (status != TaskStatus::Completed && status != TaskStatus::Failed) {
                ++it;
                continue;
            }
            if (it->second.OwnerCount() != 1) {
                ++it;
                continue;
            }
            released.push_back(std::move(it->second));
            it = m_TaskDataRegistry.erase(it);
        }
    }
    // The references drop after the unlock: a last reference runs the result's and callbacks' destructors.
    Logger::Log::Debug("JobSystem: released {} finished task(s) from the registry", released.size());
}

void WorkStealingThreadPool::FlushPendingCleanupOnThisThread() {
    const bool ownEntries = std::any_of(t_pendingCleanup.begin(), t_pendingCleanup.end(),
                                        [this](const PendingCleanupEntry& entry) { return entry.Pool == this; });
    if (!ownEntries) {
        return;
    }
    TaskDataReleaseBatch released;
    std::lock_guard<std::mutex> lock(m_TaskDataMutex);
    DrainPendingCleanupLocked(released);
}

void WorkStealingThreadPool::DependencyAwareWorkerLoop(size_t workerId) {
    // Set thread-local worker ID and pointer for fast task submission
    s_CurrentWorkerId = workerId;
    s_CurrentWorker = workers[workerId].get();
    s_CurrentPool = this;
    s_JobState = &m_ThreadRecords[workerId].Value.State;

    char threadName[32];
    snprintf(threadName, sizeof(threadName), "Job Worker #%zu", workerId);
    GameEngine::Platform::SetCurrentThreadName(threadName);

    // After a park that ends with nothing queued, the worker parks again without
    // a spin window, so an idle pool does not spin at each backstop expiry. A
    // worker that does not spin is not in m_Counters.NumSpinning: a waker claims its idle bit.
    bool spinAllowed = true;

    while (true) {
        // F13: exit as soon as shutdown is requested instead of draining the
        // queues dry. Leftover queued work belongs to Shutdown()'s post-join
        // drain — bare tasks execute there, handle/graph tasks are Cancelled.
        // Draining here would execute handle tasks the shutdown contract
        // promises to cancel.
        if (m_ShutdownRequested.load()) {
            break;
        }

        UniquePtr<TaskBase> task;

        // Take the Awaited lane first (a thread waits for it), then the local queue, the global
        // queue and the steal ring (all three Normal), then the Background lane. The lane a task
        // came from is its class. An empty class lane costs one relaxed load.
        JobPriority lane = JobPriority::Awaited;
        bool foundTask = TryGetAwaitedTask(workerId, task);
        if (!foundTask) {
            lane = JobPriority::Normal;
            foundTask = TryGetLocalTask(workerId, task) || TryGetGlobalTask(workerId, task) ||
                        TryStealTask(workerId, task);
        }
        if (!foundTask && TryGetBackgroundTask(workerId, task)) {
            foundTask = true;
            lane = JobPriority::Background;
        }

        if (!foundTask && spinAllowed) {
            foundTask = TrySpinPoll(workerId, task, lane);
        }

        if (foundTask) {
            // Common bookkeeping point for EVERY consumed task (normal dequeue
            // and spin-poll finds): m_Counters.QueuedTasks release + wake propagation
            // happen here, and the m_Counters.PendingTasks decrement stays tied to
            // post-execution (F20 — ECS backpressure semantics).
            AccountDequeuedTask(workerId + 1);
            ExecuteTaskOptimized(std::move(task), ComputeContext(lane));
            m_Counters.PendingTasks.fetch_sub(1, std::memory_order_relaxed);
            spinAllowed = true;
            continue;
        }

        // (S0) Census: a task is queued that this worker's find cycle did not
        // reach, so the worker loops again without parking. It sets no idle bit.
        if (m_Counters.QueuedTasks.load(std::memory_order_relaxed) > 0) {
            m_Census.EmptyLoopsWithBacklog.fetch_add(1, std::memory_order_relaxed);
            spinAllowed = true;
            continue;
        }

        const bool expired = ParkWorker(workerId);
        spinAllowed = m_ShutdownRequested.load() || m_Counters.QueuedTasks.load(std::memory_order_seq_cst) > 0;
        if (expired && !spinAllowed) {
            m_Census.BackstopTimeouts.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void WorkStealingThreadPool::DrainInlineTasksOnThisThread() {
    auto& trampoline = GetInlineTrampoline();
    for (;;) {
        const auto pending = std::find_if(trampoline.Deferred.begin(), trampoline.Deferred.end(),
            [this](const DeferredInlineTask& task) { return task.Pool == this; });
        if (pending == trampoline.Deferred.end())
            return;
        auto task = std::move(pending->Task);
        const JobContext context{pending->Channel, pending->Class};
        trampoline.Deferred.erase(pending);
        // A synchronous join must make progress even at the recursion bound.
        // Children published by this task stay in the same bounded queue.
        ++trampoline.Depth;
        ExecuteTaskOptimized(std::move(task), context);
        --trampoline.Depth;
    }
}

void WorkStealingThreadPool::ExecuteInlineBounded(UniquePtr<TaskBase> task, JobContext context) {
    // Per-thread, not per-pool: the resource being bounded is this thread's
    // stack, and nested inline pools share it. Entries carry their owning
    // pool so the drain executes each task against the pool it was published
    // to.
    auto& trampoline = GetInlineTrampoline();

    if (trampoline.Depth >= kMaxInlineExecutionDepth) {
        trampoline.Deferred.push_back({this, std::move(task), context.Channel, context.Class});
        return;
    }

    ++trampoline.Depth;
    ExecuteTaskOptimized(std::move(task), context);
    if (trampoline.Depth == 1) {
        // Outermost frame: drain iteratively. A drained task's own enqueues
        // re-enter above at depth 1, recurse up to the bound, and overflow
        // back into this queue — the loop terminates when the tree is done.
        while (!trampoline.Deferred.empty()) {
            DeferredInlineTask deferred = std::move(trampoline.Deferred.front());
            trampoline.Deferred.pop_front();
            deferred.Pool->ExecuteTaskOptimized(std::move(deferred.Task),
                                                JobContext{deferred.Channel, deferred.Class});
        }
    }
    --trampoline.Depth;
}

void WorkStealingThreadPool::EnqueueTask(UniquePtr<TaskBase> task) {
    if (m_InlineMode) {
        // Inline mode: execute at publish, bypassing queues, counters, and
        // wakes entirely. Graph-published dependents re-enter here;
        // ExecuteInlineBounded caps the recursion depth and carries the
        // cancel arbitration and completion paths unchanged. Graph tasks
        // are Normal.
        ExecuteInlineBounded(std::move(task), ComputeContext(JobPriority::Normal));
        return;
    }
    // Producer publish sequence (§7): counters BEFORE the push (F5 — a fast
    // consumer decrements at dequeue-success; push-then-increment would let it
    // transiently underflow the unsigned counter), then the wake decision.
#if GE_DEBUG_INSTRUMENTATION
    // Bracket enter: in-flight FIRST, then epoch (see m_CounterBracketEpoch —
    // this order is what makes the tripwire's two disarms gapless).
    m_CounterRacersInFlight.fetch_add(1, std::memory_order_seq_cst);
    m_CounterBracketEpoch.fetch_add(1, std::memory_order_seq_cst);
#endif
    m_Counters.PendingTasks.fetch_add(1, std::memory_order_relaxed);
    m_Counters.QueuedTasks.fetch_add(1, std::memory_order_seq_cst); // (P1)
    PushTask(std::move(task));
#if GE_DEBUG_INSTRUMENTATION
    m_CounterRacersInFlight.fetch_sub(1, std::memory_order_seq_cst);
#endif
    NotifyAfterPublish(); // (P2..P4)
}

void WorkStealingThreadPool::PushTask(UniquePtr<TaskBase> task) {
    // Use the local worker queue only when the calling thread is a worker of
    // THIS pool; a worker of another pool must take the external path or the
    // task would land in a queue this pool never drains.
    if (s_CurrentWorker != nullptr && s_CurrentPool == this) {
        if (!s_CurrentWorker->LocalQueue.enqueue(s_CurrentWorker->ProducerToken,
                                                 std::move(task))) {
            // Allocating enqueue fails only on genuine OOM; a dropped graph
            // task strands its dependents forever — fail loud (slice-4 OOM
            // discipline).
            std::fprintf(stderr, "WorkStealingThreadPool: local queue allocation failed under "
                                 "OOM; aborting rather than dropping queued work.\n");
            std::fflush(stderr);
            std::abort();
        }
        m_Census.LocalPushes.fetch_add(1, std::memory_order_relaxed);
        CountPublishedOnThisThread(1);
    } else {
        // External submissions may come from many threads (UI, asset batcher,
        // thumbnail preload, file watchers). ProducerToken is thread-affine, so
        // use the tokenless MPMC enqueue path instead of sharing one token.
        // Graph tasks are always Normal.
        PushGlobalQueue(std::move(task), JobPriority::Normal);
    }
}

void WorkStealingThreadPool::NotifyAfterPublish() {
    // (P2, F2-amended): skip the kernel notify ONLY when the armed spinners
    // cover the whole queue depth, evaluated AFTER our own increment — one
    // spinner can only cover one task, so N producers racing one spinner must
    // still send N-1 notifies. Both loads are seq_cst: the failure mode is
    // StoreLoad reordering of our (P1) increment below this load against a
    // retiring spinner's (Y)/(Z) pair — acquire/release does not order a store
    // before a later load to a different variable (§7 Hazard A).
    const uint32 spinning = m_Counters.NumSpinning.load(std::memory_order_seq_cst); // (P2)
    const uint32 queued = m_Counters.QueuedTasks.load(std::memory_order_seq_cst);
    if (spinning >= queued) {
        return;
    }

    // (P3', P4'): claim one idle worker and signal it. An empty mask means
    // every worker is busy, spinning or between polls, and the next dequeue
    // takes the task.
    WakeIdleWorker(0);
}

void WorkStealingThreadPool::PublishAndWakeBatch(size_t publishedCount) {
    // (P2 batch form, F2-amended): an armed spinner covers AT MOST ONE queued
    // task, so only the queued remainder above the spinner count needs kernel
    // wakes — subtracting the counts is the batch generalization of the
    // single-task "spinning >= queued" skip. Both loads are seq_cst for the
    // same §7 Hazard-A StoreLoad reasoning as NotifyAfterPublish: our (P1)
    // increments must not sink below these loads against a retiring
    // spinner's (Y)/(Z) pair — a spinner we counted either re-checks after
    // retire and sees our tasks, or retired first and is not counted here.
    const uint32 spinning = m_Counters.NumSpinning.load(std::memory_order_seq_cst); // (P2)
    const uint32 queued = m_Counters.QueuedTasks.load(std::memory_order_seq_cst);
    const uint32 uncovered = queued > spinning ? queued - spinning : 0;
    const uint32 need =
        static_cast<uint32>(std::min<size_t>(publishedCount, uncovered));
    if (need == 0) {
        return; // armed spinners cover the whole backlog (or it drained already)
    }

    // (P3', P4' batch form): the caller claims and signals one idle worker.
    // The F3 chain (AccountDequeuedTask) wakes one more on every dequeue while
    // a backlog and idle workers remain, so a wide batch still reaches full width.
    if (WakeIdleWorker(0)) {
        m_Counters.BatchNotifiesForTests.fetch_add(1, std::memory_order_relaxed);
    }
}

void WorkStealingThreadPool::AccountDequeuedTask(size_t wakeScanStart) {
    m_Counters.QueuedTasks.fetch_sub(1, std::memory_order_seq_cst);

    // Wake propagation (F3): while a backlog remains, pass one wake along to
    // an idle worker. The chain stops when the mask or the queue is empty.
    if (m_Counters.QueuedTasks.load(std::memory_order_seq_cst) > 0) {
        WakeIdleWorker(wakeScanStart);
    }
}

bool WorkStealingThreadPool::WakeIdleWorker(size_t firstWorker) {
    // (P3') The count load comes after the waker's seq_cst (P1). A worker raises
    // the count before it sets its bit and reads the queued count (§7 Hazard A),
    // so a zero means that no worker can park without seeing the task.
    if (m_Counters.NumParking.load(std::memory_order_seq_cst) == 0) {
        return false;
    }
    // Scan from the start bit to the end of its word, then the other words,
    // then the start word's bits below the start bit.
    const size_t start = firstWorker % m_ParkSlotCount;
    const uint64 startBit = uint64{1} << (start % kIdleMaskBits);
    for (size_t step = 0; step <= m_IdleMaskWords; ++step) {
        const size_t wordIndex = (start / kIdleMaskBits + step) % m_IdleMaskWords;
        uint64 range = ~uint64{0};
        if (step == 0) {
            range = ~(startBit - 1);
        } else if (step == m_IdleMaskWords) {
            range = startBit - 1;
        }
        // (P3') The mask load comes after the waker's seq_cst (P1), and a worker
        // sets its bit before it reads the queued count (§7 Hazard A): the
        // waker sees the bit, or the worker sees the task and does not block.
        std::atomic<uint64>& word = m_IdleMask[wordIndex].Value;
        uint64 idle = word.load(std::memory_order_seq_cst) & range;
        while (idle != 0) {
            // The claim: the bit is ours only if this fetch_and cleared it.
            const int bit = std::countr_zero(idle);
            const uint64 chosen = uint64{1} << bit;
            const uint64 before = word.fetch_and(~chosen, std::memory_order_seq_cst);
            if ((before & chosen) != 0) {
                SignalClaimedWorker(wordIndex * kIdleMaskBits + static_cast<size_t>(bit));
                return true;
            }
            idle = before & range & ~chosen;
        }
    }
    return false;
}

void WorkStealingThreadPool::SignalClaimedWorker(size_t workerId) {
#if GE_DEBUG_INSTRUMENTATION
    m_WakeClaimsForTests.fetch_add(1, std::memory_order_relaxed);
    CallWakeHook(WakePoint::AfterClaim, workerId);
#endif
    // (P4') The flag changes only under the slot lock (§7 Hazard B): the
    // worker reads it before it blocks, or it is blocked when the notify lands.
    ParkSlot& slot = m_ParkSlots[workerId].Value;
    {
        std::lock_guard<std::mutex> lock(slot.Mutex);
#if GE_DEBUG_INSTRUMENTATION
        if (slot.Signaled) {
            m_WakeMergedForTests.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        slot.Signaled = true;
    }
    slot.Condition.notify_one();
}

bool WorkStealingThreadPool::ParkWorker(size_t workerId) {
    const uint64 bit = uint64{1} << (workerId % kIdleMaskBits);
    std::atomic<uint64>& word = m_IdleMask[workerId / kIdleMaskBits].Value;
#if GE_DEBUG_INSTRUMENTATION
    CallWakeHook(WakePoint::BeforeIdleBit, workerId);
#endif
    // (S1) before (S2), all seq_cst, against a waker's (P1) before its count
    // and mask loads (§7 Hazard A): the waker sees this count and this bit, or
    // (S2) sees its task.
    m_Counters.NumParking.fetch_add(1, std::memory_order_seq_cst);
    word.fetch_or(bit, std::memory_order_seq_cst);
#if GE_DEBUG_INSTRUMENTATION
    CallWakeHook(WakePoint::AfterIdleBit, workerId);
#endif
    bool expired = false;
    if (!m_ShutdownRequested.load() && m_Counters.QueuedTasks.load(std::memory_order_seq_cst) == 0) { // (S2)
#if GE_DEBUG_INSTRUMENTATION
        CallWakeHook(WakePoint::BeforeSlotLock, workerId);
#endif
        ParkSlot& slot = m_ParkSlots[workerId].Value;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::microseconds(m_SleepBackstopUs.load(std::memory_order_relaxed));
        std::unique_lock<std::mutex> lock(slot.Mutex);
#if GE_DEBUG_INSTRUMENTATION
        if (slot.Signaled) {
            m_WakeStaleForTests.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        // (S3) Both flags are read under the slot lock, and wait_until releases
        // it atomically (§7 Hazard B).
        while (!slot.Signaled && !m_ShutdownRequested.load()) {
#if GE_DEBUG_INSTRUMENTATION
            CallWakeHook(WakePoint::BeforeBlock, workerId);
#endif
            if (slot.Condition.wait_until(lock, deadline) == std::cv_status::timeout) {
                expired = !slot.Signaled;
                break;
            }
        }
#if GE_DEBUG_INSTRUMENTATION
        if (slot.Signaled) {
            m_WakeConsumedForTests.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        slot.Signaled = false;
    }
#if GE_DEBUG_INSTRUMENTATION
    CallWakeHook(WakePoint::BeforeIdleClear, workerId);
#endif
    // (S5) A bit already clear here means a waker claimed this worker. Its
    // flag, if it lands after (S3), makes the next park return at once.
    word.fetch_and(~bit, std::memory_order_seq_cst);
    m_Counters.NumParking.fetch_sub(1, std::memory_order_relaxed);
    return expired;
}

bool WorkStealingThreadPool::TrySpinPoll(size_t workerId, UniquePtr<TaskBase>& task, JobPriority& lane) {
    const uint32 spinWindowUs = m_SpinWindowUs.load(std::memory_order_relaxed);
    if (spinWindowUs == 0) {
        return false; // Spin path disabled: pre-change sleep behavior.
    }

    // Bounded token acquisition: at most m_MaxSpinners workers poll at once.
    {
        const uint32 maxSpinners = m_MaxSpinners.load(std::memory_order_relaxed);
        uint32 current = m_Counters.NumSpinning.load(std::memory_order_seq_cst);
        do {
            if (current >= maxSpinners) {
                return false;
            }
        } while (!m_Counters.NumSpinning.compare_exchange_weak(current, current + 1,
                                                      std::memory_order_seq_cst));
    }

    // F4: the token is RAII-guarded — released exactly once on every exit path
    // (deadline expiry, task found, shutdown, exception). Retire() is §7 op (Y).
    struct SpinToken {
        std::atomic<uint32>& Counter;
        bool Armed = true;
        void Retire() {
            if (Armed) {
                Counter.fetch_sub(1, std::memory_order_seq_cst); // (Y)
                Armed = false;
            }
        }
        ~SpinToken() { Retire(); }
    } token{m_Counters.NumSpinning};

    bool found = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::microseconds(spinWindowUs);
    uint32 pauseCount = 1;

    for (uint32 iteration = 1;; ++iteration) {
        if (m_ShutdownRequested.load()) {
            break;
        }

        // Our own local queue is provably empty here: only this thread feeds
        // it, and it is not executing tasks while spinning. Poll the global
        // queue every iteration and the steal ring every kSpinStealPeriod-th;
        // the background lane keeps its scan position (LAST, after the steal
        // attempt) on the same cadence — polling it every iteration would let
        // a background task beat a steal-visible Normal task 7 laps out of 8.
        // The awaited lane is polled first, every iteration.
        if (TryGetAwaitedTask(workerId, task)) {
            lane = JobPriority::Awaited;
            found = true;
            break;
        }
        if (TryGetGlobalTask(workerId, task) ||
            ((iteration % kSpinStealPeriod) == 0 && TryStealTask(workerId, task))) {
            lane = JobPriority::Normal;
            found = true;
            break;
        }
        if ((iteration % kSpinStealPeriod) == 0 && TryGetBackgroundTask(workerId, task)) {
            lane = JobPriority::Background;
            found = true;
            break;
        }

        // Wall-clock deadline: pause-ramp iterations have variable cost, so
        // the window is bounded by steady_clock, not iteration count.
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }

        for (uint32 p = 0; p < pauseCount; ++p) {
            CpuRelax();
        }
        if (pauseCount < kSpinPauseMax) {
            pauseCount *= 2;
        }
        if ((iteration % kSpinYieldPeriod) == 0) {
            std::this_thread::yield();
        }
    }

    // (Y) Retire BEFORE executing anything found. The caller decrements
    // m_Counters.QueuedTasks at the common bookkeeping point only after this returns,
    // i.e. after (Y): a producer that still observes our armed token also
    // still observes the queued count covering the task we physically hold,
    // so its F2 comparison cannot double-count us against a second task.
    token.Retire();

    if (!found && m_Counters.QueuedTasks.load(std::memory_order_seq_cst) > 0) { // (Z)
        // A producer may have skipped its notify on our account (P2) between
        // our last poll and (Y). Dequeue-or-notify (F5): take the task if we
        // can still see it, otherwise wake an idle worker — never drop.
        // m_Counters.QueuedTasks counts every class, so the recheck's dequeue attempt
        // covers the awaited lane (first) and the background lane (last), as everywhere.
        found = TryGetAwaitedTask(workerId, task);
        lane = JobPriority::Awaited;
        if (!found) {
            found = TryGetGlobalTask(workerId, task) || TryStealTask(workerId, task);
            lane = JobPriority::Normal;
        }
        if (!found && TryGetBackgroundTask(workerId, task)) {
            found = true;
            lane = JobPriority::Background;
        }
        if (!found) {
            WakeIdleWorker(workerId + 1);
        }
    }

    return found;
}

bool WorkStealingThreadPool::TryGetAwaitedTask(size_t workerId, UniquePtr<TaskBase>& task) {
    // The occupancy gate and the decrement-after-dequeue of TryGetBackgroundTask.
    if (m_Counters.AwaitedQueued.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    if (m_Queues.Awaited.try_dequeue(workers[workerId]->AwaitedConsumerToken, task)) {
        m_Counters.AwaitedQueued.fetch_sub(1, std::memory_order_relaxed);
        m_Census.AwaitedPops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool WorkStealingThreadPool::TryGetLocalTask(size_t workerId, UniquePtr<TaskBase>& task) {
    auto& worker = *workers[workerId];
    if (worker.LocalQueue.try_dequeue(worker.LocalConsumerToken, task)) {
        m_Census.LocalPops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool WorkStealingThreadPool::TryGetGlobalTask(size_t workerId, UniquePtr<TaskBase>& task) {
    // Token dequeue (#360): fair rotation across producer sub-queues instead
    // of the tokenless biggest-of-first-3-non-empty heuristic, which starves
    // low-rate producers for the whole lifetime of a sustained flood (full
    // mechanism at Worker::GlobalConsumerToken). Failure semantics match the
    // tokenless path — false means every sub-queue appeared empty at check
    // time — so callers' m_Counters.QueuedTasks relap tolerance is unchanged.
    if (m_Queues.Global.try_dequeue(workers[workerId]->GlobalConsumerToken, task)) {
        m_Census.GlobalPops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool WorkStealingThreadPool::TryGetBackgroundTask(size_t workerId, UniquePtr<TaskBase>& task) {
    // The occupancy gate: an empty background lane costs exactly this one
    // relaxed load per scan — the queue itself is never touched (the slice-7
    // lesson: an ungated second global queue taxed every consumer scan).
    // A stale zero here is benign: the producer's m_Counters.QueuedTasks increment
    // keeps the sleep predicate true, so the worker relaps and re-reads.
    if (m_Counters.BackgroundQueued.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    // Token dequeue for the same #360 fairness reason as TryGetGlobalTask:
    // the hole is producer-count-driven, so a background flood (streaming is
    // this lane's steady-state consumer) could starve another background
    // producer identically.
    if (m_Queues.Background.try_dequeue(workers[workerId]->BackgroundConsumerToken, task)) {
        // Decrement AFTER dequeue-success only — paired with the producers'
        // increment-before-push, the counter can never underflow and always
        // over-counts physical occupancy (the safe direction: a transient
        // false-nonzero costs one failed try_dequeue, never a missed task).
        m_Counters.BackgroundQueued.fetch_sub(1, std::memory_order_relaxed);
        m_Census.BackgroundPops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool WorkStealingThreadPool::TryStealTask(size_t thiefId, UniquePtr<TaskBase>& task) {
    // Ring-based work stealing for better cache locality
    // Try adjacent workers first, then expand outward
    const size_t numWorkers = workers.size();

    if (numWorkers <= 1) {
        return false; // No other workers to steal from
    }

    // Try workers in ring order: thiefId+1, thiefId+2, etc.
    // This improves cache locality by preferring nearby cores
    for (size_t offset = 1; offset < numWorkers; ++offset) {
        size_t victimId = (thiefId + offset) % numWorkers;

        auto& victim = *workers[victimId];

        // IMPORTANT:
        // `moodycamel::ConsumerToken` is thread-affine. The victim's consumer token must only be
        // used by the victim thread; using it from the thief thread is undefined behavior and
        // can crash intermittently (especially under contention).
        //
        // For work stealing we either need per-thief tokens per victim queue, or simply use the
        // tokenless dequeue API (slightly slower but correct).
        if (victim.LocalQueue.try_dequeue(task)) {
            m_Census.StealPops.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Early exit optimization: if we've tried half the workers
        // and found nothing, the remaining workers are likely empty too
        if (offset >= numWorkers / 2) {
            break;
        }
    }

    m_Census.StealMisses.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void WorkStealingThreadPool::ExecuteTaskOptimized(UniquePtr<TaskBase> task, JobContext context) {
#if GE_DEBUG_INSTRUMENTATION
    // A queued body can outlive its publisher's frame, so it, the callbacks and the graph release here run with the
    // frame-joined bit clear, on inline pools too. A callback fired elsewhere (a cancel, OnComplete on a finished task)
    // keeps its thread's bit; Run closures and ParallelFor helper units set their publisher's bit again inside.
    const GameEngine::Memory::Detail::FrameJoinedScope notFrameJoined(false);
#endif
    TaskId taskId = task->GetTaskId();

    // Capability bits (slice 6: construction-time flags on TaskBase — these
    // were two virtual calls per execution).
    const bool requiresDependencyManagement = task->RequiresDependencyManagement();
    const bool needsTaskData = task->NeedsTaskData();

    // Ultra-lightweight path: no TaskHandle, no TaskData, no dependency graph.
    // Used by EnqueueWork / EnqueueWorkBatch (and the fork-join algorithms
    // built on them) for zero-bookkeeping dispatch.
    if (!needsTaskData && !requiresDependencyManagement) {
        try {
            JobBodyScope body(context);
            task->Execute();
        } catch (const std::exception& e) {
            Logger::Log::Error("Bare task execution failed: {}", e.what());
        } catch (...) {
            Logger::Log::Error("Bare task {} threw a non-standard exception", taskId);
        }
        return;
    }

    // Releases the registry's strong reference to this task's TaskData on
    // every exit path below. The TaskHandle held by the submitter keeps its
    // own shared_ptr to the same TaskData (via GetTaskData()), so the
    // underlying object stays alive for any in-flight Wait()/GetResult()
    // callers — only the registry entry goes away. Without this, every
    // Submit() leaks a TaskData entry forever (TaskIds are monotonic; no
    // entry is ever overwritten), which manifested as ~3 MB/sec CPU heap
    // growth during pure idle (TaskData is the top growing type in VS
    // Diagnostic Tools).
    struct TaskDataReleaser
    {
        WorkStealingThreadPool* pool;
        TaskId                  id;
        bool                    needsCleanup;
        ~TaskDataReleaser()
        {
            if (needsCleanup)
                pool->CleanupTaskData(id);
        }
    } releaser{this, taskId, needsTaskData};

    // The Submit(F&&) fast path carries its TaskData in the envelope
    // (HandleTask); adopt it instead of re-entering the registry, which would
    // take m_TaskDataMutex a second time. Graph tasks keep the registry
    // lookup; their completion is bridged by id from
    // CompleteGraphTask/ProcessGraphActions anyway.
    TaskHandle handle =
        task->HasInlineTaskData()
            ? TaskHandle(taskId, this, static_cast<Detail::HandleTask*>(task.get())->GetTaskData())
            : TaskHandle(taskId, this);

    // F12 cancel/execute arbitration, BEFORE any side effect of execution:
    // CAS(Pending -> Running) against Cancel()'s CAS(Pending -> Cancelled) on
    // the same status atomic. Exactly one side wins.
    if (auto data = handle.GetTaskData()) {
        TaskStatus expected = TaskStatus::Pending;
        if (!data->Status.compare_exchange_strong(expected, TaskStatus::Running,
                                                  std::memory_order_seq_cst)) {
            // The canceller won: it fired the Cancelled callbacks and waiter
            // wakeups on its own thread, and (for graph tasks) already
            // retired the graph node. This side fires NOTHING and drops the
            // envelope — the UniquePtr destructor frees the closure here
            // (F14). Counters conserve: m_Counters.QueuedTasks was released at the
            // dequeue-success point and the worker loop decrements
            // m_Counters.PendingTasks after this returns (F20).
            assert(expected == TaskStatus::Cancelled &&
                   "JobSystem: dequeued task in non-Pending, non-Cancelled state");
            return;
        }
    }

    if (requiresDependencyManagement) {
        // Dependency-aware path: flip the graph node Pending → Running. With
        // the execution CAS won above, no cancel/cascade can have erased the
        // node (cancel requires winning the same CAS; cascades only reach
        // parked payloads, and this one was dequeued) — a false return is
        // unreachable. Assert loudly rather than silently executing without a
        // graph-terminal (which would strand this task's dependents parked
        // forever); Release degrades to exactly that silent narrowing.
        const bool isInDependencyGraph = m_DependencyGraph->MarkRunning(taskId);
        assert(isInDependencyGraph &&
               "unreachable: execution CAS won but the graph node is gone — a cancel/cascade "
               "erased a node whose payload was already dequeued (F12 arbitration breach)");

        try {
            {
                // The body only: the graph release below runs outside it.
                JobBodyScope body(context);
                task->Execute();
            }

            if (isInDependencyGraph) {
                CompleteGraphTask(taskId, true, String{}, nullptr);
            }
        } catch (const std::exception& e) {
            Logger::Log::Error("Task execution failed: {}", e.what());

            if (handle.IsValid()) {
                handle.SetException(std::current_exception());

                auto data = handle.GetTaskData();
                if (data) {
                    data->ErrorMessage = e.what();
                }
            }

            if (isInDependencyGraph) {
                CompleteGraphTask(taskId, false, e.what(), std::current_exception());
            }
        } catch (...) {
            const String msg = "Task " + std::to_string(taskId) + " threw a non-standard exception";
            Logger::Log::Error("{}", msg);

            if (handle.IsValid()) {
                handle.SetException(std::current_exception());

                auto data = handle.GetTaskData();
                if (data) {
                    data->ErrorMessage = msg;
                }
            }

            if (isInDependencyGraph) {
                CompleteGraphTask(taskId, false, msg, std::current_exception());
            }
        }
    } else {
        // Ultra-fast path for simple tasks: no dependency graph interaction,
        // minimal overhead. The outcome locals cost nothing on success (SSO
        // empty string, null exception_ptr) — they exist for the graph-link
        // bridge below.
        bool success = true;
        String failureMessage;
        std::exception_ptr failureException;
        try {
            {
                // The body only: completion callbacks run outside it.
                JobBodyScope body(context);
                task->Execute();
            }
            if (handle.IsValid()) {
                handle.MarkCompleted();
            }
        } catch (const std::exception& e) {
            success = false;
            failureMessage = e.what();
            failureException = std::current_exception();
            Logger::Log::Error("Simple task execution failed: {}", failureMessage);
            if (handle.IsValid()) {
                // Exception BEFORE the terminal flip: MarkFailed's status
                // exchange is the release edge cross-thread observers read
                // from (waiters, LinkSubmitDependency's terminal recheck) —
                // writing the payload after it races those readers.
                handle.SetException(failureException);
                handle.MarkFailed(failureMessage);
            }
        } catch (...) {
            success = false;
            failureMessage = "Task " + std::to_string(taskId) + " threw a non-standard exception";
            failureException = std::current_exception();
            Logger::Log::Error("{}", failureMessage);
            if (handle.IsValid()) {
                // Exception before the terminal flip — see above.
                handle.SetException(failureException);
                handle.MarkFailed(failureMessage);
            }
        }

        // Graph-link bridge (Submit(F&&, deps)): if a dependent linked this
        // task as a dependency, its mirror node is waiting on this terminal —
        // release/cascade it now. ONE seq_cst load on the fast path, no lock,
        // no allocation (the plain-Submit alloc/lock pins hold). Sequenced
        // AFTER Mark*'s status exchange: the Dekker pair with
        // LinkSubmitDependency's flag-store-then-status-load, so a linker
        // that missed this completion sees the terminal status instead.
        if (auto data = handle.GetTaskData();
            data && data->GraphLinked.load(std::memory_order_seq_cst)) {
            CompleteLinkedDependency(taskId, success, failureMessage, failureException);
        }

        // Envelope death point, before the releaser queues the registry
        // cleanup at scope exit (F12). The closure is destroyed after the
        // completion events fire.
        task.reset();
    }
}

void WorkStealingThreadPool::CompleteGraphTask(TaskId taskId, bool success, const String& failureMessage,
                                               std::exception_ptr failureException) {
    // Pre-store the source's terminal status BEFORE the graph flip. The graph
    // mutex inside MarkCompleted is then the release edge that makes
    // graph-observed terminality imply handle-observed terminality: a
    // dependent released by ANOTHER thread (absent-dep at Register, or
    // cross-thread fan-in extraction) can start executing before this thread
    // reaches ProcessGraphActions, and its guarded TryGetResult on this task
    // must already see Completed. The MarkCompleted/MarkFailed below is an
    // idempotent re-exchange that fires the notify + callbacks exactly once
    // (terminal events are emitted once per id — the node is erased in the
    // same critical section that emits them). The source's ErrorMessage /
    // Exception were recorded by the worker's catch block before this call.
    // Cascade-FAILED dependents need no pre-store: an early TryGetResult
    // observing "not completed" on a genuinely failed dependency is the
    // correct outcome for the guarded callers.
    {
        TaskHandle sourceHandle(taskId, this);
        if (auto data = sourceHandle.GetTaskData()) {
            data->Status.store(success ? TaskStatus::Completed : TaskStatus::Failed);
        }
    }

    // ONE critical section inside MarkCompleted (F9): terminal flip,
    // dependents' remaining-dependency decrement, extraction of newly-ready
    // parked payloads, failure propagation, and erase of every node made
    // terminal (B4). Everything user-visible happens after unlock, below.
    TaskGraphActions actions = m_DependencyGraph->MarkCompleted(taskId, success);
    ProcessGraphActions(actions, taskId, failureMessage, failureException);
}

void WorkStealingThreadPool::ProcessGraphActions(TaskGraphActions& actions, TaskId sourceTaskId,
                                                 const String& failureMessage,
                                                 std::exception_ptr failureException) {
    // F10: runs with NO graph lock held. TaskHandle flips fire user callbacks
    // (which may Submit or Cancel and re-enter the graph), and EnqueueTask
    // takes a worker's park-slot mutex to wake it.
    //
    // NOTE: the guarded-TryGetResult contract (a released dependent observes
    // its dependency's TaskData terminal) is carried by CompleteGraphTask's
    // status PRE-STORE before the graph flip — not by the events-before-
    // enqueue order here, which only covers dependents released in THIS
    // actions set. Dependents released by another thread (absent-dep
    // Register, cross-thread fan-in) never see this loop's ordering.
    // Single-fire discipline (F12): terminal events for the SOURCE id are
    // owned by the caller — the executing worker (which won Pending→Running
    // and is the only possible producer of its Completed/Failed), or a
    // canceller that won Pending→Cancelled before calling in. Cascaded
    // dependents' events, however, can race a concurrent CancelTask on the
    // same dependent, so their fire is guarded by winning the dependent's own
    // Pending→{Failed,Cancelled} transition; the loser fires nothing.
    for (const auto& event : actions.Events) {
        TaskHandle handle(event.Id, this);
        if (!handle.IsValid()) {
            continue;
        }
        const bool isSource = (event.Id == sourceTaskId);

        switch (event.NewStatus) {
        case TaskStatus::Completed:
            // Only ever emitted for the source id (executor-exclusive).
            handle.MarkCompleted();
            break;
        case TaskStatus::Failed: {
            auto data = handle.GetTaskData();
            if (!isSource) {
                // Claim the dependent exclusively BEFORE seeding its error
                // info: CAS(Pending -> Running) loses only to a canceller
                // that already won this dependent's arbitration and fired.
                // Claiming with Running (not Failed) keeps the ordering
                // waiters rely on — a handle never reads terminal without
                // its ErrorMessage/Exception already seeded; MarkFailed
                // below performs the terminal flip + notify + callbacks.
                TaskStatus expected = TaskStatus::Pending;
                if (!data || !data->Status.compare_exchange_strong(
                                 expected, TaskStatus::Running, std::memory_order_seq_cst)) {
                    break; // a concurrent canceller won this dependent's fire
                }
                // Dependency-failure propagation: seed the dependent's
                // error info before its failure callbacks observe it. The
                // source task's own info was recorded by the worker in
                // ExecuteTaskOptimized's catch block.
                if (data->ErrorMessage.empty()) {
                    data->ErrorMessage = "Dependency task " + std::to_string(sourceTaskId) +
                                         " failed: " + failureMessage;
                }
                if (!data->Exception) {
                    data->Exception = failureException;
                }
            }
            handle.MarkFailed(data ? data->ErrorMessage : String{});
            break;
        }
        case TaskStatus::Cancelled: {
            // A cascaded dependent, or a cancel ORIGIN routed from Shutdown
            // step 4 (CancelAllPending emits origin events with sourceTaskId
            // 0, and nothing strips them). A user-facing Cancel() origin
            // never reaches here — CancelPendingArbitrated fires it directly
            // on its arbitrated TaskData and strips its event before this
            // loop. The CAS below keeps every path single-fire.
            auto data = handle.GetTaskData();
            TaskStatus expected = TaskStatus::Pending;
            if (!data || !data->Status.compare_exchange_strong(
                             expected, TaskStatus::Cancelled, std::memory_order_seq_cst)) {
                break; // a concurrent canceller won this dependent's fire
            }
            handle.MarkCancelled();
            break;
        }
        default:
            break;
        }

        // Registry cleanup does NOT happen per-event here. The TaskData
        // registry entry must outlive the task's ENVELOPE: a Cancelled task's
        // payload can still be sitting in a pool queue, and erasing its entry
        // lets the dequeuing worker's GetOrCreateTaskData re-materialize a
        // FRESH Pending TaskData — which then wins the F12 execution CAS and
        // resurrects the cancelled task. Cleanup is tied to envelope death
        // instead: the executing/dropping worker's TaskDataReleaser, the
        // retired-payload loop below, or the shutdown drain.
    }

    for (auto& ready : actions.ReadyTasks) {
        EnqueueTask(std::move(ready));
    }

    // Parked payloads of failed/cancelled dependents die here, outside every
    // lock — their destructors run user code (captured closures). This is
    // their envelopes' death point, so their registry entries release here
    // too (callers' TaskHandle copies keep their own shared_ptr).
    for (const auto& retired : actions.RetiredTasks) {
        if (retired && retired->NeedsTaskData()) {
            CleanupTaskData(retired->GetTaskId());
        }
    }
    actions.RetiredTasks.clear();
}

void WorkStealingThreadPool::CleanupTaskData(TaskId taskId) {
    // Call this only after the task's envelope is dead (F12). An erase while the
    // envelope is queued lets its worker make a fresh Pending TaskData and run a
    // cancelled task. The erase waits in this thread's batch, off the registry lock.
    t_pendingCleanup.push_back({this, taskId});

    // A full share of this pool drains the batch, so the registry stays bounded on a
    // thread that seldom submits. Entries of other pools do not count: they must
    // not make each push lock the registry.
    if (t_pendingCleanup.size() >= kPendingCleanupBatch)
    {
        size_t own = 0;
        for (const PendingCleanupEntry& entry : t_pendingCleanup)
        {
            own += (entry.Pool == this) ? 1 : 0;
        }
        if (own < kPendingCleanupBatch)
        {
            return;
        }
        TaskDataReleaseBatch released;
        std::lock_guard<std::mutex> lock(m_TaskDataMutex);
        DrainPendingCleanupLocked(released);
    }
}

void WorkStealingThreadPool::EnqueueTaskGlobal(UniquePtr<TaskBase> task, JobPriority priority) {
    if (m_InlineMode) {
        // Inline mode: execute at publish (see EnqueueTask). Priority orders
        // nothing when execution is synchronous; the body still runs as its
        // class.
        ExecuteInlineBounded(std::move(task), ComputeContext(priority));
        return;
    }
    // Same publish sequence as EnqueueTask (§7): counters BEFORE the push (F5),
    // wake decision after. Always a global lane — never a local queue.
    // m_Counters.QueuedTasks counts every class, so the wake protocol below is
    // priority-blind; the lane occupancy gate follows the same
    // before-push direction.
#if GE_DEBUG_INSTRUMENTATION
    // Bracket enter: in-flight FIRST, then epoch (see m_CounterBracketEpoch).
    m_CounterRacersInFlight.fetch_add(1, std::memory_order_seq_cst);
    m_CounterBracketEpoch.fetch_add(1, std::memory_order_seq_cst);
#endif
    m_Counters.PendingTasks.fetch_add(1, std::memory_order_relaxed);
    m_Counters.QueuedTasks.fetch_add(1, std::memory_order_seq_cst); // (P1)
    if (std::atomic<uint32>* occupancy = LaneOccupancy(priority)) {
        occupancy->fetch_add(1, std::memory_order_relaxed);
    }
    PushGlobalQueue(std::move(task), priority);
#if GE_DEBUG_INSTRUMENTATION
    m_CounterRacersInFlight.fetch_sub(1, std::memory_order_seq_cst);
#endif
    NotifyAfterPublish(); // (P2..P4)
}

ConcurrentQueue<UniquePtr<TaskBase>>& WorkStealingThreadPool::PriorityLane(JobPriority priority) {
    switch (priority) {
        case JobPriority::Awaited:
            return m_Queues.Awaited;
        case JobPriority::Background:
            return m_Queues.Background;
        case JobPriority::Normal:
            break;
    }
    return m_Queues.Global;
}

std::atomic<uint32>* WorkStealingThreadPool::LaneOccupancy(JobPriority priority) {
    switch (priority) {
        case JobPriority::Awaited:
            return &m_Counters.AwaitedQueued;
        case JobPriority::Background:
            return &m_Counters.BackgroundQueued;
        case JobPriority::Normal:
            break;
    }
    return nullptr;
}

std::atomic<uint64>& WorkStealingThreadPool::LanePushes(JobPriority priority) {
    switch (priority) {
        case JobPriority::Awaited:
            return m_Census.AwaitedPushes;
        case JobPriority::Background:
            return m_Census.BackgroundPushes;
        case JobPriority::Normal:
            break;
    }
    return m_Census.GlobalPushes;
}

void WorkStealingThreadPool::PushGlobalQueue(UniquePtr<TaskBase> task, JobPriority priority) {
    // Allocating tokenless enqueue: fresh blocks are allocated on demand and
    // recycled after warmup, so this fails only on genuine OOM. Until slice 7
    // this lane was a try_enqueue (never-allocate) queue spilling into a
    // second allocating queue — the census measured the spill at 0 across
    // ~450k pushes, so the two-queue dance bought nothing and cost every
    // consumer a second empty poll per scan. (The background lane does not
    // regress that: every poll of it is gated on m_Counters.BackgroundQueued.)
    if (!PriorityLane(priority).enqueue(std::move(task))) {
        // A silently dropped task can be a caller-barrier release (B7) or a
        // graph dependency — dropping is a permanent hang, strictly worse
        // than terminating an already-OOM'd process (slice-4 discipline).
        std::fprintf(stderr, "WorkStealingThreadPool: global queue allocation failed under "
                             "OOM; aborting rather than dropping queued work.\n");
        std::fflush(stderr);
        std::abort();
    }
    LanePushes(priority).fetch_add(1, std::memory_order_relaxed);
    CountPublishedOnThisThread(1);
}

void WorkStealingThreadPool::PublishInScratchRuns(
    size_t count,
    GameEngine::FunctionRef<void(size_t first, size_t run, UniquePtr<TaskBase>* staging, void** slabs)> publishRun) {
    // The scope is open for every run, and the arrays hold no task after their run's push.
    GameEngine::Memory::ScratchScope scratch;
    const size_t room = std::min(count, kMaxBulkRun);
    const std::span<void*> slabs = scratch.AllocateArrayForOverwrite<void*>(room);
    std::pmr::vector<UniquePtr<TaskBase>> staging(room, &scratch.Resource());
    for (size_t first = 0; first < count; first += room) {
        publishRun(first, std::min(count - first, room), staging.data(), slabs.data());
    }
}

void WorkStealingThreadPool::EnqueueBulkGlobal(UniquePtr<TaskBase>* tasks, size_t count,
                                               JobPriority priority) {
    // ALLOCATING enqueue_bulk (see the declaration comment for why
    // try_enqueue_bulk is not used). The caller already published the
    // counters for the whole run (F5), the background occupancy included.
    const bool ok = PriorityLane(priority).enqueue_bulk(std::make_move_iterator(tasks), count);
    if (!ok) {
        // OOM-only path (audit F3): the counters are already published and
        // barrier chunks may be in this run — dropping it would strand
        // callers and poison the counters forever. A failed enqueue_bulk
        // returns BEFORE constructing any element (audit-verified), so the
        // source range is un-moved and a per-task retry is safe; if even
        // single-element allocation fails, fail LOUD.
        assert(false && "enqueue_bulk failed — falling back to per-task publish");
        for (size_t i = 0; i < count; ++i) {
            PushGlobalQueue(std::move(tasks[i]), priority);
        }
        return;
    }
    LanePushes(priority).fetch_add(count, std::memory_order_relaxed);
    CountPublishedOnThisThread(count);
}

} // namespace JobSystem
