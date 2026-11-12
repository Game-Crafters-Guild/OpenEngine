#pragma once

#include "JobSystem/AvailableProcessors.h"
#include "JobSystem/ConcurrentQueue.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/JobSystemStatistics.h"
#include "JobSystem/ParallelForOptions.h"
#include "JobSystem/TaskBase.h"
#include "JobSystem/TaskHandle.h"
#include "JobSystem/TaskSlabPool.h"
#include "JobSystem/TaskTypes.h"
#include "JobSystem/Types.h"
#include "Logger/Logger.h"
#include "Memory/FrameJoinedScope.h"
#include "Types/FalseSharing.h"
#include "Types/FunctionRef.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace JobSystem
{

struct TaskGraphActions;

namespace Detail
{
// One member's bytes and its writer group, for GroupsNeverShareALine.
struct GroupedMember
{
    size_t Offset;
    size_t Size;
    size_t Group;
};

// True when members of different groups are a full false-sharing separation apart, and each member is a full
// separation from both ends of the struct: then no two groups, and no group and a member outside the struct,
// share a line at any address of the struct.
template <size_t N>
constexpr bool GroupsNeverShareALine(const GroupedMember (&members)[N], size_t structSize)
{
    for (const GroupedMember& member : members)
    {
        if (member.Offset < GameEngine::kFalseSharingSeparation ||
            member.Offset + member.Size + GameEngine::kFalseSharingSeparation > structSize)
        {
            return false;
        }
        for (const GroupedMember& other : members)
        {
            if (other.Group != member.Group && other.Offset > member.Offset &&
                other.Offset < member.Offset + member.Size + GameEngine::kFalseSharingSeparation)
            {
                return false;
            }
        }
    }
    return true;
}

class BlockingThreads;
class ChannelSlot;
class JobChannelState;
class HandleTask;
template <typename F>
class SubmitTask;
template <typename F>
class SubmitGraphTask;
template <typename F>
class BareWorkTask;
} // namespace Detail

class WorkStealingThreadPool;

/**
 * @brief Run `body(unit)` once for every unit in [0, count), claimed one at
 * a time by the calling thread and by helper tasks on `pool`: up to
 * `options.Helpers` from the start, and more while
 * `options.HelpersToKeepQueued` asks for them; legal on any thread, a pool
 * worker included.
 *
 * Helpers are bare pool tasks published at the class of the pool body running
 * on this thread, or at Background when options.HelperPriority asks for it
 * (ParallelForOptions::HelperPriority): one batch at the start and, with
 * HelpersToKeepQueued, a batch before any claim of the caller. The caller claims and
 * runs units itself until none is left or the run is stopped, then closes
 * the run and waits only for the units in progress on other threads, parked
 * on an atomic word (std::atomic::wait). A helper still queued at that point
 * finds the run closed when it runs and ends without touching the caller's
 * body, Stop or Admission, so the call never waits on a unit nobody has
 * started, needs no participation in the pool's queues and no JobCounter,
 * and is legal on a worker, the main thread or any other thread. On a thread
 * that cannot block (the browser main thread) the wait services the thread's
 * proxied calls instead of parking.
 *
 * Stopping: a body that returns false, a body that throws, options.Stop
 * returning true or throwing, an admission hook throwing, or
 * options.HelpersToKeepQueued throwing stops the run: no further unit starts
 * on any thread, units in flight finish.
 *
 * Frame memory: under GE_DEBUG_INSTRUMENTATION every unit runs with the
 * frame-joined bit (Memory::Detail::IsFrameJoined) of the calling thread, on
 * a helper as on the caller.
 *
 * @param pool The pool helpers publish to; null runs every unit on the
 *        caller, in order, with the same stop rules.
 * @param count The number of units.
 * @param body Callable as void(size_t unit) or bool(size_t unit); false
 *        stops the run. Runs on several threads at once; each unit must
 *        touch only its own output.
 * @param options Helpers, their lane, the stop poll, the helper admission and
 *        the count of helpers to keep queued.
 * @return False when a body returned false or options.Stop returned true (a
 *         stopped run, even when no unit was left to skip), true otherwise;
 *         always true for a void body without a Stop.
 * @throws The first exception a body, options.Stop, an admission hook or
 *         options.HelpersToKeepQueued threw, on any thread, rethrown here
 *         once every unit in flight has finished.
 * @note No helper is published in inline mode or once Shutdown() has been
 *       requested, and none re-publishes after that: the caller runs what is
 *       left. The pool keeps at most
 *       WorkStealingThreadPool::kMaxUnstartedForkTasksPerWorker helpers
 *       per worker published and not yet started in each lane, across every
 *       caller; a fork publishes only the helpers that fit and the caller
 *       claims the rest. The run's state is one 128-byte slab and the helpers
 *       are slab envelopes: no heap allocation at steady state. The chunked
 *       overload and ParallelSort are in JobSystem/ParallelAlgorithms.h.
 */
template <typename Body>
bool ParallelFor(WorkStealingThreadPool* pool, size_t count, Body&& body, const ParallelForOptions& options = {});

/**
 * @brief The engine's job scheduler: one global MPMC queue, per-worker local
 * queues, a steal ring, an Awaited lane and a Background lane.
 *
 * - The Awaited lane is one more global MPMC queue, for JobPriority::Awaited:
 *   jobs that a thread is blocked waiting for. Every scan (the worker lap, the
 *   spin poll and the retiring spinner's recheck) polls it first, before the
 *   worker's own local queue, so an Awaited job waits only for the remainder
 *   of the bodies already running. Each poll is gated on m_Counters.AwaitedQueued, as
 *   the Background lane's is, and the lane has the Background lane's rules.
 * - The global queue (moodycamel; it shards internally per producer) carries
 *   the Normal Submit(F&&) handle envelopes, EnqueueWork and EnqueueWorkBatch
 *   bare envelopes and JobCounter visits, and the graph tasks published from
 *   threads that are not workers of this pool. Producers publish tokenless;
 *   workers consume through their own ConsumerTokens for cross-producer
 *   fairness (see Worker::GlobalConsumerToken).
 *   An Awaited or Background publish goes to the Awaited or Background lane.
 * - A worker's local queue has one feed: graph tasks published from that
 *   worker (a completion releasing its ready dependents, or a
 *   Submit(F&&, dependencies) on the worker). Owners consume their own
 *   extractions first, for cache locality.
 * - The steal ring lets an idle worker take from other workers' local queues
 *   in ring order, tokenless, giving up after half the ring. A task in a busy
 *   worker's local queue can therefore be invisible to a given thief; the
 *   sleep predicate (m_Counters.QueuedTasks > 0) keeps such a worker re-scanning
 *   instead of sleeping, a busy-poll bounded by the backlog's lifetime
 *   (counted in the census as EmptyLoopsWithBacklog).
 * - The Background lane is one more global MPMC queue, for
 *   JobPriority::Background, polled last in every scan (the worker lap, the
 *   spin poll and the retiring spinner's recheck). Each poll is gated on the
 *   m_Counters.BackgroundQueued occupancy counter, so an empty lane costs one relaxed
 *   load. Background work never enters a local queue or the steal ring;
 *   graph tasks are always Normal, and a fork (JobCounter visits, ParallelFor
 *   helpers) publishes at the class of the body that forks, so a Background
 *   body's forks stay Background. m_Counters.QueuedTasks counts
 *   every class, so all share one wake protocol and differ only in dequeue
 *   order. Starvation runs one way: a saturating Normal flood postpones
 *   Background work indefinitely (there is no aging), while Background work
 *   delays Normal work only by the remainder of a body already running.
 * - Wake protocol: each worker parks on its own slot and sets its bit in an
 *   idle mask. A waker claims a set bit and signals only that slot, so no
 *   lock is shared between workers. Up to m_MaxSpinners workers spin before
 *   they park (100 us by default), and every dequeue passes one wake along
 *   while a backlog and idle workers remain. A parked worker also wakes on a
 *   backstop timeout (1 ms by default), as a liveness rescue.
 * - Blocking threads run the jobs of JobChannels (JobChannel.h): work whose
 *   thread is off-CPU for most of the job (another process, the OS, a
 *   driver). They are created on a channel's first dispatch, up to the sum of
 *   the registered channels' caps and never above the budget given at
 *   construction, and never retired. They take work only from their own
 *   FIFO, never from the compute queues, and never touch the wake protocol
 *   or its counters.
 * - Shutdown executes queued bare tasks and cancels queued handle and graph
 *   tasks (see Shutdown()). The priority class orders dequeue only, never
 *   lifetime: queued bare Background tasks execute in the post-join drain
 *   like bare Normal tasks, because dropping a task without a handle turns a
 *   shutdown race into a caller hang. Work that should die at shutdown
 *   belongs on Submit(fn, JobPriority::Background), which is cancelled like
 *   every handle task. After Shutdown() returns, EnqueueWork(fn, Background)
 *   runs synchronously on the caller.
 */
class WorkStealingThreadPool
{
  public:
    /**
     * @brief Create the pool and start its workers.
     * @param numThreads Number of worker threads (default: the processors this
     *        process may run on, AvailableProcessors::Query()). 0 selects
     *        inline mode: no worker threads exist and every submission
     *        executes synchronously on the submitting thread at publish
     *        time (dependency chains recurse up to a stack bound).
     *        For single-threaded platforms (wasm without SharedArrayBuffer)
     *        and deterministic tests.
     * @param blockingThreadBudget The most blocking threads the pool creates
     *        for its JobChannels (the engine passes
     *        Platform::BlockingThreadBudget()). At least 1 unless numThreads
     *        is 0, at most kMaxBlockingThreadBudget; ignored in inline
     *        mode, where a channel job runs on the submitting thread.
     */
    explicit WorkStealingThreadPool(
        size_t numThreads = AvailableProcessors::Query().Count,
        size_t blockingThreadBudget = kMaxBlockingThreadBudget);

    /** @brief True when constructed with 0 workers (inline execution). */
    bool IsInlineMode() const { return m_InlineMode; }

    /**
     * @brief Shuts the pool down (see Shutdown()).
     */
    ~WorkStealingThreadPool();

    /**
     * @brief Submit a callable and get a handle to its outcome.
     * @param task Callable to execute; a non-void return is stored as the
     *        handle's result
     * @param priority The lane it is published to
     * @return TaskHandle for status, result and cancellation
     * @note The callable does not enter the dependency graph. One TaskData is
     *       created under the registry mutex; the returned handle, the
     *       envelope (from the 128-byte slab pool, or the heap for a closure
     *       too large for a slab, chosen at compile time) and the execute side
     *       all share it, so execution never looks it up again.
     * @note Use Submit(F&&, dependencies) when the task must run after others.
     * @note Gated on shutdown: once Shutdown() has been requested this returns
     *       an invalid handle and the callable never runs. A Submit that passes
     *       the gate concurrently with Shutdown() re-checks after its publish
     *       and cancels its own task; the returned handle is then valid and
     *       Cancelled, so Wait() always returns.
     * @note JobPriority::Background publishes into the Background lane, polled
     *       last by every consumer, and JobPriority::Awaited into the Awaited
     *       lane, polled first; Cancel, Wait and shutdown cancellation
     *       behave exactly as for Normal.
     */
    template <typename F, typename = std::enable_if_t<std::is_invocable_v<F&>>>
    TaskHandle Submit(F&& task, JobPriority priority = JobPriority::Normal);

    /**
     * @brief Submit a callable that runs only after every handle in
     * `dependencies` is terminal: the callable form of the dependency-graph
     * path.
     *
     * The callable is a graph task (Detail::SubmitGraphTask) registered in the
     * dependency graph, so graph registration, the rule that an absent
     * dependency is terminal, the failure cascade, the cancel cascade and
     * shutdown coverage are the graph path's own. It costs a graph node and a
     * heap envelope like any graph task, and always runs at
     * JobPriority::Normal (graph tasks never use the Background lane).
     * A non-void return is stored as the handle's result.
     *
     * Dependencies may come from any submission path of this pool, plain
     * Submit(F&&) included, whose tasks never register in the graph: a live
     * dependency outside the graph is bridged with a payload-less mirror node
     * (TaskDependencyGraph::EnsureTracked) whose terminal flip is driven by the
     * dependency's own completion through TaskData::GraphLinked. A plain
     * Submit(F&&) pays one flag load at completion for this, with no lock and
     * no allocation.
     *
     * A terminal or invalid handle in `dependencies` adds no edge, and the task
     * runs: dependents of completed, failed or cancelled dependencies run and
     * observe the outcomes through the handles. A dependency that fails while
     * the dependent is parked in the graph cascades Failed to it instead. A
     * handle from another pool is a contract violation:
     * Debug-asserted, skipped in Release.
     */
    template <typename F, typename = std::enable_if_t<std::is_invocable_v<F&>>>
    TaskHandle Submit(F&& task, std::span<const TaskHandle> dependencies);

    /**
     * @brief Fire-and-forget: enqueue one callable without a TaskHandle or
     * TaskData. The caller owns all synchronization.
     *
     * Cost per call: one pooled envelope (a 128-byte slab recycled through the
     * slab freelist; a closure too large for a slab falls back to the heap,
     * chosen at compile time, see TaskSlabPool.h), one queue push and one
     * bounded wake decision. A homogeneous group of callables belongs in
     * EnqueueWorkBatch (one bulk queue operation, one bounded wake).
     *
     * Shutdown: EnqueueWork never drops work. Bare tasks are the release edge
     * of caller barriers (ParallelFor helpers reference the
     * blocked caller's stack), so dropping one turns a shutdown race into a
     * caller hang. If shutdown is observed after the publish, this call drains
     * the queues on the calling thread: bare tasks (this one and possibly other
     * callers') execute, handle and graph tasks are cancelled. After Shutdown()
     * has returned, the callable therefore runs synchronously on the calling
     * thread. The rule is the same for every priority class.
     */
    template <typename F, typename = std::enable_if_t<std::is_invocable_v<F&>>>
    void EnqueueWork(F&& func, JobPriority priority = JobPriority::Normal);

    // EnqueueWorkBatch stages a batch of at most kStackBatch envelopes on the stack, and a larger one in the calling
    // thread's scratch arena in runs of at most kMaxBulkRun (16 KiB of scratch). Each run is one bulk push.
    static constexpr size_t kStackBatch = 64;
    static constexpr size_t kMaxBulkRun = 1024;
    // A smaller batch uses the per-task publish loop: a bulk push hides every
    // task until the last one, so a small warm fork loses its publish-execute overlap.
    static constexpr size_t kBulkPublishThreshold = 8;

    // Fork tasks published and not yet started, across every caller, are at
    // most this many per worker in each lane: ParallelFor's helpers and Run's
    // queue visits. A ParallelFor publishes only the helpers that fit its
    // helpers' lane (none: the caller runs every unit); a Run past the budget
    // publishes a visit only when its queue has none pending, since one
    // visit drains every job of its queue. The budget: two forks at full
    // width from threads that are no workers (2 per worker), plus the copies
    // running helpers re-publish between units without checking the budget
    // (at most one per worker): 3, taken as 4. The tasks of joined forks are
    // what the bound limits: a caller that forks back to back faster than the
    // workers drain them would otherwise queue them, and the slabs they hold,
    // without limit. Each lane has its own budget, so cook helpers queued
    // behind a streaming flood never take a Normal fork's.
    static constexpr size_t kMaxUnstartedForkTasksPerWorker = 4;

    /**
     * @brief Fire-and-forget batch: publish `count` callables of one closure
     * type with bulk queue operations and one bounded wake, in place of
     * `count` push and notify pairs. The elements of `funcs` are moved from.
     * The envelopes are bare, as with EnqueueWork: no TaskHandle, no TaskData,
     * and the caller owns synchronization.
     *
     * Publication (count >= kBulkPublishThreshold): envelopes are staged in
     * one run (on the stack up to kStackBatch, else in scratch; a batch above
     * kMaxBulkRun makes one run per kMaxBulkRun), counted into the wake and
     * backpressure counters before each bulk push (so a fast consumer cannot
     * underflow them), and published with moodycamel's allocating enqueue_bulk.
     * It fails only when an allocation fails, and then before any element is
     * moved, so EnqueueBulkGlobal falls back to per-task publication rather
     * than dropping work. try_enqueue_bulk is not used: it fails whenever the
     * queue's preallocated blocks or index run out, which would put that
     * fallback on every call. Batches below the threshold publish through the
     * per-task path (see kBulkPublishThreshold): the same observable
     * semantics, per-task visibility, and the single-task wake decision (at
     * most one notify per task) in place of the batch wake below.
     *
     * Wake policy: after the whole batch is pushed, the caller claims and
     * signals one idle worker when armed spinners do not cover the queued
     * tasks. Every successful dequeue on a worker passes one more wake along
     * while a backlog and idle workers remain (AccountDequeuedTask), so the
     * batch reaches full worker width without notify_all, which would wake a
     * thundering herd that then thrashes the steal ring.
     *
     * Shutdown (as EnqueueWork): never drops work. After Shutdown() has
     * returned the batch executes synchronously on the calling thread; a batch
     * racing Shutdown() re-checks after its publish and drains. Every callable
     * runs exactly once: on a worker, in Shutdown()'s post-join drain, or on
     * this thread. The rule is the same for every priority class.
     */
    template <typename F, typename = std::enable_if_t<std::is_invocable_v<F&>>>
    void EnqueueWorkBatch(F* funcs, size_t count, JobPriority priority = JobPriority::Normal);

    /** @brief EnqueueWorkBatch(F*, size_t) over a span. */
    template <typename F, typename = std::enable_if_t<std::is_invocable_v<F&>>>
    void EnqueueWorkBatch(std::span<F> funcs, JobPriority priority = JobPriority::Normal);

    /**
     * @brief Fork-join submit: run `fn` on the pool, counted into `counter`.
     *
     * No TaskHandle, no TaskData, no dependency graph: completion is observed
     * only through the counter (join with Wait(counter)). The counter is
     * incremented here, before the task is published, and decremented by a
     * scope guard on the task's completion path, so a throwing task always
     * releases its waiter (the exception is logged and sets the counter's
     * sticky HasAnyFailed(); there is no per-task failure observation).
     *
     * The closure is queued in the counter's own tagged queue and a visit to
     * that queue (a bare task) is published to the pool while the lane's
     * budget of unstarted fork tasks lasts, else only when no visit to the
     * queue is pending. Whichever consumer reaches the envelope first, a
     * worker running a visit or a participating Wait() on the same counter,
     * executes it; a visit drains the queue until it is empty, and a late
     * visit finds nothing and returns. The pool's counters, wake protocol and
     * shutdown drain see only the visit, which is an ordinary EnqueueWork
     * task. If the tagged enqueue fails (an allocation failure, the
     * allocating enqueue's only failure mode), the envelope executes inline on
     * this thread: never dropped, never a hung waiter.
     *
     * Lifetime: once the envelope is consumable, another thread can complete
     * it, drive the counter to zero, release a waiter and destroy the counter.
     * The envelope therefore becomes consumable only inside this call's one
     * critical section of the counter mutex, where the parked-waiter notify
     * also happens, and after that critical section Run touches only its own
     * reference to the tagged queue, never `counter`.
     *
     * Shutdown: as EnqueueWork, the visit is never dropped, so the envelope
     * always executes and the counter always drains. After Shutdown() has
     * returned, `fn` runs synchronously on the calling thread.
     *
     * The counter must outlive the join: call Wait(counter) before it is
     * destroyed. Every task of one counter must target the same pool.
     *
     * Priority: Run() takes no JobPriority; its visits publish at the class
     * of the pool body running on this thread (Background inside a
     * Background body, Normal everywhere else), so a Background body's fork
     * never publishes Normal work.
     *
     * Frame memory: under GE_DEBUG_INSTRUMENTATION `fn` runs with the
     * frame-joined bit (Memory::Detail::IsFrameJoined) of the thread that
     * called Run, on whichever thread runs it. `fn` is joined by the frame
     * only when that thread waits `counter` before its frame ends, as
     * Query::Parallel and the ECS system waves do; frame-joined code
     * publishes work that it polls across frames through Submit or
     * EnqueueWork instead. A Submit, EnqueueWork, EnqueueWorkBatch or channel
     * job body runs with the bit clear.
     */
    template <typename F, typename = std::enable_if_t<std::is_invocable_v<F&>>>
    void Run(F&& fn, JobCounter& counter);

    /**
     * @brief Join on a JobCounter. Returns once every task counted into
     * `counter` has completed; the tasks' writes happen-before the return.
     *
     * Every waiter participates, on every thread: a worker of this pool, the
     * main thread, a worker of another pool, a blocking thread. It executes
     * only tasks tagged with this counter (consumed from the counter's own
     * queue, never an arbitrary drain of the pool queues), so a waiter never
     * runs a job that could wait on the waiter itself, and waiting on a
     * worker is deadlock-free. A tagged task the waiter runs is a job body of
     * the waiter's own current context (IsExecutingJob() is true inside it;
     * Normal outside a body). A waiter that finds no tagged task (the rest
     * are running on other threads, or the counter is a barrier for bare
     * chunks with no tagged queue) parks on the counter's condvar and is
     * woken by the last decrement or by a new tagged publish. A thread that
     * cannot block (Platform::CanBlockCurrentThread() is false) parks in
     * 10 s slices and reports each one on stderr.
     *
     * A participating waiter is neither a spinner nor a sleeper of the pool:
     * it never touches the pool's wake protocol or its queued-task counters
     * (the visits carry those through the normal worker path).
     *
     * Participation covers only tasks submitted with Run(fn, counter). A
     * counter used as a barrier for work published another way (bare chunks)
     * must be joined from a thread that is not a worker of this pool: the
     * waiter can only park on it.
     *
     * A caller must not hold, across this Wait, a lock that a task of the
     * counter takes: the caller may run that task itself.
     *
     * Destruction: only one thread may treat this Wait's return as permission
     * to destroy the counter. A second concurrent waiter can still be inside
     * the condvar's wait when the first destroys it, which is undefined
     * behaviour (see the contract block in JobCounter.h). Every in-tree
     * counter has a single waiter.
     */
    void Wait(JobCounter& counter);

    /**
     * @brief Create a handle born terminal (Completed) carrying `result` as
     * its payload; nothing is enqueued for it and no task body exists.
     *
     * For callers whose result already exists (an asset that is already
     * loaded): submitting a real task and force-completing its handle from the
     * caller races the worker's cancel arbitration CAS (Pending -> Running),
     * which then fails against Completed, a contract violation (Debug assert)
     * that leaks the graph node in Release.
     *
     * The returned handle reads IsDone()/IsCompleted() immediately and
     * TryGetResult<T> yields `result` (T is the decayed argument type; match it
     * to what the polling side requests). Cancel() is refused (already
     * terminal) and waiters never block. The payload is bound at creation:
     * TaskHandle has no public mutators (see its private mutator block).
     */
    template <typename T>
    TaskHandle CreateCompletedHandle(T result);

    /**
     * @brief Cancel a task by id. Single-fire: exactly one of the canceller
     * and the executing worker wins.
     *
     * Cancel performs CAS(Pending -> Cancelled) on the task's status while the
     * worker performs CAS(Pending -> Running) when it dequeues the task.
     *
     * - The canceller wins (returns true): the task body never executes. The
     *   Cancelled callbacks and the waiter wakeups fire exactly once, on the
     *   calling thread. For a dependency-managed task, its parked payload is
     *   retired and its pending dependents cascade to Cancelled. The queued
     *   envelope and its closure are not freed here: a worker frees them when
     *   it dequeues the envelope and loses the arbitration, so destruction
     *   waits for that dequeue, not for Cancel().
     * - The worker wins (returns false): the task is running or already
     *   terminal; a running task completes normally and its callbacks fire
     *   with its actual outcome, never Cancelled.
     *
     * Every submission path that returns a TaskHandle supports it, Submit(F&&)
     * included.
     *
     * @param taskId Task identifier
     * @return True if this call moved the task from Pending to Cancelled.
     */
    bool CancelTask(TaskId taskId);

    /**
     * @brief Number of worker threads (0 in inline mode).
     */
    size_t GetWorkerCount() const { return workers.size(); }

    // Backpressure signals, read by ECS Query::Parallel's batch coarsening and
    // by ProjectMaterialPrewarmService's pacing. Both exclude the queued
    // Background backlog: every consumer polls the Background lane last, so a
    // backlog there never delays a Normal submission, and a streaming flood
    // keeps 10k+ Background tasks queued as its steady state; coarsening ECS
    // batches against it would give up parallelism for no contention relief.
    // Background bodies that are running still count in GetPendingTasksApprox
    // (at most the worker count; they occupy workers). The subtraction
    // saturates at zero: the two relaxed loads can interleave with concurrent
    // publishes, and the occupancy counter over-counts during a producer's
    // publish window.
    // GetApproximateQueueSize counts queued Normal and Awaited tasks in every queue, from
    // the counter the sleep predicate reads. GetPendingTasksApprox also counts
    // tasks that are executing (queued plus in flight).
    size_t GetApproximateQueueSize() const
    {
        const uint32 queued = m_Counters.QueuedTasks.load(std::memory_order_relaxed);
        const uint32 background = m_Counters.BackgroundQueued.load(std::memory_order_relaxed);
        return queued > background ? queued - background : 0;
    }
    size_t GetPendingTasksApprox() const
    {
        const size_t pending = m_Counters.PendingTasks.load(std::memory_order_relaxed);
        const uint32 background = m_Counters.BackgroundQueued.load(std::memory_order_relaxed);
        return pending > background ? pending - background : 0;
    }

    /**
     * @brief Test hook: override the adaptive-spin configuration at runtime.
     *
     * The environment knobs (GE_JOB_SPIN_US, GE_JOB_MAX_SPINNERS) are read
     * once at construction; tests use this instead of mutating the process
     * environment. spinWindowUs == 0 disables the spin path. Lowering
     * maxSpinners does not preempt spinners already armed: the spinning count
     * can exceed the new cap until their windows expire.
     */
    void SetSpinConfigForTest(uint32 spinWindowUs, uint32 maxSpinners);

    /**
     * @brief Test hook: current number of armed spinner tokens.
     */
    uint32 GetSpinningCountForTests() const;

    /**
     * @brief Test hook: workers whose idle bit is set and not yet claimed by a
     * waker. A worker sets its bit before it reads the queued count and clears
     * it when it leaves the wait; a waker clears the bit when it claims the worker.
     */
    uint32 GetIdleCountForTests() const;

    /**
     * @brief Test hook: idle workers that batch publishes (PublishAndWakeBatch)
     * claimed and signaled, at most 1 per EnqueueWorkBatch call. Worker-side wakes
     * and the per-task path's wakes (NotifyAfterPublish) are not counted.
     */
    uint64 GetBatchNotifyCountForTests() const;

    /**
     * @brief Test hook: number of tasks tracked by the dependency graph.
     *
     * The graph erases terminal nodes eagerly, so this returns to zero once
     * every dependency-managed task has completed, failed or been cancelled;
     * a leak shows as a nonzero count.
     */
    size_t GetGraphTaskCountForTests() const;

    /**
     * @brief Test hook: live TaskData registry entries. A deferred cleanup
     * (the thread-local pending buffer) keeps an entry alive until the owning
     * pool drains that thread's buffer.
     */
    size_t GetTaskDataRegistrySizeForTests() const;

    /**
     * @brief The pool's threads, lanes, channels and cumulative queue traffic
     * (see JobSystemStatistics). For diagnostics: it allocates and takes each
     * registered channel's mutex in turn, so a thread that must not block
     * reads SnapshotOccupancy() instead.
     *
     * The census counters cost the per-task paths one relaxed read-modify-write
     * each, on paths that already pay two seq_cst ones; the idle-lap counters
     * live on their own cache line (see TopologyCensusCounters).
     * TasksExecuted is derived from them, so it adds no per-task cost.
     */
    JobSystemStatistics GetStatistics() const;

    /**
     * @brief What every pool thread is running, for a reader that must not
     * block on the pool: the main-thread hang watchdog reads it while the
     * main thread, or the pool, may be wedged.
     *
     * Fills `out` with one entry per compute worker, then one per blocking
     * thread created so far, up to out.size(), and returns the number of
     * entries written. Lock-free and allocation-free: relaxed loads of
     * per-thread slots that each thread writes when a job body starts and
     * ends. Each entry's fields are read independently, so a job starting or
     * ending during the read can pair one job's name with another's start
     * time; a job that runs long, the case the reader exists for, reads
     * consistently.
     */
    size_t SnapshotOccupancy(std::span<JobSystemStatistics::Occupancy> out) const;

    /**
     * @brief Jobs published into the compute queues so far: GlobalPushes +
     * LocalPushes + AwaitedPushes + BackgroundPushes of the census, as four relaxed loads
     * with no allocation and no lock. The ECS wave trace reads it at the start
     * and the end of a traced frame.
     */
    uint64 GetPublishedJobCount() const;

    /**
     * @brief Attributes publishes to a site: while it lives, every job the
     * calling thread publishes into a compute queue (a Run visit, an
     * EnqueueWork or Submit envelope, each element of an EnqueueWorkBatch, a
     * graph task) adds one to `sink`. Jobs published by other threads are not
     * counted, so a body another thread runs adds nothing. A body this
     * thread's own Wait runs inline (every Wait participates: it runs the jobs
     * tagged with its counter) publishes on this thread, so its publishes
     * count as this thread's.
     *
     * Scopes nest: the innermost one counts, and the enclosing one counts
     * again once it ends. `sink` is a plain integer written only by the
     * calling thread, so it must be read on that thread or after a join that
     * orders the thread's writes before the read. The ECS wave trace opens
     * one around each traced system body and one around a wave's publish
     * loop. Without a scope a publish pays one thread-local load.
     */
    class PublishCountScope
    {
      public:
        explicit PublishCountScope(uint32& sink);
        ~PublishCountScope();
        PublishCountScope(const PublishCountScope&) = delete;
        PublishCountScope& operator=(const PublishCountScope&) = delete;

      private:
        uint32* m_Enclosing;
    };

    /**
     * @brief Whether the calling thread is inside a job body, on any thread
     * and of any pool: a compute job, a channel job, a tagged job a
     * participating Wait runs, or a ParallelFor unit, whether a helper or the
     * forking caller runs it. False outside a body, completion callbacks
     * included (they run after the body's context has ended). Consumers: the
     * scripting ABI's engine-thread predicates.
     */
    static bool IsExecutingJob();

    /**
     * @brief Test hook: what the calling thread is executing as a job body:
     * "Normal", "Awaited" or "Background" for a compute job, the channel's name for a
     * channel job, null outside a job body (completion callbacks included).
     */
    static const char* GetCurrentJobForTests();

    /**
     * @brief Test hook: current Background-lane occupancy counter (above the
     * physical queue depth during a producer's publish window, zero at
     * quiescence).
     */
    uint32 GetBackgroundQueuedForTests() const;

    /**
     * @brief Test hook: override the sleep backstop timeout.
     *
     * The 1 ms default backstop is a liveness rescue that can mask a protocol
     * bug; the lost-wakeup stress test raises it so a lost wakeup shows as a
     * spike of hundreds of milliseconds instead of a 1 ms rescue.
     */
    void SetSleepBackstopForTest(uint32 backstopUs);

#if GE_DEBUG_INSTRUMENTATION
    /**
     * @brief Debug-only test hook: where a ParallelFor helper calls the hook
     * installed with SetClaimHelperHookForTests.
     *  - PassedOpenCheck: the helper saw the run open and units left on its
     *    first load and has not yet counted itself in progress.
     *  - CountedInProgress: the helper counted itself in progress and has
     *    not yet re-read the run's state.
     */
    enum class ClaimHelperPoint : uint8
    {
        PassedOpenCheck,
        CountedInProgress,
    };

    /**
     * @brief Debug-only test hook: install a function every ParallelFor
     * helper of every pool calls at each ClaimHelperPoint (null removes it).
     * A test holds a helper at a point by blocking inside the hook.
     */
    static void SetClaimHelperHookForTests(void (*hook)(ClaimHelperPoint point));

    /**
     * @brief Debug-only test hook: how many times ParallelFor callers, across
     * every pool, have parked waiting for units in progress.
     */
    static uint64 GetClaimCallerParksForTests();

    /**
     * @brief Debug-only test hook: where a visit to a Run's tagged queue calls
     * the hook installed with SetTaggedVisitHookForTests.
     *  - ProvedEmpty: the visit found the queue empty and stopped counting
     *    itself pending, and has not yet re-read the queue's job count.
     */
    enum class TaggedVisitPoint : uint8
    {
        ProvedEmpty,
    };

    /**
     * @brief Debug-only test hook: install a function every tagged-queue
     * visit of every pool calls at each TaggedVisitPoint (null removes it).
     * A test holds a visit at a point by blocking inside the hook.
     */
    static void SetTaggedVisitHookForTests(void (*hook)(TaggedVisitPoint point));

    /**
     * @brief Debug-only test hook: where the wake path calls the hook installed
     * with SetWakeHookForTests. `workerId` is the parked worker.
     */
    enum class WakePoint : uint8
    {
        BeforeIdleBit,   ///< The worker found no task and did not set its idle bit yet.
        AfterIdleBit,    ///< The bit is set. The worker did not read the queued count yet.
        BeforeSlotLock,  ///< The queued count is zero. The slot is not locked yet.
        BeforeBlock,     ///< The worker holds its slot lock and blocks next. The hook must not lock.
        AfterClaim,      ///< On the waker's thread: the bit is claimed, the slot is not locked yet.
        BeforeIdleClear, ///< The worker left its wait and did not clear its bit yet.
    };

    /**
     * @brief Debug-only test hook: install a function the wake path of every
     * pool calls at each WakePoint (null removes it). A test holds a worker
     * or a waker at a point by blocking inside the hook.
     */
    static void SetWakeHookForTests(void (*hook)(WakePoint point, size_t workerId));

    /**
     * @brief Debug-only test hook: the wake path's counts. All but FlagsSet count
     * over the pool's life. At quiesce, Claims equals Consumed + Merged + FlagsSet.
     */
    struct WakeCountsForTests
    {
        uint64 Claims = 0;        ///< Idle workers claimed by a waker (one flag write each).
        uint64 Consumed = 0;      ///< Flags a worker cleared when it left its wait.
        uint64 Merged = 0;        ///< Flag writes that found the flag already set.
        uint64 StaleConsumed = 0; ///< Flags already set when the worker locked its slot.
        uint64 FlagsSet = 0;      ///< Slots whose flag is set now.
    };
    WakeCountsForTests GetWakeCountsForTests() const;
#endif

    /**
     * @brief The calling thread's worker index, read from a thread-local that
     * is not checked against this pool (a worker of another pool reads its
     * index there).
     * @return The index, or SIZE_MAX on a thread that is no pool's worker
     */
    size_t GetCurrentWorkerId() const;

    /**
     * @brief True on a compute worker thread of @p pool; false on every other
     * thread, a worker of another pool included.
     */
    static bool IsComputeWorkerOf(const WorkStealingThreadPool* pool);

    /**
     * @brief Destroys the result and callbacks of every Completed or Failed task that only the
     * registry holds. The engine calls it while a native module image is still mapped, before the
     * unload takes away the code those results and callbacks came from. A task a handle holds stays.
     */
    void ReleaseFinishedTasks();

    /**
     * @brief Shut the pool down.
     *
     * Order (each step depends on the previous): gate -> join compute workers
     * -> release (drain, retire graph-parked payloads, registry sweep) -> join
     * blocking threads -> release again.
     *  - Every submit path is gated: Submit and JobChannel::Submit return an
     *    invalid handle, EnqueueWork drains on its caller (see its contract),
     *    ParallelFor runs every unit on the caller.
     *  - Compute workers stop taking new work and are joined; the tasks they
     *    are running complete normally. Blocking threads take no new work
     *    either, but keep running the job they hold.
     *  - The drain empties the pool queues, every registered channel's FIFO
     *    and the blocking threads' FIFO in one pass: queued handle and graph
     *    tasks are cancelled first, then queued bare tasks execute on the
     *    shutting-down thread (they release caller barriers; dropping one is
     *    a hang). Payloads parked in the dependency graph are retired with
     *    Cancelled events, and every non-terminal TaskData in the registry
     *    sweep's snapshot is marked Cancelled and its waiters notified.
     *  - Only then are the blocking threads joined: a job running on one may
     *    wait on a compute handle or on another channel's handle, and the
     *    release above made every such handle terminal or running.
     *  - The release runs again for whatever a finishing blocking job
     *    published or released.
     * A Submit racing the whole sequence can insert its TaskData after the
     * sweep's snapshot; the submitter closes that case itself, because every
     * submit path re-checks shutdown after its publish (a channel reads the
     * gate under the mutex of the FIFO it appends to) and cancels its own
     * task. Together, no TaskHandle::Wait() can outlive the pool.
     */
    void Shutdown();

    /**
     * @brief True once Shutdown() has been requested.
     */
    bool IsShuttingDown() const { return m_ShutdownRequested.load(); }

  private:
    // What a job body runs as. The envelope carries no class (TaskBase has
    // three flag bits and no lane), so the path that dequeued the job
    // supplies it: the lane it was taken from on a compute worker (local,
    // global and steal pops are Normal, the Background lane is Background),
    // the channel on a blocking thread, the class given at publish in inline
    // mode, and the recorded lane or channel in the shutdown drain.
    struct JobContext
    {
        // Non-null for a channel job: the channel's static literal.
        const char* Channel = nullptr;
        // A compute job's class; Normal for a channel job.
        JobPriority Class = JobPriority::Normal;
    };
    static JobContext ComputeContext(JobPriority priority) { return {nullptr, priority}; }
    static JobContext ChannelContext(const char* channel) { return {channel, JobPriority::Normal}; }
    // The calling thread's own current context, Normal outside a body: what
    // a job run on the caller's behalf (a participating Wait's tagged job, a
    // Run whose enqueue failed) runs as.
    static JobContext CallerContext();

    // One thread's occupancy, written only by that thread when a job body
    // starts and ends and read with relaxed loads by SnapshotOccupancy and
    // GetStatistics.
    struct OccupancySlot
    {
        // The running job's name (see JobSystemStatistics::Occupancy::Running).
        std::atomic<const char*> Running{nullptr};
        // Its start, for Background and channel jobs only.
        std::atomic<uint64> SinceNs{0};
        // A blocking thread's slot: set once its thread exists.
        std::atomic<bool> Live{false};
    };

    // The current-job context of a thread: what its innermost job body runs
    // as, when that body started, how deeply bodies nest (a participating
    // Wait inside a body, an inline publish inside an inline body), and the
    // occupancy slot the thread writes (null on threads that have none: the
    // main thread, the thread running the shutdown drain, threads outside the
    // pool). A job body is a job's Execute(), never its completion callbacks
    // or the graph release after it.
    struct ThreadJobState
    {
        OccupancySlot* Slot = nullptr;
        JobContext Context;
        uint64 SinceNs = 0;
        uint32 Depth = 0;
    };

    // A pool thread's own record: its occupancy slot and its job state, both
    // written by that thread on every job, alone on their false-sharing span
    // (m_ThreadRecords). The state lives here rather than in a thread_local:
    // the runtime packs threads' thread_local blocks next to one another, so
    // per-job writes there could share a line with another thread's.
    struct ThreadRecord
    {
        OccupancySlot Occupancy;
        ThreadJobState State;
    };

    // The calling thread's job state: its record's on a pool thread (bound
    // once when the thread starts), s_OwnJobState on any other thread.
    static thread_local ThreadJobState* s_JobState;
    static thread_local ThreadJobState s_OwnJobState;
    static ThreadJobState& CurrentJobState();

    // Sets the calling thread's current-job context, and its occupancy slot,
    // for one job body; restores the enclosing body's on destruction. Reads
    // the clock only for Background and channel jobs, so a Normal job costs
    // two relaxed stores to the thread's own slot.
    class JobBodyScope
    {
      public:
        explicit JobBodyScope(JobContext context);
        ~JobBodyScope();
        JobBodyScope(const JobBodyScope&) = delete;
        JobBodyScope& operator=(const JobBodyScope&) = delete;

      private:
        ThreadJobState& m_State;
        ThreadJobState m_Enclosing;
    };

    // steady_clock nanoseconds since its epoch: the occupancy start times and
    // the channels' queue waits.
    static uint64 SteadyClockNs();

    // The thread records: one per compute worker, then one per blocking
    // thread up to the budget. Null in inline mode.
    std::unique_ptr<GameEngine::FalseSharingPadded<ThreadRecord>[]> m_ThreadRecords;
    size_t m_BlockingRecordCount = 0;
    // Called first on blocking thread `index`: marks it a thread of this pool
    // that is no compute worker and binds its record (none past the budget).
    void BindBlockingThread(size_t index);

    // Runs a batch callable on the calling thread, as a job body of class
    // `priority`, with the exception handling a bare task gets on a worker
    // (ExecuteTaskOptimized's bare path). EnqueueWorkBatch's synchronous path
    // after Shutdown() uses it.
    template <typename F>
    static void RunBareInline(F& func, JobPriority priority);

    // ParallelFor after type erasure: the body is `invoke(body, unit)`; a
    // null pool runs every unit on the caller.
    template <typename Body>
    friend bool ParallelFor(WorkStealingThreadPool* pool, size_t count, Body&& body,
                            const ParallelForOptions& options);
    static bool RunParallelFor(WorkStealingThreadPool* pool, bool (*invoke)(void* body, size_t unit), void* body,
                               size_t unitCount, const ParallelForOptions& options);
    template <typename B>
    static bool InvokeParallelForUnit(void* body, size_t unit)
    {
        if constexpr (std::is_void_v<std::invoke_result_t<B&, size_t>>)
        {
            (*static_cast<B*>(body))(unit);
            return true;
        }
        else
        {
            return (*static_cast<B*>(body))(unit);
        }
    }

    // A worker thread and its queues. Every ConsumerToken here is
    // thread-affine: only the owning worker's thread may pass it to a dequeue.
    // The steal path and the drain, which run on other threads, dequeue
    // tokenless.
    struct Worker
    {
        std::thread Thread;
        ConcurrentQueue<UniquePtr<TaskBase>> LocalQueue;
        moodycamel::ProducerToken ProducerToken;
        moodycamel::ConsumerToken LocalConsumerToken;
        // Fair-scan tokens for the global lanes. moodycamel's tokenless
        // try_dequeue scores only the first three non-empty producer
        // sub-queues (newest first), dequeues from the biggest, and scans them
        // all only when that dequeue fails, so while a sustained flood from a
        // few producers keeps deep sub-queues non-empty, a low-rate producer's
        // small sub-queue is never tried and its tasks wait until the flood
        // drains (a Query::Parallel wave behind a streaming flood would
        // complete only at flood drain). A token consumer instead sticks to one
        // producer sub-queue, forces a rotation every 256 consumed items, and
        // on an empty sub-queue walks the entire producer ring, so every
        // producer's sub-queue is reached within a bounded number of rotations
        // whatever the flood depth. A failed dequeue still means every
        // sub-queue appeared empty, so the relap on m_Counters.QueuedTasks is the same
        // as for the tokenless path.
        moodycamel::ConsumerToken GlobalConsumerToken;
        moodycamel::ConsumerToken AwaitedConsumerToken;
        moodycamel::ConsumerToken BackgroundConsumerToken;

        explicit Worker(WorkStealingThreadPool& pool)
            : ProducerToken(LocalQueue), LocalConsumerToken(LocalQueue),
              GlobalConsumerToken(pool.m_Queues.Global),
              AwaitedConsumerToken(pool.m_Queues.Awaited),
              BackgroundConsumerToken(pool.m_Queues.Background)
        {
        }
    };

    Vector<UniquePtr<Worker>> workers;

    // The lanes' queues, each a full separation of char padding from the next and from the members around them: a
    // queue's head (producer list, free list) and its tail (consumer rotation) take writes from different threads.
    struct LaneQueues
    {
        char PadBeforeGlobal[GameEngine::kFalseSharingSeparation];
        // The Global lane (MPMC). Every Normal-priority bare, handle, batch and JobCounter-visit publish enqueues here
        // tokenless, and so does a graph task published from a thread that is not a worker of this pool. Workers
        // dequeue through their GlobalConsumerToken; the drain, which runs on any thread, dequeues tokenless.
        ConcurrentQueue<UniquePtr<TaskBase>> Global;
        char PadBeforeBackground[GameEngine::kFalseSharingSeparation];
        // The Background lane: Global's discipline (allocating tokenless enqueue; allocation failure aborts). Workers
        // dequeue through their BackgroundConsumerToken (GlobalConsumerToken tells why). Every scan polls it last, and
        // only when m_Counters.BackgroundQueued is not zero.
        ConcurrentQueue<UniquePtr<TaskBase>> Background;
        char PadBeforeAwaited[GameEngine::kFalseSharingSeparation];
        // The Awaited lane: the Background lane's queue type and discipline, with
        // its own consumer token and occupancy counter (m_Counters.AwaitedQueued). Consumed
        // first in every scan.
        ConcurrentQueue<UniquePtr<TaskBase>> Awaited;
        char PadAfterAwaited[GameEngine::kFalseSharingSeparation];
    };
    // One row per member: a member without a row is not checked.
    static_assert(Detail::GroupsNeverShareALine(
                      {
                          {offsetof(LaneQueues, Global), sizeof(LaneQueues::Global), 0},
                          {offsetof(LaneQueues, Background), sizeof(LaneQueues::Background), 1},
                          {offsetof(LaneQueues, Awaited), sizeof(LaneQueues::Awaited), 2},
                      },
                      sizeof(LaneQueues)),
                  "LaneQueues: no two queues share a line");
    LaneQueues m_Queues;

    // One worker's wait. The worker blocks on Condition until Signaled or
    // shutdown. Signaled is read and written only under Mutex, a leaf lock:
    // nobody takes another lock while holding it.
    struct ParkSlot
    {
        std::mutex Mutex;
        std::condition_variable Condition;
        bool Signaled = false;
    };
    // Read-mostly: construction writes them; every park and every wake reads them. One slot and one idle bit per
    // worker, each slot and mask word on its own line (none in inline mode). A waker claims a worker when its
    // fetch_and clears the bit.
    UniquePtr<GameEngine::FalseSharingPadded<ParkSlot>[]> m_ParkSlots;
    UniquePtr<GameEngine::FalseSharingPadded<std::atomic<uint64>>[]> m_IdleMask;
    size_t m_ParkSlotCount = 0;
    size_t m_IdleMaskWords = 0;
    // Fills the handles to the bytes of a std::condition_variable and a std::mutex, so m_Counters and the members
    // after it sit at the offsets of the constructor's MeasuredLayout: per-task enqueue time changes with the
    // counters' placement. The constructor checks the offset at compile time.
    [[maybe_unused]] char m_PadKeepsCounterPlacement[sizeof(std::mutex) + sizeof(std::condition_variable) -
                                                     sizeof(m_ParkSlots) - sizeof(m_IdleMask) -
                                                     sizeof(m_ParkSlotCount) - sizeof(m_IdleMaskWords)];

    // Wake protocol state: the atomics that threads write on every task or every idle lap, in writer groups. A full
    // separation of char padding comes before each group and after the last, so no two groups, and no group and a
    // member outside the struct, share a line at any address of the pool. Not alignas: see the alignof check (.cpp).
    struct CounterLines
    {
        char PadBeforeTaskCounts[GameEngine::kFalseSharingSeparation];
        // Task counts: every publish and every dequeue or completion writes them; idle scans read them every lap.
        // QueuedTasks counts queued tasks in every queue, not in flight: incremented before every push, decremented at
        // every successful dequeue. It is the sleep predicate and the producer side of the Dekker pairs with NumSpinning
        // and with the idle mask.
        std::atomic<uint32> QueuedTasks{0};
        // Background-lane occupancy: incremented before the push, decremented only after a successful dequeue (it can
        // over-count, never underflow). Every scan reads it relaxed. Liveness never rests on it: the sleep predicate
        // and every Dekker pair read QueuedTasks, so a stale zero only defers the pickup to the next lap.
        std::atomic<uint32> BackgroundQueued{0};
        // Queued plus executing tasks; the backpressure signals read it.
        std::atomic<size_t> PendingTasks{0};
        // Fork tasks (ParallelFor helpers, Run's queue visits) published and not started, per lane, at most
        // kMaxUnstartedForkTasksPerWorker per worker. A fork reserves the counts before it publishes; a task gives its
        // count back when it starts or when its envelope is destroyed unrun. A bound, not a synchronization: relaxed.
        std::atomic<uint32> UnstartedForkTasks[kJobPriorityCount]{};
        // Caller-side notifies issued by batch publishes (one relaxed add per
        // bulk EnqueueWorkBatch, not per task).
        std::atomic<uint64> BatchNotifiesForTests{0};
        char PadBeforeAwaitedCount[GameEngine::kFalseSharingSeparation];
        // Awaited-lane occupancy, with BackgroundQueued's rules, in a group of its own: every scan and every spin
        // iteration reads it first, and every publish and dequeue writes the task counts.
        std::atomic<uint32> AwaitedQueued{0};
        char PadBeforeIdleCounts[GameEngine::kFalseSharingSeparation];
        // Idle counts: a worker writes them when it starts or stops a spin or a park; every publish reads them.
        // NumSpinning counts workers inside the TrySpinPoll poll loop only (the token is retired before any task
        // found is executed). NumParking counts workers in ParkWorker from before the idle bit is set to after it is
        // cleared: it is never below the idle mask's popcount, so a zero means no worker to claim.
        std::atomic<uint32> NumSpinning{0};
        std::atomic<uint32> NumParking{0};
        char PadAfterIdleCounts[GameEngine::kFalseSharingSeparation];
    };
    // One row per member: a member without a row is not checked.
    static_assert(Detail::GroupsNeverShareALine(
                      {
                          {offsetof(CounterLines, QueuedTasks), sizeof(CounterLines::QueuedTasks), 0},
                          {offsetof(CounterLines, BackgroundQueued), sizeof(CounterLines::BackgroundQueued), 0},
                          {offsetof(CounterLines, PendingTasks), sizeof(CounterLines::PendingTasks), 0},
                          {offsetof(CounterLines, UnstartedForkTasks), sizeof(CounterLines::UnstartedForkTasks), 0},
                          {offsetof(CounterLines, BatchNotifiesForTests), sizeof(CounterLines::BatchNotifiesForTests), 0},
                          {offsetof(CounterLines, AwaitedQueued), sizeof(CounterLines::AwaitedQueued), 1},
                          {offsetof(CounterLines, NumSpinning), sizeof(CounterLines::NumSpinning), 2},
                          {offsetof(CounterLines, NumParking), sizeof(CounterLines::NumParking), 2},
                      },
                      sizeof(CounterLines)),
                  "CounterLines: the task counts (0), the Awaited count (1) and the idle counts (2) never share a line");
    CounterLines m_Counters;
    // Reserves up to `wanted` of `unstarted`'s budget and returns the count
    // granted; the rest is given back at once. The add comes first, so
    // concurrent forks never grant more than `budget` between them.
    static size_t ReserveForkTasks(std::atomic<uint32>& unstarted, size_t wanted, size_t budget);
    // The pool task a Run publishes: a visit that drains one tagged queue.
    // WorkStealingThreadPool.cpp.
    class TaggedQueueVisit;
    // After a Run's publish, publishes a visit to `jobs` at `lane`: one the
    // lane's fork-task budget grants, else one only when no visit to `jobs`
    // is pending.
    void PublishTaggedQueueVisit(JobCounter::TaggedJobs& jobs, JobPriority lane);
    // Read-mostly: every scan, spin or publish reads them; construction, Shutdown and the test setters write them.
    // They sit between the padded m_Counters and m_Census, so no hot writer shares their line.
    std::atomic<bool> m_ShutdownRequested{false};
    // Constructed with 0 workers: submissions execute synchronously on the
    // submitting thread at the publish points (EnqueueTask, EnqueueTaskGlobal,
    // EnqueueWorkBatch); the queues and the wake protocol stay untouched.
    bool m_InlineMode = false;
    // Adaptive-spin configuration: read from the environment once at
    // construction (GE_JOB_SPIN_US, GE_JOB_MAX_SPINNERS), overridable with
    // SetSpinConfigForTest.
    std::atomic<uint32> m_SpinWindowUs{0};
    std::atomic<uint32> m_MaxSpinners{0};
    // Sleep backstop (liveness rescue) in microseconds; test-overridable so
    // the lost-wakeup stress test can prove the protocol without the rescue
    // masking a bug.
    std::atomic<uint32> m_SleepBackstopUs{1000};

    // Topology census (see JobSystemStatistics), in two contention classes:
    //  - Per-task counters (pushes and pops): one relaxed read-modify-write per
    //    task on paths that already pay two seq_cst read-modify-writes
    //    (m_Counters.QueuedTasks, m_Counters.PendingTasks) per task, so their shared-line traffic
    //    is bounded by task throughput. Producers and consumers contend on
    //    this line with each other, bounded the same way.
    //  - Idle-lap counters (StealMisses per failed scan, spin-poll iterations
    //    included; EmptyLoopsWithBacklog per lap that relaps on a true
    //    predicate; BackstopTimeouts per expired sleep): written by idle
    //    workers in every configuration. On the per-task line they would turn
    //    read-only idle polling into shared-line ping-pong against the
    //    producers' counters, so they get their own line. A full separation of
    //    char padding comes before, between and after the two classes, not
    //    alignas (MSVC's C4324 is an error here), so no line is shared at any
    //    address.
    struct TopologyCensusCounters
    {
        char PadHead[GameEngine::kFalseSharingSeparation];
        std::atomic<uint64> GlobalPushes{0};
        std::atomic<uint64> LocalPushes{0};
        std::atomic<uint64> AwaitedPushes{0};
        std::atomic<uint64> BackgroundPushes{0};
        std::atomic<uint64> GlobalPops{0};
        std::atomic<uint64> LocalPops{0};
        std::atomic<uint64> StealPops{0};
        std::atomic<uint64> AwaitedPops{0};
        std::atomic<uint64> BackgroundPops{0};
        char PadIdleLapSplit[GameEngine::kFalseSharingSeparation];
        std::atomic<uint64> StealMisses{0};
        std::atomic<uint64> EmptyLoopsWithBacklog{0};
        std::atomic<uint64> BackstopTimeouts{0};
        char PadTail[GameEngine::kFalseSharingSeparation];
    };
    // One row per member: a member without a row is not checked.
    static_assert(Detail::GroupsNeverShareALine(
                      {
                          {offsetof(TopologyCensusCounters, GlobalPushes), sizeof(TopologyCensusCounters::GlobalPushes), 0},
                          {offsetof(TopologyCensusCounters, LocalPushes), sizeof(TopologyCensusCounters::LocalPushes), 0},
                          {offsetof(TopologyCensusCounters, AwaitedPushes), sizeof(TopologyCensusCounters::AwaitedPushes), 0},
                          {offsetof(TopologyCensusCounters, BackgroundPushes), sizeof(TopologyCensusCounters::BackgroundPushes), 0},
                          {offsetof(TopologyCensusCounters, GlobalPops), sizeof(TopologyCensusCounters::GlobalPops), 0},
                          {offsetof(TopologyCensusCounters, LocalPops), sizeof(TopologyCensusCounters::LocalPops), 0},
                          {offsetof(TopologyCensusCounters, StealPops), sizeof(TopologyCensusCounters::StealPops), 0},
                          {offsetof(TopologyCensusCounters, AwaitedPops), sizeof(TopologyCensusCounters::AwaitedPops), 0},
                          {offsetof(TopologyCensusCounters, BackgroundPops), sizeof(TopologyCensusCounters::BackgroundPops), 0},
                          {offsetof(TopologyCensusCounters, StealMisses), sizeof(TopologyCensusCounters::StealMisses), 1},
                          {offsetof(TopologyCensusCounters, EmptyLoopsWithBacklog), sizeof(TopologyCensusCounters::EmptyLoopsWithBacklog), 1},
                          {offsetof(TopologyCensusCounters, BackstopTimeouts), sizeof(TopologyCensusCounters::BackstopTimeouts), 1},
                      },
                      sizeof(TopologyCensusCounters)),
                  "TopologyCensusCounters: the per-task counters (0) and the idle-lap counters (1) never share a line");
    TopologyCensusCounters m_Census;

#if GE_DEBUG_INSTRUMENTATION
    // Threads inside a counter-changing window that a concurrent observer
    // cannot attribute: a publisher between its counter increments (made
    // before the push) and its queue push, or a drain between a physical
    // dequeue and its AccountDequeuedTask. Shutdown's conservation tripwire
    // samples this on both sides of its counter reads and disarms when it is
    // nonzero. Debug-only: production publishes pay nothing.
    std::atomic<uint32> m_CounterRacersInFlight{0};
    // Monotonic count of bracket entries (publishers and drains fetch_add on
    // entry, after their in-flight increment). The in-flight count alone
    // misses a racer whose whole bracket (enter, change, exit) fits between
    // the tripwire's two racer samples, such as a publish plus its complete
    // drain; the tripwire also disarms when this epoch changed across its full
    // read sequence (the epoch is read outermost). Why no racer slips through:
    // any bracket whose changes reach the tripwire's counter snapshot either
    // (a) is still open at one of the two racer samples (a nonzero sample
    // disarms), or (b) entered after the first racer sample and exited before
    // the second, and then its epoch increment, sequenced after its in-flight
    // increment and before its exit, falls strictly between the tripwire's two
    // epoch reads (a changed epoch disarms). A bracket that closed entirely
    // before the first sample left either conserved counters (no false fire)
    // or a queued task that a later bracket consumes, which reduces to (a) or
    // (b) for that bracket.
    std::atomic<uint64> m_CounterBracketEpoch{0};
    std::atomic<uint64> m_WakeClaimsForTests{0};
    std::atomic<uint64> m_WakeConsumedForTests{0};
    std::atomic<uint64> m_WakeMergedForTests{0};
    std::atomic<uint64> m_WakeStaleForTests{0};
#endif

    // Dependency management
    UniquePtr<TaskDependencyGraph> m_DependencyGraph;

    // Process-wide task id source; ids are never reused across pools.
    static std::atomic<TaskId> m_NextTaskId;

    // The TaskData registry every TaskHandle of a task resolves through.
    mutable std::mutex m_TaskDataMutex;
    std::unordered_map<TaskId, TaskHandle::TaskDataRef> m_TaskDataRegistry;

    // Thread-local worker identity.
    static thread_local size_t s_CurrentWorkerId;
    static thread_local Worker* s_CurrentWorker;
    // The pool the current worker thread belongs to. PushTask's local-queue
    // path applies only to workers of this pool: routing a task into another
    // pool's local queue would corrupt both pools' queued-task counters.
    static thread_local WorkStealingThreadPool* s_CurrentPool;
    // The calling thread's innermost PublishCountScope sink; null outside one.
    static thread_local uint32* s_PublishSink;
    // Adds `count` published jobs to the calling thread's sink, if it has one.
    static void CountPublishedOnThisThread(size_t count);

    // The blocking threads and their FIFO; null in inline mode.
    UniquePtr<Detail::BlockingThreads> m_BlockingThreads;

    // The registered JobChannels, for the shutdown drain and the blocking
    // threads' demand. A channel registers at construction and unregisters at
    // destruction; its state can outlive it while its jobs run. A leaf mutex.
    mutable std::mutex m_ChannelsMutex;
    Vector<WeakPtr<Detail::JobChannelState>> m_Channels;

    friend class TaskHandle;
    // Writes its result through GetOrCreateTaskData.
    template <typename F>
    friend class Detail::SubmitGraphTask;
    // The channel builds handle envelopes and dispatches them; its state
    // registers itself and dispatches promoted jobs; the blocking threads run
    // envelopes through ExecuteTaskOptimized and read the shutdown gate.
    friend class JobChannel;
    friend class Detail::JobChannelState;
    friend class Detail::BlockingThreads;

    // Builds a handle envelope of type Envelope (a Detail::HandleTask) around
    // `fn`, constructed as Envelope(taskId, data, fn, extra...): a fresh task
    // id, its TaskData (the registry insert) and the handle that shares it.
    // Submit(F&&) and JobChannel::Submit publish what it builds.
    template <typename Envelope, typename F, typename... Extra>
    TaskHandle CreateHandleEnvelope(UniquePtr<Envelope>& envelope, F&& fn, Extra&&... extra);

    // Runs a job of `channel` that took a slot: on the blocking threads, or
    // at once on this thread in inline mode. An envelope the blocking threads
    // refuse because the shutdown gate is set is disposed of here
    // (DisposeEnvelopeAfterGate).
    void DispatchToBlockingThreads(UniquePtr<TaskBase> task, const char* channel);
    // A destroyed channel's queued Enqueue job: on to the blocking threads
    // with no channel slot and no new thread (the threads that run the
    // channel's running jobs take it), or, once the shutdown gate is set, run
    // on this thread.
    void HandOnToBlockingThreads(UniquePtr<TaskBase> task, const char* channel);
    // An envelope that will never reach a queue again: a handle or graph task
    // is cancelled (the arbitration CAS keeps it single-fire) and its registry
    // entry released; a bare task executes on this thread as `context`.
    void DisposeEnvelopeAfterGate(UniquePtr<TaskBase> task, JobContext context);
    // Cancels a handle or graph envelope that never ran and releases its
    // registry entry; the envelope is destroyed here.
    void CancelUnrunEnvelope(UniquePtr<TaskBase> task);
    void RegisterChannel(const SharedPtr<Detail::JobChannelState>& channel, uint32 maxRunning);
    void UnregisterChannel(const Detail::JobChannelState* channel, uint32 maxRunning);
    // Releases this thread's deferred registry cleanups for this pool now. A
    // blocking thread may idle for an hour between jobs; its share would
    // otherwise keep up to kPendingCleanupBatch - 1 TaskData (and their
    // results) alive that long.
    void FlushPendingCleanupOnThisThread();
    // Shutdown's release step: the drain, the graph retire and the registry
    // sweep.
    void ReleaseQueuedAndParkedWork();

    // Internal methods
    void DependencyAwareWorkerLoop(size_t workerId);
    void EnqueueTask(UniquePtr<TaskBase> task);
    void PushTask(UniquePtr<TaskBase> task);
    // The global lane for `priority`: m_Queues.Global, m_Queues.Awaited or m_Queues.Background.
    // Worker-local queues are not lanes: they carry graph tasks published on
    // workers, always Normal.
    ConcurrentQueue<UniquePtr<TaskBase>>& PriorityLane(JobPriority priority);
    // The occupancy counter that gates the polls of `priority`'s lane; null
    // for Normal, whose lane is polled ungated.
    std::atomic<uint32>* LaneOccupancy(JobPriority priority);
    // The census counter of pushes into `priority`'s lane.
    std::atomic<uint64>& LanePushes(JobPriority priority);
    // Push into the priority's lane (allocating tokenless enqueue). It fails
    // only when an allocation fails, and a silently dropped task can be a
    // caller-barrier release, so a failure aborts loudly.
    void PushGlobalQueue(UniquePtr<TaskBase> task, JobPriority priority);
    // Bulk push for EnqueueWorkBatch: the allocating enqueue_bulk (fresh
    // blocks on demand, recycled after warmup); try_enqueue_bulk is not used
    // because it fails whenever the preallocated blocks or index run out. A
    // failed enqueue_bulk returns before constructing any element, so the
    // failure path retries per task and aborts loudly only if a
    // single-element allocation fails too.
    void EnqueueBulkGlobal(UniquePtr<TaskBase>* tasks, size_t count, JobPriority priority);
    // One run of EnqueueWorkBatch: stages funcs[0, run) as envelopes in `staging` (their slabs in `slabs` when the
    // envelope is slab-pooled), counts the run, and pushes it with one EnqueueBulkGlobal. No wake.
    template <typename F>
    void PublishBulkRun(F* funcs, size_t run, UniquePtr<TaskBase>* staging, void** slabs, JobPriority priority);
    // Stages a batch above kStackBatch in the calling thread's scratch arena: calls publishRun once per run of at
    // most kMaxBulkRun tasks, with the run's first index, its size, and scratch room for its envelopes and slabs.
    void PublishInScratchRuns(
        size_t count,
        GameEngine::FunctionRef<void(size_t first, size_t run, UniquePtr<TaskBase>* staging, void** slabs)> publishRun);
    void NotifyAfterPublish();
    // Batch form of NotifyAfterPublish: one wake decision for a batch of
    // `publishedCount` tasks already counted and pushed. Wakes one idle worker
    // when min(publishedCount, queued - armed spinners) is above 0.
    void PublishAndWakeBatch(size_t publishedCount);
    // `wakeScanStart` is the worker id where the wake propagation starts its
    // scan of the idle mask.
    void AccountDequeuedTask(size_t wakeScanStart);
    // Claims one idle worker and signals it. The scan starts at worker
    // `firstWorker` (modulo the worker count) and wraps. Returns false when no
    // worker is idle.
    bool WakeIdleWorker(size_t firstWorker);
    // Sets the claimed worker's flag under its slot lock, then notifies it.
    void SignalClaimedWorker(size_t workerId);
    // Parks the calling worker until a waker signals it, shutdown, or the
    // backstop. Returns true when the backstop expired with no signal.
    bool ParkWorker(size_t workerId);
    // `lane` receives the class of the lane the task came from.
    bool TrySpinPoll(size_t workerId, UniquePtr<TaskBase>& task, JobPriority& lane);
    // The first arm of every scan, before the local queue. Gated on
    // m_Counters.AwaitedQueued like TryGetBackgroundTask; worker-only.
    bool TryGetAwaitedTask(size_t workerId, UniquePtr<TaskBase>& task);
    bool TryGetLocalTask(size_t workerId, UniquePtr<TaskBase>& task);
    // Worker-only (takes the worker's thread-affine GlobalConsumerToken); the
    // drain consumes the global lanes tokenless.
    bool TryGetGlobalTask(size_t workerId, UniquePtr<TaskBase>& task);
    bool TryStealTask(size_t thiefId, UniquePtr<TaskBase>& task);
    // The last arm of every scan. Gated on m_Counters.BackgroundQueued (one relaxed
    // load when the lane is empty, never a queue touch). No retry on a
    // spurious failure: consumers relap on the m_Counters.QueuedTasks predicate, the
    // same tolerance the global lane and the steal ring rely on. Worker-only,
    // like TryGetGlobalTask.
    bool TryGetBackgroundTask(size_t workerId, UniquePtr<TaskBase>& task);
    // Runs one dequeued envelope with its body inside a JobBodyScope of
    // `context`; completion callbacks and the graph release run after the
    // scope ends.
    void ExecuteTaskOptimized(UniquePtr<TaskBase> task, JobContext context);
    // Inline mode's execute-at-publish with a stack bound: at
    // kMaxInlineExecutionDepth nested executions on this thread a task is
    // deferred to a thread-local queue that the outermost frame drains
    // iteratively, so a job tree that spawns jobs from jobs cannot overflow
    // the caller's stack. DrainInlineTasksOnThisThread runs this pool's
    // deferred tasks for a synchronous join.
    void DrainInlineTasksOnThisThread();
    void ExecuteInlineBounded(UniquePtr<TaskBase> task, JobContext context);
    void CompleteGraphTask(TaskId taskId, bool success, const String& failureMessage,
                           std::exception_ptr failureException);
    void ProcessGraphActions(TaskGraphActions& actions, TaskId sourceTaskId,
                             const String& failureMessage, std::exception_ptr failureException);
    void CleanupTaskData(TaskId taskId);
    // Publish sequence for tasks that always use a global lane (Submit's
    // handle envelopes, bare EnqueueWork and batch envelopes, JobCounter
    // visits): counters, PushGlobalQueue into the priority's lane, wake
    // decision. Graph tasks go through EnqueueTask instead, whose PushTask
    // routes a publish on a worker into that worker's local queue (always
    // Normal).
    void EnqueueTaskGlobal(UniquePtr<TaskBase> task,
                           JobPriority priority = JobPriority::Normal);

    // Deferred registry cleanup: CleanupTaskData batches ids thread-locally,
    // and a thread drains its batch when this pool's share reaches this size.
    static constexpr size_t kPendingCleanupBatch = 32;
    // The registry references that one drain erases. Declare it before the
    // lock on m_TaskDataMutex: the references then drop after the unlock,
    // because a last reference runs user destructors (results, callbacks).
    struct TaskDataReleaseBatch
    {
        TaskHandle::TaskDataRef Entries[kPendingCleanupBatch];
        size_t Count = 0;

        void Add(TaskHandle::TaskDataRef data);
    };
    // Erases this pool's entries in this thread's batch. The caller holds
    // m_TaskDataMutex.
    void DrainPendingCleanupLocked(TaskDataReleaseBatch& released);

    // The registry's factory: the TaskData every handle of `taskId` shares.
    // Callers are the pool, TaskHandle's constructor and
    // Detail::SubmitGraphTask (its result).
    TaskHandle::TaskDataRef GetOrCreateTaskData(TaskId taskId);
    TaskId GenerateTaskId();

    // Registry lookup without creation (GetOrCreateTaskData stays the only
    // factory). Returns null for an unknown id and for a task whose deferred
    // registry cleanup already ran (both mean terminal or never submitted).
    TaskHandle::TaskDataRef TryGetTaskData(TaskId taskId) const;
    // Cancel core: CAS(Pending -> Cancelled) on the TaskData and, if it wins,
    // fire the single Cancelled event set (the graph cascade included) on the
    // calling thread, outside every lock.
    bool CancelPendingArbitrated(TaskId taskId, const TaskHandle::TaskDataRef& data);
    // Submit(F&&, dependencies) per-dependency linking: returns true when the
    // caller should register an edge to the dependency. A terminal, invalid or
    // foreign-pool handle returns false (no edge; an absent dependency is
    // terminal). A live graph dependency (the TaskData::GraphManaged brand,
    // never graph-node presence, which is false during cascade retirement)
    // gets a plain edge; a live dependency outside the graph (a plain
    // Submit(F&&) task) is bridged with a mirror node and the
    // TaskData::GraphLinked flag, whose terminal recheck resolves a dependency
    // that completed during the linking (the implementation carries the race
    // argument).
    bool LinkSubmitDependency(const TaskHandle& dependency);
    // Registers a graph envelope with the ids that LinkSubmitDependency
    // linked, and queues it when none is pending. The envelope's id is
    // graph-managed before the returned handle escapes.
    TaskHandle SubmitToGraph(UniquePtr<TaskBase> task, std::span<const TaskId> dependencies);
    // Completion bridge for a graph-linked task outside the graph (GraphLinked
    // set): releases or cascades the graph dependents of `taskId` through
    // MarkCompleted, suppressing the source's own terminal events, which
    // already fired on its own completion path; firing them twice would re-run
    // user callbacks.
    void CompleteLinkedDependency(TaskId taskId, bool success, const String& failureMessage,
                                  std::exception_ptr failureException);
    // Drain: dequeue everything reachable, cancel handle and graph tasks, then
    // execute bare tasks (two phases, because a bare task may Wait() on a
    // handle task queued behind it, and cancelling first makes that Wait()
    // return). Runs on the calling thread: Shutdown after the join, an
    // EnqueueWork caller that observed shutdown after its publish, or a Submit
    // caller's post-publish shutdown re-check.
    void DrainQueuesOnThisThread();
    // One drained envelope. PoolCounted: it came from a pool queue, so its
    // disposal releases m_Counters.PendingTasks; channel and blocking-FIFO envelopes
    // never entered the pool's counters. Context: the lane it was taken from,
    // or its channel, which phase 3 runs a bare one as.
    struct DrainedTask
    {
        UniquePtr<TaskBase> Task;
        bool PoolCounted;
        JobContext Context;
    };
    // The drain's channel arm: every registered channel's FIFO and the
    // blocking threads' FIFO, each emptied under its own mutex.
    void TakeChannelJobs(Vector<DrainedTask>& drained);
    // A Submit that passed the shutdown gate but observes shutdown after its
    // publish drains and arbitrates its own task directly. That covers the
    // queued envelope (drained and cancelled) and the graph-parked payload
    // (CancelPending retires it), so the returned handle is terminal and its
    // Wait() cannot strand.
    void AbandonSubmitDuringShutdown(TaskId taskId);

    WorkStealingThreadPool(const WorkStealingThreadPool&) = delete;
    WorkStealingThreadPool& operator=(const WorkStealingThreadPool&) = delete;
};

} // namespace JobSystem

#include "JobSystem/TaskEnvelopes.h"

namespace JobSystem
{

template <typename F, typename>
TaskHandle WorkStealingThreadPool::Submit(F&& task, JobPriority priority)
{
    // Gated on shutdown, like Submit(F&&, dependencies): the callable never
    // runs and the caller gets an invalid handle.
    if (IsShuttingDown())
    {
        return TaskHandle();
    }

    UniquePtr<Detail::SubmitTask<std::decay_t<F>>> envelope;
    TaskHandle handle = CreateHandleEnvelope(envelope, std::forward<F>(task));
    const TaskId taskId = handle.GetId();
    EnqueueTaskGlobal(std::move(envelope), priority);

    // The gate above can pass just before Shutdown() sets the flag and runs
    // to completion; the TaskData created above then lands after the registry
    // sweep's snapshot and the envelope in a queue nobody drains, which would
    // strand this handle's Wait(). Re-check after the publish, and on
    // detection drain and cancel this task. One load on the hot path.
    if (IsShuttingDown())
    {
        AbandonSubmitDuringShutdown(taskId);
    }

    return handle;
}

template <typename F, typename>
TaskHandle WorkStealingThreadPool::Submit(F&& task, std::span<const TaskHandle> dependencies)
{
    // The shutdown gate of every submit path. SubmitToGraph re-checks after
    // its publish, so a shutdown landing between here and there cannot
    // strand the returned handle.
    if (IsShuttingDown())
    {
        return TaskHandle();
    }

    auto graphTask = JobSystem::MakeUnique<Detail::SubmitGraphTask<std::decay_t<F>>>(
        this, GenerateTaskId(), std::forward<F>(task));
    Vector<TaskId> linkedDependencies;
    for (const TaskHandle& dependency : dependencies)
    {
        if (LinkSubmitDependency(dependency))
        {
            linkedDependencies.push_back(dependency.GetId());
        }
    }
    return SubmitToGraph(UniquePtr<TaskBase>(std::move(graphTask)), linkedDependencies);
}

template <typename Envelope, typename F, typename... Extra>
TaskHandle WorkStealingThreadPool::CreateHandleEnvelope(UniquePtr<Envelope>& envelope, F&& fn,
                                                        Extra&&... extra)
{
    const TaskId taskId = GenerateTaskId();

    // One TaskData for the task's whole lifetime: this registry insert is the
    // path's only m_TaskDataMutex acquisition; the returned handle adopts the
    // same reference, the envelope carries it, and the execute side reads it
    // from the envelope. The registry entry itself stays, because
    // CancelTask(id), the arbitration invariant (see CleanupTaskData) and
    // Shutdown's registry sweep all resolve through it.
    TaskHandle::TaskDataRef data = GetOrCreateTaskData(taskId);
    TaskHandle handle(taskId, this, data);
    envelope = MakeTaskEnvelope<Envelope>(taskId, std::move(data), std::forward<F>(fn),
                                          std::forward<Extra>(extra)...);
    return handle;
}

template <typename T>
TaskHandle WorkStealingThreadPool::CreateCompletedHandle(T result)
{
    const TaskId taskId = GenerateTaskId();
    TaskHandle handle(taskId, this); // creates the TaskData (the only factory)
    if (auto data = handle.GetTaskData())
    {
        // Payload before the status flip: TryGetResult is gated on Completed.
        data->Result = std::move(result);
        data->Status.store(TaskStatus::Completed, std::memory_order_release);
    }
    // No envelope ever exists for this id, so the registry entry has no
    // arbitration role (nothing can be dequeued for it). Release the entry
    // now; the returned handle keeps the TaskData alive through its own
    // shared_ptr. Keeping it would leak one registry entry per call until
    // pool shutdown.
    CleanupTaskData(taskId);
    return handle;
}

template <typename F, typename>
void WorkStealingThreadPool::EnqueueWork(F&& func, JobPriority priority)
{
    // Slab-pooled envelope: a freelist pop on this thread, a freelist push on
    // the executing worker.
    auto task = MakeTaskEnvelope<Detail::BareWorkTask<std::decay_t<F>>>(std::forward<F>(func));
    EnqueueTaskGlobal(std::move(task), priority);

    // Enqueue-versus-shutdown race: once shutdown is requested, the workers
    // may be past their final dequeue and Shutdown()'s post-join drain may
    // have swept the queues already, so the task just published could strand
    // and hang its caller's barrier. Re-check after the publish and drain on
    // detection: the publish completed before this load, so the task is
    // visible to some drain (this one, a live worker's dequeue, or Shutdown's)
    // and always executes. The argument is formal only when the load reads
    // true (program order makes the push visible to this thread's own drain);
    // a load reading false has no happens-before edge to Shutdown's drain
    // dequeues, and that case holds operationally: the push has retired long
    // before Shutdown() finishes joining the workers and starts its drain.
    if (IsShuttingDown())
    {
        DrainQueuesOnThisThread();
    }
}

template <typename F, typename>
void WorkStealingThreadPool::EnqueueWorkBatch(F* funcs, size_t count, JobPriority priority)
{
    if (count == 0)
    {
        return;
    }

    // Inline mode: batch work executes synchronously on the caller. Each
    // callable rides a slab-pooled envelope (the one threaded mode publishes)
    // so ExecuteInlineBounded can defer past the stack bound: a batch job
    // that batches more jobs must not recurse without bound.
    if (m_InlineMode)
    {
        for (size_t i = 0; i < count; ++i)
        {
            ExecuteInlineBounded(MakeTaskEnvelope<Detail::BareWorkTask<F>>(std::move(funcs[i])),
                                 ComputeContext(priority));
        }
        return;
    }

    // Up-front shutdown gate: after Shutdown() there are no workers and the
    // queues are drained or draining; batch work is fire-and-forget, so it
    // runs synchronously on the caller. That is the terminal behaviour
    // EnqueueWork reaches through publish and drain, without publishing
    // `count` envelopes only to drain them back.
    if (IsShuttingDown())
    {
        for (size_t i = 0; i < count; ++i)
        {
            RunBareInline(funcs[i], priority);
        }
        return;
    }

    // Small batches: the per-task publish loop. Each task becomes visible
    // (and consumable by a spinner) as soon as it is pushed, so publication
    // overlaps execution (see kBulkPublishThreshold). Each publish makes its
    // own bounded wake decision (NotifyAfterPublish, at most one notify per
    // task); the shutdown re-check below still applies.
    if (count < kBulkPublishThreshold)
    {
        for (size_t i = 0; i < count; ++i)
        {
            EnqueueTaskGlobal(MakeTaskEnvelope<Detail::BareWorkTask<F>>(std::move(funcs[i])), priority);
        }
        if (IsShuttingDown())
        {
            DrainQueuesOnThisThread();
        }
        return;
    }

    // One run per kMaxBulkRun tasks: the caller visits the lines that awake workers write (the slab freelist, the
    // task counts, the queue's index, the census) once per run. A batch of at most kStackBatch stages on the stack.
    if (count <= kStackBatch)
    {
        UniquePtr<TaskBase> staging[kStackBatch];
        void* slabs[kStackBatch];
        PublishBulkRun(funcs, count, staging, slabs, priority);
    }
    else
    {
        PublishInScratchRuns(count, [&](size_t first, size_t run, UniquePtr<TaskBase>* staging, void** slabs) {
            PublishBulkRun(funcs + first, run, staging, slabs, priority);
        });
    }

    // One bounded wake decision for the whole batch; worker-side wake
    // propagation carries the wake front to full width.
    PublishAndWakeBatch(count);

    // Post-publish shutdown re-check (as EnqueueWork): a batch that passed the
    // gate concurrently with Shutdown() may have published into queues whose
    // drain already ran. The publishes completed before this load, so every
    // envelope is reachable by some drain (this one, a live worker, or
    // Shutdown()'s): nothing is dropped and no caller barrier strands.
    if (IsShuttingDown())
    {
        DrainQueuesOnThisThread();
    }
}

template <typename F>
void WorkStealingThreadPool::PublishBulkRun(F* funcs, size_t run, UniquePtr<TaskBase>* staging,
                                            [[maybe_unused]] void** slabs, JobPriority priority)
{
    // Stage the envelopes before touching the counters: the over-count
    // window (the queued counter above the physical queue depth, the
    // benign direction) then spans only the bulk push, not the envelope
    // allocations. Slabs are acquired with one bulk freelist operation per
    // run; a dequeue per item doubles the caller's cost for a 200-task
    // burst.
    if constexpr (kIsSlabPooled<Detail::BareWorkTask<F>>)
    {
        // A throwing move constructor at staging index i would leak the
        // raw slabs [i+1, run), which no UniquePtr owns yet, and skew the
        // Live statistic for good. Every real chunk type is nothrow
        // movable; the requirement is stated here rather than guarded on a
        // path no caller can reach.
        static_assert(std::is_nothrow_move_constructible_v<F>,
                      "EnqueueWorkBatch bulk staging requires a nothrow-move closure");
        Detail::AcquireTaskSlabsBulk(slabs, run);
        for (size_t i = 0; i < run; ++i)
        {
            staging[i] = MakeTaskEnvelopeInSlab<Detail::BareWorkTask<F>>(slabs[i], std::move(funcs[i]));
        }
    }
    else
    {
        for (size_t i = 0; i < run; ++i)
        {
            staging[i] = MakeTaskEnvelope<Detail::BareWorkTask<F>>(std::move(funcs[i]));
        }
    }

#if GE_DEBUG_INSTRUMENTATION
    // Bracket enter: in-flight first, then the epoch (see m_CounterBracketEpoch).
    m_CounterRacersInFlight.fetch_add(1, std::memory_order_seq_cst);
    m_CounterBracketEpoch.fetch_add(1, std::memory_order_seq_cst);
#endif
    // The publish sequence, batch form: counters before the push (a fast
    // consumer decrements at dequeue; pushing first would let it underflow
    // the unsigned counter for a moment). The lane occupancy counter
    // follows the same before-push order.
    m_Counters.PendingTasks.fetch_add(run, std::memory_order_relaxed);
    m_Counters.QueuedTasks.fetch_add(static_cast<uint32>(run), std::memory_order_seq_cst); // (P1)
    if (std::atomic<uint32>* occupancy = LaneOccupancy(priority))
    {
        occupancy->fetch_add(static_cast<uint32>(run), std::memory_order_relaxed);
    }
    EnqueueBulkGlobal(staging, run, priority);
#if GE_DEBUG_INSTRUMENTATION
    m_CounterRacersInFlight.fetch_sub(1, std::memory_order_seq_cst);
#endif
}

template <typename F, typename>
void WorkStealingThreadPool::EnqueueWorkBatch(std::span<F> funcs, JobPriority priority)
{
    EnqueueWorkBatch(funcs.data(), funcs.size(), priority);
}

template <typename F, typename>
void WorkStealingThreadPool::Run(F&& fn, JobCounter& counter)
{
    // Everything that can throw or allocate comes before the count, because a
    // throw between Add and the publish would leave the counter over-counted
    // and its Wait hung: first the envelope (from the 128-byte slab pool when
    // the closure fits, else the heap, chosen at compile time), then the
    // tagged queue on the counter's first Run (m_JobsCreated lets later Runs
    // skip EnsureJobs and its lock).
    auto job = MakeTaskEnvelope<JobCounter::Job<std::decay_t<F>>>(
        &counter, std::forward<F>(fn));
    if (!counter.m_JobsCreated.load(std::memory_order_acquire))
    {
        counter.EnsureJobs();
    }

    counter.Add(1); // before the publish, and immediately before its critical section
#if GE_DEBUG_INSTRUMENTATION
    // Counted until the envelope runs: the frame advance asserts that the
    // frame waited every Run it published.
    if (job->FrameJoined)
        GameEngine::Memory::Detail::AddOutstandingJoinedRun();
#endif

    JobCounter::TaggedJobsRef jobs;
    bool enqueued;
    {
        // One critical section holds the occupancy increment, the tagged
        // enqueue and the parked-waiter notify. The notify shares the mutex
        // with waiter registration and predicate evaluation, so either the
        // waiter's predicate sees the new occupancy or the waiter is already
        // blocked when the notify lands. Holding the mutex across the enqueue
        // also pins the counter's lifetime: a consumer completing this
        // envelope serializes its zero-publishing Decrement behind this lock,
        // so no waiter can return, and no owner can destroy the counter, until
        // this block exits. After this block, Run touches only its own
        // reference to the TaggedJobs, never `counter`.
        std::lock_guard<std::mutex> lock(counter.m_Mutex);
        jobs = counter.m_Jobs;
        // Occupancy before the enqueue (see TaggedJobs::JobCount): a consumer
        // that observes the count retries its dequeue, so over-counting is
        // the safe direction.
        jobs->JobCount.fetch_add(1, std::memory_order_release);
        enqueued = jobs->Queue.enqueue(jobs->Producer, job.get());
        if (enqueued)
        {
            // The queue owns the envelope now. release() only nulls the local
            // pointer, which is safe even if a consumer already freed it.
            job.release();
            if (counter.m_Waiters > 0)
            {
                counter.m_Cv.notify_all();
            }
        }
        else
        {
            // An allocation failure (the allocating enqueue's only failure
            // mode): the envelope never became visible, so its occupancy is
            // taken back and visits and waiters can still prove emptiness.
            jobs->JobCount.fetch_sub(1, std::memory_order_release);
        }
    }

    if (!enqueued)
    {
        // Never drop work, never hang a waiter: execute the envelope inline on
        // the submitting thread. Its scope guard decrements the counter, so
        // the count is conserved, and no visit is published for it. It runs
        // as the caller's own class.
        JobBodyScope body(CallerContext());
        job->Execute();
        return;
    }

    // Outside the critical section: a visit reaches the queue object, never
    // the counter, so it may outlive a stack-owned counter whose envelopes a
    // participating waiter ran, and it never keeps the queue from being
    // recycled. It publishes at the forking body's class, so a Background
    // body's fork stays Background.
    PublishTaggedQueueVisit(*jobs, CallerContext().Class);
}

template <typename Body>
bool ParallelFor(WorkStealingThreadPool* pool, size_t count, Body&& body, const ParallelForOptions& options)
{
    using BodyType = std::remove_reference_t<Body>;
    if constexpr (std::is_invocable_v<BodyType&, size_t>)
    {
        using Result = std::invoke_result_t<BodyType&, size_t>;
        static_assert(std::is_void_v<Result> || std::is_same_v<Result, bool>,
                      "ParallelFor: the body returns void or bool (false stops the run)");
    }
    else
    {
        static_assert(std::is_invocable_v<BodyType&, size_t>,
                      "ParallelFor: the body is callable as body(size_t unit); a chunk body "
                      "(size_t begin, size_t end) takes the chunked overload in "
                      "JobSystem/ParallelAlgorithms.h with a minBatchSize");
    }
    return WorkStealingThreadPool::RunParallelFor(
        pool, &WorkStealingThreadPool::InvokeParallelForUnit<BodyType>,
        const_cast<void*>(static_cast<const void*>(std::addressof(body))), count, options);
}

template <typename F>
void WorkStealingThreadPool::RunBareInline(F& func, JobPriority priority)
{
#if GE_DEBUG_INSTRUMENTATION
    // A queued body runs without the frame-joined bit (ExecuteTaskOptimized).
    const GameEngine::Memory::Detail::FrameJoinedScope notFrameJoined(false);
#endif
    try
    {
        JobBodyScope body(ComputeContext(priority));
        func();
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("Bare batch task execution failed: {}", e.what());
    }
    catch (...)
    {
        Logger::Log::Error("Bare batch task threw a non-standard exception");
    }
}

} // namespace JobSystem
