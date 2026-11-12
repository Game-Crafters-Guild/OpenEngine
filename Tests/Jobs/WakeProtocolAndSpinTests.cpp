// Slice-1 wake-protocol + adaptive-spin coverage.
//
// A lost wakeup manifests as a backstop-timeout rescue: ~1ms with a 1ms system
// timer period, ~15.6ms without timeBeginPeriod(1). Every latency assertion
// below therefore uses 900µs as the "protocol failed" line, and the bench
// fixture logs the actual timer resolution so timeout-derived numbers can be
// interpreted.

#include "JobSystem/ParallelAlgorithms.h"
#include "JobSystem/TaskSlabPool.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "HeldWorkers.h"
#include "LatencyBudget.h"
#include "TestPlatform.h"
#if !GE_TEST_HAS_WIN_TIMERS
#include <sys/resource.h>
#endif

namespace GameEngine::Tests {

namespace {

using SteadyClock = std::chrono::steady_clock;

double ElapsedUs(SteadyClock::time_point t0, SteadyClock::time_point t1)
{
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

// Precise sub-millisecond gap between forks. sleep_for cannot hit microsecond
// gaps on Windows; the stress tests need gaps that straddle the spin-window
// expiry, so burn the gap on the submitting thread.
void BusyWaitUs(int us)
{
    if (us <= 0)
        return;
    const auto deadline = SteadyClock::now() + std::chrono::microseconds(us);
    while (SteadyClock::now() < deadline)
    {
        YieldProcessor();
    }
}

double Percentile(const std::vector<double>& sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    const double idx = p * static_cast<double>(sorted.size() - 1);
    return sorted[static_cast<size_t>(idx + 0.5)];
}

// Timeout-based numbers are meaningless without the effective timer period:
// a backstop rescue costs ~1ms at wPeriod=1 but ~15.6ms at the default.
void LogTimerResolution()
{
#if GE_TEST_HAS_WIN_TIMERS
    using NtQueryTimerResolutionFn = LONG(NTAPI*)(PULONG, PULONG, PULONG);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
    {
        if (auto fn = reinterpret_cast<NtQueryTimerResolutionFn>(
                GetProcAddress(ntdll, "NtQueryTimerResolution")))
        {
            ULONG minRes = 0, maxRes = 0, curRes = 0;
            if (fn(&minRes, &maxRes, &curRes) == 0)
            {
                std::printf("[timer] NtQueryTimerResolution (100ns units -> ms): "
                            "min=%.3f max=%.3f CURRENT=%.3f\n",
                            minRes / 10000.0, maxRes / 10000.0, curRes / 10000.0);
            }
        }
    }
    if (HMODULE winmm = LoadLibraryW(L"winmm.dll"))
    {
        struct TimeCapsMirror
        {
            UINT PeriodMin;
            UINT PeriodMax;
        };
        using TimeGetDevCapsFn = UINT(WINAPI*)(TimeCapsMirror*, UINT);
        if (auto fn = reinterpret_cast<TimeGetDevCapsFn>(GetProcAddress(winmm, "timeGetDevCaps")))
        {
            TimeCapsMirror caps{};
            if (fn(&caps, sizeof(caps)) == 0)
            {
                std::printf("[timer] timeGetDevCaps: wPeriodMin=%ums wPeriodMax=%ums\n",
                            caps.PeriodMin, caps.PeriodMax);
            }
        }
        FreeLibrary(winmm);
    }
#else
    std::printf("[timer] host timer-resolution diagnostics unavailable on this platform\n");
#endif
}

double MeasureForkUs(JobSystem::WorkStealingThreadPool& pool,
                     std::function<void()>* tasks, uint32_t count)
{
    const auto t0 = SteadyClock::now();
    JobSystem::ParallelFor(&pool, count, [&](size_t task) { tasks[task](); });
    return ElapsedUs(t0, SteadyClock::now());
}

// The flood and window tests raise the backstop to 60 s: a lost wake is then a
// hang that a 2 s watchdog reports, not a slow pass.
constexpr uint32_t kRaisedBackstopUs = 60'000'000;
constexpr std::chrono::milliseconds kWatchdog{2000};

// The pool's wake state, for a watchdog message.
std::string WakeState(const JobSystem::WorkStealingThreadPool& pool)
{
    std::string state = "idle bits " + std::to_string(pool.GetIdleCountForTests()) + ", spinning " +
                        std::to_string(pool.GetSpinningCountForTests()) + ", published " +
                        std::to_string(pool.GetPublishedJobCount());
#if GE_DEBUG_INSTRUMENTATION
    const JobSystem::WorkStealingThreadPool::WakeCountsForTests counts = pool.GetWakeCountsForTests();
    state += ", claims " + std::to_string(counts.Claims) + ", consumed " + std::to_string(counts.Consumed) +
             ", merged " + std::to_string(counts.Merged) + ", stale " + std::to_string(counts.StaleConsumed) +
             ", flags set " + std::to_string(counts.FlagsSet);
#endif
    return state;
}

// Polls the fork in flight (start time, 0 between forks). A fork older than
// the watchdog is a stalled fork: the pool's wake state is kept for the failure.
void WatchForks(const JobSystem::WorkStealingThreadPool& pool, const std::atomic<int64_t>& forkStartNs,
                const std::atomic<bool>& stop, std::string& stuckState)
{
    constexpr int64_t kWatchdogNs = std::chrono::duration_cast<std::chrono::nanoseconds>(kWatchdog).count();
    while (!stop.load() && stuckState.empty())
    {
        const int64_t startNs = forkStartNs.load();
        if (startNs != 0 && SteadyNowNs() - startNs > kWatchdogNs)
            stuckState = "a fork in flight for over " + std::to_string(kWatchdog.count()) + " ms; " + WakeState(pool);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Shared body for the lost-wakeup stress: 100k single-offload forks with
// randomized 0-200µs inter-fork gaps. The gap range straddles the 100µs spin
// expiry, so forks land in every phase of the spinner lifecycle — including
// the §7 Hazard-A window (producer-skip racing a retiring spinner).
void RunLostWakeupStress(uint32_t spinWindowUs, uint32_t maxSpinners)
{
    JobSystem::WorkStealingThreadPool pool(4);
    pool.SetSpinConfigForTest(spinWindowUs, maxSpinners);

    // The stress backstop takes the 1 ms rescue out of the fork times. The
    // caller runs a unit that no helper claims, so a lost wake does not stall a fork.
    pool.SetSleepBackstopForTest(kStressBackstopUs);
    const LatencyBudget budget = ReadLatencyBudget();

    constexpr int kIterations = 100000;
    std::mt19937 rng(0x5EED);
    std::uniform_int_distribution<int> gapUs(0, 200);

    // The offloaded task records when it actually ran, splitting each outlier
    // into submit->task-ran (pool wake protocol) vs task-ran->caller-woke
    // (caller barrier / scheduler). A lost wakeup shows up as a submit->ran
    // gap quantized at the backstop boundary (~1-2ms at wPeriod=1); scheduler
    // preemption scatters continuously and hits both phases.
    std::atomic<SteadyClock::time_point::rep> taskRanAt{0};
    std::function<void()> tasks[2];
    tasks[0] = [] {};
    tasks[1] = [&taskRanAt] {
        taskRanAt.store(SteadyClock::now().time_since_epoch().count(),
                        std::memory_order_relaxed);
    };

    struct Outlier
    {
        double TotalUs;
        double SubmitToRanUs;
    };
    std::vector<Outlier> outliers; // every sample >900µs, for scheduler-noise triage
    int over100ms = 0;
    double worstUs = 0.0;
    std::atomic<int64_t> forkStartNs{0};
    std::atomic<bool> stopWatchdog{false};
    std::string stuckState;
    std::thread watchdog(WatchForks, std::cref(pool), std::cref(forkStartNs), std::cref(stopWatchdog),
                         std::ref(stuckState));
    for (int i = 0; i < kIterations; ++i)
    {
        const auto t0 = SteadyClock::now();
        forkStartNs.store(SteadyNowNs());
        JobSystem::ParallelFor(&pool, 2, [&](size_t task) { tasks[task](); });
        forkStartNs.store(0);
        const double us = ElapsedUs(t0, SteadyClock::now());
        worstUs = std::max(worstUs, us);
        if (us > 900.0)
        {
            const auto ranAt = SteadyClock::time_point(
                SteadyClock::duration(taskRanAt.load(std::memory_order_relaxed)));
            outliers.push_back({us, ElapsedUs(t0, ranAt)});
            if (us > 100000.0)
            {
                ++over100ms;
            }
        }
        BusyWaitUs(gapUs(rng));
    }
    stopWatchdog.store(true);
    watchdog.join();
    EXPECT_TRUE(stuckState.empty()) << "a stalled fork: " << stuckState;

    std::printf("[LostWakeupStress spin=%uus maxSpinners=%u] iters=%d worst=%.1fus "
                "over900us=%zu over100ms=%d\n",
                spinWindowUs, maxSpinners, kIterations, worstUs, outliers.size(), over100ms);
    for (const Outlier& o : outliers)
    {
        std::printf("[LostWakeupStress]   outlier: total=%.1fus submit->task-ran=%.1fus "
                    "task-ran->caller-woke=%.1fus\n",
                    o.TotalUs, o.SubmitToRanUs, o.TotalUs - o.SubmitToRanUs);
    }

    // A fork at the backstop line is a stall of the fork path: a started helper or
    // the caller's join. Scheduler tails on a loaded machine stay under
    // kLostWakeupLineUs, so only the latency line reads the machine load.
    EXPECT_LT(worstUs, kLostWakeupLineUs) << "a fork sample at the backstop line is a stalled fork";
    ExpectWithinBudget(worstUs, kStressLatencyLineUs, budget, "LostWakeupStress worst fork");
}

} // namespace

TEST(WakeProtocolTest, LostWakeupStress)
{
    LogTimerResolution();
    RunLostWakeupStress(100, 2);
}

// With the spin path disabled, every fork uses the park protocol alone: the
// waker's claim and flag write under the slot lock against the worker's idle
// bit, queued-count read and flag check under the same lock (§7 Hazard B).
TEST(WakeProtocolTest, LostWakeupStressNoSpinners)
{
    RunLostWakeupStress(0, 0);
}

namespace {

using JobSystem::JobPriority;
using JobSystem::WorkStealingThreadPool;

constexpr uint32_t kShippingSpinWindowUs = 100;
constexpr uint32_t kShippingMaxSpinners = 2;
constexpr uint32_t kTimeoutRaceBackstopUs = 20;
constexpr uint32_t kFloodRounds = 300;
constexpr uint32_t kStallRounds = 100;
constexpr size_t kRunSizes[] = {1, 7, 8, 9, 17, 64, 65};
constexpr size_t kLargestRun = 65;
constexpr int kMaxGapUs = 200;

// At quiesce every claim wrote one flag: a wake consumed it, it merged into a
// flag that was already set, or it is still set.
void ExpectWakeCountsBalance(const WorkStealingThreadPool& pool)
{
#if GE_DEBUG_INSTRUMENTATION
    const bool balanced = WaitUntil(
        [&] {
            const WorkStealingThreadPool::WakeCountsForTests counts = pool.GetWakeCountsForTests();
            return counts.Claims == counts.Consumed + counts.Merged + counts.FlagsSet;
        },
        kWatchdog);
    EXPECT_TRUE(balanced) << "the wake counts do not balance: " << WakeState(pool);
#else
    (void)pool;
#endif
}

// A flood job is larger than a task slab, so its worker frees it to the heap.
// A worker that frees a slab keeps a freelist block for the rest of the
// process, and 70 such workers starve TaskSlabPoolTest and JobCounterTest.
struct Tick
{
    std::atomic<uint64_t>* Ran = nullptr;
    char Pad[JobSystem::kTaskSlabSize] = {};

    void operator()() const { Ran->fetch_add(1, std::memory_order_relaxed); }
};
static_assert(sizeof(Tick) > JobSystem::kTaskSlabSize);

struct FloodShape
{
    uint32_t Workers = 0;
    uint32_t SpinWindowUs = 0;
    uint32_t MaxSpinners = 0;
    uint32_t Producers = 1;
    uint32_t BackstopUs = kRaisedBackstopUs;
};

std::string Describe(const FloodShape& shape)
{
    return std::to_string(shape.Workers) + " workers, spin " + std::to_string(shape.SpinWindowUs) + " us x " +
           std::to_string(shape.MaxSpinners) + ", " + std::to_string(shape.Producers) + " producers, backstop " +
           std::to_string(shape.BackstopUs) + " us";
}

// The floods run on one pool per width, and each pool runs every shape in
// turn: a pool per shape starts enough threads to fill the task slab
// freelist's per-thread blocks. Only the run flood adds 70 workers (a second
// mask word).
constexpr uint32_t kFloodWidths[] = {2, 4, 16};
constexpr uint32_t kSecondMaskWordWidth = 70;

// The shapes one pool runs: the shipping spin and no spin, from one and from
// four producers.
std::vector<FloodShape> FloodShapes(uint32_t workers, uint32_t backstopUs)
{
    std::vector<FloodShape> shapes;
    for (const bool spin : {true, false})
        for (const uint32_t producers : {1u, 4u})
            shapes.push_back({workers, spin ? kShippingSpinWindowUs : 0u, spin ? kShippingMaxSpinners : 0u, producers,
                              backstopUs});
    return shapes;
}

void ConfigureFlood(WorkStealingThreadPool& pool, const FloodShape& shape)
{
    pool.SetSpinConfigForTest(shape.SpinWindowUs, shape.MaxSpinners);
    pool.SetSleepBackstopForTest(shape.BackstopUs);
}

uint64_t ClaimCount(const WorkStealingThreadPool& pool)
{
#if GE_DEBUG_INSTRUMENTATION
    return pool.GetWakeCountsForTests().Claims;
#else
    (void)pool;
    return 0;
#endif
}

// A flood that claims no idle worker never parks one, so its pass says nothing.
void ExpectClaimsSince(const WorkStealingThreadPool& pool, uint64_t claimsBefore)
{
#if GE_DEBUG_INSTRUMENTATION
    EXPECT_GT(ClaimCount(pool), claimsBefore) << "the flood parked no worker: " << WakeState(pool);
#else
    (void)pool;
    (void)claimsBefore;
#endif
}

size_t RunSize(uint32_t roundIndex, uint32_t producer)
{
    return kRunSizes[(roundIndex + producer) % std::size(kRunSizes)];
}

// One producer's run of one round, after a 0-200 us gap. Every third run goes
// to the Background lane.
void PublishRun(WorkStealingThreadPool& pool, std::atomic<uint64_t>& ran, uint32_t roundIndex, uint32_t producer,
                std::mt19937& rng)
{
    std::uniform_int_distribution<int> gapUs(0, kMaxGapUs);
    BusyWaitUs(gapUs(rng));
    const JobPriority lane = (roundIndex + producer) % 3 == 2 ? JobPriority::Background : JobPriority::Normal;
    const size_t size = RunSize(roundIndex, producer);
    if (size == 1)
    {
        pool.EnqueueWork(Tick{&ran}, lane);
        return;
    }
    Tick run[kLargestRun];
    for (size_t i = 0; i < size; ++i)
        run[i] = Tick{&ran};
    pool.EnqueueWorkBatch(run, size, lane);
}

// A helper producer publishes its run of each round when the round opens.
void ProduceRounds(WorkStealingThreadPool& pool, std::atomic<uint64_t>& ran, const std::atomic<int64_t>& openRound,
                   const std::atomic<bool>& stop, uint32_t producer)
{
    std::mt19937 rng(0xF100D + producer);
    for (int64_t roundIndex = 0;; ++roundIndex)
    {
        while (openRound.load() < roundIndex && !stop.load())
            std::this_thread::yield();
        if (stop.load())
            return;
        PublishRun(pool, ran, static_cast<uint32_t>(roundIndex), producer, rng);
    }
}

// External producers publish runs of 1 to 65 tasks. Each round must drain
// inside the watchdog before the next round opens, so no later publish can
// rescue a lost wake. `ran` outlives the pool, so a task a failed round left
// queued counts into live memory.
void RunExternalRunFlood(WorkStealingThreadPool& pool, std::atomic<uint64_t>& ran, const FloodShape& shape,
                         uint32_t rounds)
{
    SCOPED_TRACE(Describe(shape));
    ConfigureFlood(pool, shape);
    const uint64_t claimsBefore = ClaimCount(pool);

    std::atomic<int64_t> openRound{-1};
    std::atomic<bool> stop{false};
    std::vector<std::thread> producers;
    for (uint32_t producer = 1; producer < shape.Producers; ++producer)
        producers.emplace_back(ProduceRounds, std::ref(pool), std::ref(ran), std::cref(openRound), std::cref(stop),
                               producer);

    std::mt19937 rng(0xF100D);
    uint64_t expected = ran.load();
    std::string stuckState;
    for (uint32_t roundIndex = 0; roundIndex < rounds && stuckState.empty(); ++roundIndex)
    {
        for (uint32_t producer = 0; producer < shape.Producers; ++producer)
            expected += RunSize(roundIndex, producer);
        openRound.store(roundIndex);
        PublishRun(pool, ran, roundIndex, 0, rng);
        if (!WaitUntil([&] { return ran.load() == expected; }, kWatchdog))
            stuckState = "round " + std::to_string(roundIndex) + " ran " + std::to_string(ran.load()) + " of " +
                         std::to_string(expected) + "; " + WakeState(pool);
    }
    stop.store(true);
    for (std::thread& producer : producers)
        producer.join();
    EXPECT_TRUE(stuckState.empty()) << "a lost wake: " << stuckState;
    ExpectWakeCountsBalance(pool);
    ExpectClaimsSince(pool, claimsBefore);
}

// Runs the run flood with every shape on one pool per width.
void RunRunFloods(std::span<const uint32_t> widths, uint32_t backstopUs, uint32_t rounds)
{
    for (const uint32_t workers : widths)
    {
        std::atomic<uint64_t> ran{0};
        WorkStealingThreadPool pool(workers);
        for (const FloodShape& shape : FloodShapes(workers, backstopUs))
            RunExternalRunFlood(pool, ran, shape, rounds);
    }
}

// A step shaped like a physics step: the external thread publishes a stage of
// 17, the last job of a stage publishes one single, and the single publishes
// the next stage. Worker publishes and the dequeue-side wake carry the step.
constexpr uint32_t kStageWidth = 17;
constexpr uint32_t kStagesPerStep = 4;

struct StageChain
{
    WorkStealingThreadPool* Pool = nullptr;
    std::atomic<uint64_t>* Ran = nullptr;
    std::atomic<uint32_t> StagesLeft{0};
    std::atomic<uint32_t> JobsLeft{0};
};

void PublishStage(StageChain* chain);

// Each job counts itself last, so a drained count means no job reads the chain.
struct StageSingle
{
    StageChain* Chain = nullptr;

    void operator()() const
    {
        PublishStage(Chain);
        Chain->Ran->fetch_add(1);
    }
};

struct StageJob
{
    StageChain* Chain = nullptr;

    void operator()() const
    {
        if (Chain->JobsLeft.fetch_sub(1) == 1 && Chain->StagesLeft.fetch_sub(1) > 1)
            Chain->Pool->EnqueueWork(StageSingle{Chain});
        Chain->Ran->fetch_add(1);
    }
};

void PublishStage(StageChain* chain)
{
    chain->JobsLeft.store(kStageWidth);
    StageJob stage[kStageWidth];
    for (StageJob& job : stage)
        job = StageJob{chain};
    chain->Pool->EnqueueWorkBatch(stage, kStageWidth);
}

// The chain outlives the pool, so a job a failed step left queued reads live memory.
void RunStageChainFlood(StageChain& chain, const FloodShape& shape, uint32_t steps)
{
    SCOPED_TRACE(Describe(shape));
    WorkStealingThreadPool& pool = *chain.Pool;
    std::atomic<uint64_t>& ran = *chain.Ran;
    ConfigureFlood(pool, shape);
    const uint64_t claimsBefore = ClaimCount(pool);

    std::mt19937 rng(0xC4A1);
    std::uniform_int_distribution<int> gapUs(0, kMaxGapUs);
    uint64_t expected = ran.load();
    std::string stuckState;
    for (uint32_t step = 0; step < steps && stuckState.empty(); ++step)
    {
        expected += kStageWidth * kStagesPerStep + kStagesPerStep - 1;
        chain.StagesLeft.store(kStagesPerStep);
        PublishStage(&chain);
        if (!WaitUntil([&] { return ran.load() == expected; }, kWatchdog))
            stuckState = "step " + std::to_string(step) + " ran " + std::to_string(ran.load()) + " of " +
                         std::to_string(expected) + "; " + WakeState(pool);
        BusyWaitUs(gapUs(rng));
    }
    EXPECT_TRUE(stuckState.empty()) << "a lost wake: " << stuckState;
    ExpectWakeCountsBalance(pool);
    ExpectClaimsSince(pool, claimsBefore);
}

#if GE_DEBUG_INSTRUMENTATION
using WakePoint = WorkStealingThreadPool::WakePoint;

constexpr int kNoPoint = -1;
constexpr int kStallUs = 200;
constexpr std::chrono::milliseconds kHeldWakerStall{50};

// The forced interleavings delay one wake point: a yield on even visits and a
// 200 us busy stall on odd visits. The stall takes no lock, because at
// BeforeBlock the worker holds its slot lock.
std::atomic<int> g_StallPoint{kNoPoint};
std::atomic<uint32_t> g_StallVisits{0};

void StallAtOnePoint(WakePoint point, size_t)
{
    if (static_cast<int>(point) != g_StallPoint.load(std::memory_order_relaxed))
        return;
    if (g_StallVisits.fetch_add(1, std::memory_order_relaxed) % 2 == 0)
        std::this_thread::yield();
    else
        BusyWaitUs(kStallUs);
}

// Holds one visit of one wake point. A worker point holds until the release.
// BeforeBlock holds under the slot lock until a waker claims the worker, then
// until the release or 50 ms, so a waker that writes the flag under the lock
// waits for the hold. AfterClaim (on the waker) stalls 50 ms.
std::atomic<int> g_HoldPoint{kNoPoint};
std::atomic<bool> g_Held{false};
std::atomic<bool> g_Release{false};
std::atomic<const WorkStealingThreadPool*> g_HeldPool{nullptr};

void HoldOneVisit(WakePoint point, size_t)
{
    int armed = static_cast<int>(point);
    if (!g_HoldPoint.compare_exchange_strong(armed, kNoPoint))
        return;
    g_Held.store(true);
    if (point == WakePoint::AfterClaim)
    {
        std::this_thread::sleep_for(kHeldWakerStall);
        return;
    }
    if (point == WakePoint::BeforeBlock)
    {
        while (g_HeldPool.load()->GetIdleCountForTests() != 0 && !g_Release.load())
            std::this_thread::yield();
        const SteadyClock::time_point until = SteadyClock::now() + kHeldWakerStall;
        while (!g_Release.load() && SteadyClock::now() < until)
            std::this_thread::yield();
        return;
    }
    while (!g_Release.load())
        std::this_thread::yield();
}

// Opens every hold, so a failed case holds no worker of the next case.
void OpenEveryHold()
{
    g_StallPoint.store(kNoPoint);
    g_HoldPoint.store(kNoPoint);
    g_Release.store(true);
}

// One worker, no spin, the raised backstop. The worker (at AfterClaim, the
// waker) is held once at `point` while the test publishes one task, which
// must run inside the watchdog.
void RunHeldWindow(WakePoint point)
{
    std::atomic<uint64_t> ran{0};
    WorkStealingThreadPool pool(1);
    pool.SetSpinConfigForTest(0, 0);
    pool.SetSleepBackstopForTest(kRaisedBackstopUs);
    g_Held.store(false);
    g_Release.store(false);
    g_HeldPool.store(&pool);
    ScopedWakeHook hook(&HoldOneVisit, &OpenEveryHold);
    ASSERT_TRUE(WaitUntil([&] { return pool.GetIdleCountForTests() == 1; }, kWatchdog))
        << "the worker never parked: " << WakeState(pool);

    g_HoldPoint.store(static_cast<int>(point));
    uint64_t expected = 1;
    if (point != WakePoint::AfterClaim)
    {
        // One task cycles the parked worker, and its next park meets the hold.
        pool.EnqueueWork(Tick{&ran});
        ASSERT_TRUE(WaitUntil([&] { return g_Held.load(); }, kWatchdog))
            << "the worker never reached the held point: " << WakeState(pool);
        expected = 2;
    }
    pool.EnqueueWork(Tick{&ran});
    g_Release.store(true);
    EXPECT_TRUE(g_Held.load()) << "the hold never ran";
    EXPECT_TRUE(WaitUntil([&] { return ran.load() == expected; }, kWatchdog))
        << "a lost wake: ran " << ran.load() << " of " << expected << "; " << WakeState(pool);
    ExpectWakeCountsBalance(pool);
}
#endif // GE_DEBUG_INSTRUMENTATION

} // namespace

TEST(WakeProtocolTest, RunFloodsLoseNoTaskOnAnyPoolWidth)
{
    constexpr uint32_t kRunFloodWidths[] = {2, 4, 16, kSecondMaskWordWidth};
    RunRunFloods(kRunFloodWidths, kRaisedBackstopUs, kFloodRounds);
}

TEST(WakeProtocolTest, WorkerPublishedStageChainsLoseNoTask)
{
    for (const uint32_t workers : kFloodWidths)
    {
        std::atomic<uint64_t> ran{0};
        StageChain chain;
        WorkStealingThreadPool pool(workers);
        chain.Pool = &pool;
        chain.Ran = &ran;
        for (const FloodShape& shape : FloodShapes(workers, kRaisedBackstopUs))
            if (shape.Producers == 1)
                RunStageChainFlood(chain, shape, kFloodRounds);
    }
}

// A 20 us backstop races timeouts against claims (at least 1 ms on Windows,
// where the wait has millisecond steps): a worker's idle-bit clear finds its
// bit claimed, and the flag it leaves makes its next park return.
TEST(WakeProtocolTest, TimeoutRaceAtTwentyMicrosecondsLosesNoTask)
{
    RunRunFloods(kFloodWidths, kTimeoutRaceBackstopUs, kFloodRounds);
}

// Shutdown notifies each parked worker: with the raised backstop, only that
// notify ends the worker's wait inside the watchdog.
TEST(WakeProtocolTest, ShutdownWakesAParkedWorker)
{
    WorkStealingThreadPool pool(1);
    pool.SetSpinConfigForTest(0, 0);
    pool.SetSleepBackstopForTest(kRaisedBackstopUs);
    ASSERT_TRUE(WaitUntil([&] { return pool.GetIdleCountForTests() == 1; }, kWatchdog));
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // the worker blocks in its raised-backstop wait
    const SteadyClock::time_point start = SteadyClock::now();
    pool.Shutdown();
    EXPECT_LT(SteadyClock::now() - start, kWatchdog) << "Shutdown waited out the parked worker's backstop";
}

#if GE_DEBUG_INSTRUMENTATION
// (0) Held after the failed find cycle, before its idle count and bit: the waker
// finds no idle worker, so only the queued-count check before the block runs the task.
TEST(WakeWindowTest, AWorkerHeldBeforeItsIdleBitRunsTheTaskPublishedMeanwhile)
{
    RunHeldWindow(WakePoint::BeforeIdleBit);
}

// (i) Held after the idle bit, before the queued-count check: the waker claims it.
TEST(WakeWindowTest, AWorkerHeldAfterItsIdleBitIsClaimed)
{
    RunHeldWindow(WakePoint::AfterIdleBit);
}

// (ii) Held after the check, before the slot lock: it reads the flag under the lock.
TEST(WakeWindowTest, AWorkerHeldBeforeItsSlotLockReadsTheFlag)
{
    RunHeldWindow(WakePoint::BeforeSlotLock);
}

// (B) Held under the slot lock between the flag read and the block: the
// waker's flag write waits for the lock, so its notify reaches the blocked worker.
TEST(WakeWindowTest, AWorkerHeldBeforeItBlocksGetsTheFlagAfterItBlocks)
{
    RunHeldWindow(WakePoint::BeforeBlock);
}

// (iii) The waker held after its claim, before the slot lock: the worker is
// blocked when the notify lands.
TEST(WakeWindowTest, AWakerHeldAfterItsClaimStillWakesTheWorker)
{
    RunHeldWindow(WakePoint::AfterClaim);
}

// Each wake point stalls in turn under the run floods. (iv), the stall after a
// wait ends and before the idle-bit clear, also runs with the 20 us backstop,
// so wakers claim workers whose waits expired.
TEST(WakeWindowTest, StallsAtEveryWakePointLoseNoTask)
{
    ScopedWakeHook hook(&StallAtOnePoint, &OpenEveryHold);
    for (const uint32_t workers : kFloodWidths)
    {
        std::atomic<uint64_t> ran{0};
        WorkStealingThreadPool pool(workers);
        for (const WakePoint point : {WakePoint::BeforeIdleBit, WakePoint::AfterIdleBit, WakePoint::BeforeSlotLock,
                                      WakePoint::BeforeBlock, WakePoint::AfterClaim, WakePoint::BeforeIdleClear})
        {
            SCOPED_TRACE("stall at wake point " + std::to_string(static_cast<int>(point)));
            g_StallPoint.store(static_cast<int>(point));
            for (const FloodShape& shape : FloodShapes(workers, kRaisedBackstopUs))
                RunExternalRunFlood(pool, ran, shape, kStallRounds);
        }
        g_StallPoint.store(static_cast<int>(WakePoint::BeforeIdleClear));
        for (const FloodShape& shape : FloodShapes(workers, kTimeoutRaceBackstopUs))
            RunExternalRunFlood(pool, ran, shape, kStallRounds);
        g_StallPoint.store(kNoPoint);
    }
}
#else
TEST(WakeWindowTest, ForcedWindowsNeedDebugInstrumentation)
{
    GTEST_SKIP() << "the wake hooks are compiled only where GE_DEBUG_INSTRUMENTATION is 1";
}
#endif

// Pins the F2-amended skip rule (composed with F3 wake propagation): a
// producer may skip the kernel notify only when armed spinners >= its
// post-increment queued count. Four producers race a single long-lived
// spinner; under the naive "any spinner armed -> skip" rule the spinner
// absorbs all four producers' notifies while covering only one task, and any
// straggler the F3 dequeue-chain cannot reach waits out the full backstop —
// which this test raises to 500ms so a strand is unmissable.
TEST(WakeProtocolTest, MultiProducerSkipNotify)
{
    JobSystem::WorkStealingThreadPool pool(8);
    pool.SetSpinConfigForTest(20000, 1); // one spinner, armed across whole bursts
    pool.SetSleepBackstopForTest(kStressBackstopUs); // no rescue: a stranded task waits for a backstop
    const LatencyBudget budget = ReadLatencyBudget();

    constexpr int kThreads = 4;
    constexpr int kRoundsPerThread = 2000;
    std::atomic<int> over900{0};
    std::atomic<int> over100ms{0};
    std::atomic<double> worstUs{0.0};
    std::atomic<int> started{0};

    auto submitter = [&](int tid) {
        std::mt19937 rng(1000 + static_cast<unsigned>(tid));
        std::uniform_int_distribution<int> gapUs(0, 50);
        std::function<void()> tasks[2];
        tasks[0] = [] {};
        tasks[1] = [] {};

        started.fetch_add(1);
        while (started.load() < kThreads)
        {
            std::this_thread::yield();
        }

        for (int i = 0; i < kRoundsPerThread; ++i)
        {
            const double us = MeasureForkUs(pool, tasks, 2);
            if (us > 900.0)
            {
                over900.fetch_add(1);
            }
            if (us > 100000.0)
            {
                over100ms.fetch_add(1);
            }
            double prev = worstUs.load();
            while (us > prev && !worstUs.compare_exchange_weak(prev, us))
            {
            }
            BusyWaitUs(gapUs(rng));
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(submitter, t);
    }
    for (auto& t : threads)
    {
        t.join();
    }

    std::printf("[MultiProducerSkipNotify] rounds=%d worst=%.1fus over900us=%d over100ms=%d\n",
                kThreads * kRoundsPerThread, worstUs.load(), over900.load(), over100ms.load());
    EXPECT_LT(worstUs.load(), kLostWakeupLineUs)
        << "straggler with the rescue disabled: a producer skipped a notify no spinner covered";
    ExpectWithinBudget(worstUs.load(), kStressLatencyLineUs, budget, "MultiProducerSkipNotify worst round");
}

// F2 pin: the sole spinner claims a task that blocks for one backstop. Its token retires before execution (Y),
// so a second producer sees no spinner and notifies a sleeper. A token armed through execution makes the
// producer skip that notify, and the second task waits for the blocker or a sleeper's backstop.
TEST(WakeProtocolTest, BlockingCoverTaskDoesNotAbsorbSecondProducer)
{
    // Declared before the pool: a queued job can still use them when an ASSERT returns early.
    std::atomic<bool> warmed{false};
    std::atomic<bool> blockerStarted{false};
    std::atomic<bool> blockerDone{false};
    std::atomic<bool> secondDone{false};
    JobSystem::WorkStealingThreadPool pool(4);
    // One spinner, effectively-infinite window, no timeout rescue.
    pool.SetSpinConfigForTest(60000000, 1);
    pool.SetSleepBackstopForTest(kStressBackstopUs);
    const LatencyBudget budget = ReadLatencyBudget();

    // Warm one worker through an execution so it re-arms the (now long) spin
    // window; the remaining workers stay parked.
    pool.EnqueueWork([&warmed] { warmed.store(true); });
    auto deadline = SteadyClock::now() + std::chrono::seconds(2);
    while (!warmed.load() && SteadyClock::now() < deadline)
    {
        std::this_thread::yield();
    }
    ASSERT_TRUE(warmed.load());
    while (pool.GetSpinningCountForTests() != 1 && SteadyClock::now() < deadline)
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(pool.GetSpinningCountForTests(), 1u) << "no spinner armed after warmup";

    // The blocker: our own publish skips the notify (spinning=1 >= queued=1),
    // so only the armed spinner can claim it.
    pool.EnqueueWork([&blockerStarted, &blockerDone] {
        blockerStarted.store(true);
        std::this_thread::sleep_for(std::chrono::microseconds(kStressBackstopUs));
        blockerDone.store(true);
    });
    deadline = SteadyClock::now() + std::chrono::seconds(2);
    while (!blockerStarted.load() && SteadyClock::now() < deadline)
    {
        std::this_thread::yield();
    }
    ASSERT_TRUE(blockerStarted.load()) << "spinner never claimed the blocking task";
    // Retire-before-execute: while the blocker runs, no token may be armed
    // (the other workers are parked and cannot re-arm without executing).
    // The premise is Windows-shaped: a parked worker whose sleep BACKSTOP
    // expires in the claim window observes the transient queued>0, wakes
    // "with work", finds nothing, and legitimately arms a token via the spin
    // poll — no execution involved. macOS's wait_for cadence fires that
    // backstop orders of magnitude more often, so this intermediate check is
    // Windows-only; the test's actual subject (the second producer is
    // notified, not absorbed) asserts below on every platform.
#if defined(_WIN32)
    EXPECT_EQ(pool.GetSpinningCountForTests(), 0u);
#endif

    // Second producer: must be notified immediately, not absorbed by the
    // spinner that is busy inside the blocker.
    const auto t0 = SteadyClock::now();
    pool.EnqueueWork([&secondDone] { secondDone.store(true); });
    deadline = SteadyClock::now() + std::chrono::seconds(2);
    while (!secondDone.load() && SteadyClock::now() < deadline)
    {
        std::this_thread::yield();
    }
    const double elapsedMs = ElapsedUs(t0, SteadyClock::now()) / 1000.0;
    ASSERT_TRUE(secondDone.load());
    std::printf("[BlockingCoverTask] second task completed in %.3fms (blocker=%.0fms)\n", elapsedMs,
                kStressBackstopUs / 1000.0);
    // A stranded second task waits about one backstop, so this line holds at any machine load.
    EXPECT_LT(elapsedMs * 1000.0, kLostWakeupLineUs)
        << "the second producer's task waited on the blocked spinner: a lost wakeup";
    ExpectWithinBudget(elapsedMs, 50.0, budget,
                       "second producer's task waited on the blocked spinner instead of waking a sleeper (ms)");

    // Let the blocker finish before pool destruction.
    deadline = SteadyClock::now() + std::chrono::seconds(2);
    while (!blockerDone.load() && SteadyClock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(blockerDone.load());
}

// F4: spinner tokens are RAII-guarded and bounded. Sampled concurrently under
// a 4-thread fork hammer the count must stay within [0, maxSpinners], and it
// must return to exactly 0 once the pool quiesces.
TEST(SpinnerTokenTest, BoundsUnderHammerAndQuiesce)
{
    constexpr uint32_t kMaxSpinners = 2;
    JobSystem::WorkStealingThreadPool pool(4);
    pool.SetSpinConfigForTest(100, kMaxSpinners);

    std::atomic<bool> stopSampling{false};
    std::atomic<uint32_t> maxSeen{0};
    std::thread sampler([&] {
        while (!stopSampling.load(std::memory_order_relaxed))
        {
            const uint32_t s = pool.GetSpinningCountForTests();
            uint32_t prev = maxSeen.load(std::memory_order_relaxed);
            while (s > prev && !maxSeen.compare_exchange_weak(prev, s))
            {
            }
            std::this_thread::yield();
        }
    });

    constexpr int kSubmitters = 4;
    const auto stopAt = SteadyClock::now() + std::chrono::seconds(5);
    std::vector<std::thread> submitters;
    submitters.reserve(kSubmitters);
    for (int t = 0; t < kSubmitters; ++t)
    {
        submitters.emplace_back([&pool, stopAt, t] {
            std::mt19937 rng(77 + static_cast<unsigned>(t));
            std::uniform_int_distribution<int> gapUs(0, 150);
            while (SteadyClock::now() < stopAt)
            {
                pool.EnqueueWork([] {});
                BusyWaitUs(gapUs(rng));
            }
        });
    }
    for (auto& t : submitters)
    {
        t.join();
    }

    // Quiesce: drain in-flight work, then outlast any armed spin window.
    const auto drainDeadline = SteadyClock::now() + std::chrono::seconds(5);
    while (pool.GetPendingTasksApprox() != 0 && SteadyClock::now() < drainDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(pool.GetPendingTasksApprox(), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    stopSampling.store(true, std::memory_order_relaxed);
    sampler.join();

    std::printf("[SpinnerTokenAccounting] maxSeen=%u (cap %u)\n", maxSeen.load(), kMaxSpinners);
    EXPECT_LE(maxSeen.load(), kMaxSpinners);
    EXPECT_EQ(pool.GetSpinningCountForTests(), 0u);
}

// Pool destroyed while spinners are armed mid-window: no hang, no token leak,
// no crash. (Shutdown's post-join Debug assert independently checks the token
// count; this pins it from the public surface too.)
TEST(SpinnerTokenTest, ShutdownDuringSpin)
{
    for (int round = 0; round < 20; ++round)
    {
        JobSystem::WorkStealingThreadPool pool(4);
        // Long windows: workers that run out of work park in the spin loop and
        // are still armed when Shutdown lands.
        pool.SetSpinConfigForTest(50000, 2);
        for (int i = 0; i < 8; ++i)
        {
            pool.EnqueueWork([] {});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        pool.Shutdown();
        EXPECT_EQ(pool.GetSpinningCountForTests(), 0u);
    }
}

namespace {

// Submits a graph task with no dependency for the cross-pool routing test
// below. The test checks the graph publish path, not the closure.
void SubmitCountingGraphTask(JobSystem::WorkStealingThreadPool& pool, std::atomic<int>& ran)
{
    pool.Submit([&ran] { ran.fetch_add(1); }, std::span<const JobSystem::TaskHandle>());
}

} // namespace

// Pins the s_CurrentPool guard on the local-queue fast path: a worker of pool
// A submitting into pool B must publish into B's queues, not into its own
// pool-A local queue. Without the guard the graph Submit(fn, dependencies) path
// below misroutes — B's counters are incremented for tasks A drains (or
// leaves stranded), corrupting conservation in both pools; with it, both
// pools drain fully and both Shutdown conservation tripwires stay silent.
TEST(CrossPoolSubmitTest, WorkerSubmitsIntoOtherPool)
{
    constexpr int kTasks = 64;
    std::atomic<int> fireAndForgetRan{0};
    std::atomic<int> handleTasksRan{0};

    JobSystem::WorkStealingThreadPool poolA(2);
    JobSystem::WorkStealingThreadPool poolB(2);

    std::atomic<bool> submitted{false};
    poolA.EnqueueWork([&] {
        // Runs on a pool-A worker thread (s_CurrentWorker set, s_CurrentPool
        // == &poolA). Exercise both publish paths against pool B:
        // EnqueueWork (lock-free queue) and the dependency-managed Submit
        // (EnqueueTask -> PushTask, the guarded local-queue fast path).
        for (int i = 0; i < kTasks; ++i)
        {
            poolB.EnqueueWork([&fireAndForgetRan] { fireAndForgetRan.fetch_add(1); });
        }
        for (int i = 0; i < kTasks; ++i)
        {
            SubmitCountingGraphTask(poolB, handleTasksRan);
        }
        submitted.store(true);
    });

    const auto deadline = SteadyClock::now() + std::chrono::seconds(5);
    while ((!submitted.load() || fireAndForgetRan.load() < kTasks ||
            handleTasksRan.load() < kTasks ||
            poolA.GetPendingTasksApprox() != 0 || poolB.GetPendingTasksApprox() != 0) &&
           SteadyClock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(fireAndForgetRan.load(), kTasks);
    EXPECT_EQ(handleTasksRan.load(), kTasks);
    EXPECT_EQ(poolA.GetApproximateQueueSize(), 0u);
    EXPECT_EQ(poolB.GetApproximateQueueSize(), 0u);
    EXPECT_EQ(poolA.GetPendingTasksApprox(), 0u);
    EXPECT_EQ(poolB.GetPendingTasksApprox(), 0u);

    // Explicit shutdowns run the Debug conservation tripwires for both pools.
    poolB.Shutdown();
    poolA.Shutdown();
}

// The deferred-cleanup buffer is thread-local and shared across pools. An
// untagged drain by pool B cleared the WHOLE buffer, silently dropping pool
// A's pending ids — A's registry entries then leaked until destruction.
// Entries are now tagged with their owning pool: B's drain must leave A's
// entries parked, and A's next drain must still consume them.
// CreateCompletedHandle queues its cleanup on the calling thread
// deterministically, which makes the buffer contents observable.
TEST(CrossPoolSubmitTest, DeferredCleanupIsRoutedByOwningPool)
{
    JobSystem::WorkStealingThreadPool poolA(1);
    JobSystem::WorkStealingThreadPool poolB(1);

    // Queue one pending cleanup for A on this thread. Its registry entry
    // stays live until a drain of A's registry runs on this thread.
    auto aHandle0 = poolA.CreateCompletedHandle(0);
    EXPECT_EQ(poolA.GetTaskDataRegistrySizeForTests(), 1u);

    // Drain through POOL B on the same thread (the TaskHandle constructor
    // inside hits B's GetOrCreateTaskData, which drains this thread's
    // buffer). Before the pool tag this dropped A's pending id on the floor.
    auto bHandle = poolB.CreateCompletedHandle(1);
    EXPECT_EQ(poolA.GetTaskDataRegistrySizeForTests(), 1u);

    // A's own next drain must still find and erase the parked entry. The
    // triggering call queues one fresh cleanup of its own (consumed by A's
    // NEXT drain), so exactly one entry remains — with the pre-tag drop this
    // reads 2 (aHandle0's entry leaked).
    auto aHandle1 = poolA.CreateCompletedHandle(2);
    EXPECT_EQ(poolA.GetTaskDataRegistrySizeForTests(), 1u);

    // Symmetric check for B (its first entry was parked across A's drain).
    auto bHandle2 = poolB.CreateCompletedHandle(3);
    EXPECT_EQ(poolB.GetTaskDataRegistrySizeForTests(), 1u);

    poolB.Shutdown();
    poolA.Shutdown();
}

namespace {

struct ForkBenchResult
{
    double MedianUs = 0.0;
    double P99Us = 0.0;
    double P999Us = 0.0;
    double WorstUs = 0.0;
    int Over900 = 0;
};

ForkBenchResult RunForkBench(JobSystem::WorkStealingThreadPool& pool, int iterations,
                             uint32_t fanout, int gapLoUs, int gapHiUs, bool sleepGaps)
{
    std::vector<std::function<void()>> tasks(fanout, [] {});
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(iterations));
    std::mt19937 rng(0xBE7C);
    std::uniform_int_distribution<int> gapUs(gapLoUs, gapHiUs);

    for (int i = 0; i < 100; ++i) // warmup, not measured
    {
        JobSystem::ParallelFor(&pool, fanout, [&](size_t task) { tasks[task](); });
    }

    for (int i = 0; i < iterations; ++i)
    {
        const int gap = gapUs(rng);
        if (sleepGaps)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(gap));
        }
        else
        {
            BusyWaitUs(gap);
        }
        samples.push_back(MeasureForkUs(pool, tasks.data(), fanout));
    }

    std::sort(samples.begin(), samples.end());
    ForkBenchResult r;
    r.MedianUs = Percentile(samples, 0.5);
    r.P99Us = Percentile(samples, 0.99);
    r.P999Us = Percentile(samples, 0.999);
    r.WorstUs = samples.back();
    r.Over900 = static_cast<int>(std::count_if(samples.begin(), samples.end(),
                                               [](double s) { return s > 900.0; }));
    return r;
}

void PrintBenchRow(const char* name, const ForkBenchResult& r)
{
    std::printf("[bench] %-28s median=%8.2fus p99=%8.2fus p99.9=%8.2fus worst=%8.2fus over900=%d\n",
                name, r.MedianUs, r.P99Us, r.P999Us, r.WorstUs, r.Over900);
}

} // namespace

// Warm-storm fork: N=8 fan-out with 20-50µs inter-fork gaps, so spinners stay
// armed between forks. Acceptance (spec slice-1 gate): median < 20µs (live
// baseline before this slice: 100-235µs).
TEST(JobSystemBench, BENCHMARK_WarmStormFork)
{
    LogTimerResolution();
    JobSystem::WorkStealingThreadPool pool(8);

    pool.SetSpinConfigForTest(100, 2);
    const LatencyBudget budget = ReadLatencyBudget();
    const ForkBenchResult on = RunForkBench(pool, 3000, 8, 20, 50, /*sleepGaps=*/false);
    PrintBenchRow("warm-storm spin=on", on);

    pool.SetSpinConfigForTest(0, 0);
    const ForkBenchResult off = RunForkBench(pool, 3000, 8, 20, 50, /*sleepGaps=*/false);
    PrintBenchRow("warm-storm spin=off", off);

    // 20µs is the Windows bench-box gate; macOS Release measures ~22µs on
    // the same protocol (REPORT row + catastrophic ceiling there).
    if (budget.Verdict == LatencyBudgetVerdict::Skip)
    {
        GTEST_SKIP() << budget.Describe();
    }
#if defined(_WIN32)
    EXPECT_LT(on.MedianUs, budget.Scale(20.0)) << budget.Describe();
#else
    EXPECT_LT(on.MedianUs, budget.Scale(100.0))
        << "warm-storm median regressed catastrophically; " << budget.Describe();
#endif
}

// Wide-pool warm-storm fork (slice-4 F4 follow-up): >=16 workers with a
// fan-out-16 fork, so the offloaded 15 chunks take EnqueueWorkBatch's BULK
// publication path (>= kBulkPublishThreshold) against a wide sleeper set —
// the previously unbenched wide-pool bulk shape. This is a REPORT row: a
// 16-wide join's round-trip is dominated by the wake propagation
// chain (one caller wake + F3 worker-side fan-out), so the
// 8-wide <20us gate does not transfer (measured ~43us median on the bench
// box). The assertion is a strand-sanity bound only: no sample may reach the
// backstop band.
TEST(JobSystemBench, BENCHMARK_WarmStormFork16Wide)
{
    LogTimerResolution();
    JobSystem::WorkStealingThreadPool pool(16);

    pool.SetSpinConfigForTest(100, 2);
#if defined(_WIN32)
    const LatencyBudget budget = ReadLatencyBudget();
#endif
    const ForkBenchResult on = RunForkBench(pool, 3000, 16, 20, 50, /*sleepGaps=*/false);
    PrintBenchRow("warm-storm-16w spin=on", on);

    pool.SetSpinConfigForTest(0, 0);
    const ForkBenchResult off = RunForkBench(pool, 3000, 16, 20, 50, /*sleepGaps=*/false);
    PrintBenchRow("warm-storm-16w spin=off", off);

    // Gates are calibrated on the Windows bench box; macOS wake tails under
    // whole-suite load cross the band without a protocol defect (the strand
    // detector for these paths is LostWakeupStress). Other platforms keep the
    // REPORT rows above until per-platform calibration lands.
#if defined(_WIN32)
    if (budget.Verdict == LatencyBudgetVerdict::Skip)
    {
        GTEST_SKIP() << budget.Describe();
    }
    EXPECT_LE(on.WorstUs, budget.Scale(900.0))
        << "16-wide warm fork hit the backstop band (median=" << on.MedianUs << "us); " << budget.Describe();
    // Loose median ceiling (slice-5 audit F4): the tight <20us gate lives on
    // the 8-wide row; this row measures the multi-wake bulk regime (observed
    // 44-52us). ~3x observed guards against catastrophic regression without
    // gating on box noise.
    EXPECT_LT(on.MedianUs, budget.Scale(150.0))
        << "16-wide warm fork median regressed catastrophically; " << budget.Describe();
#endif
}

// True-cold fork: 5ms gaps guarantee every worker timed out of its backstop
// wait and parked before each fork. Runs the SHIPPING config (1ms backstop).
// Spec gate: p99.9 < 300µs with zero samples > 900µs. The p99.9 gate is
// asserted with headroom for scheduler tails (the 2nd-worst of 2000 samples
// tracks machine load, not the wake path: spin=off measures within noise of
// spin=on); the spec-strict number is logged for the PR table.
TEST(JobSystemBench, BENCHMARK_TrueColdFork)
{
    LogTimerResolution();
    JobSystem::WorkStealingThreadPool pool(8);

    pool.SetSpinConfigForTest(100, 2);
#if defined(_WIN32)
    const LatencyBudget budget = ReadLatencyBudget();
#endif
    const ForkBenchResult on = RunForkBench(pool, 2000, 8, 5000, 5000, /*sleepGaps=*/true);
    PrintBenchRow("true-cold spin=on", on);

    pool.SetSpinConfigForTest(0, 0);
    const ForkBenchResult off = RunForkBench(pool, 2000, 8, 5000, 5000, /*sleepGaps=*/true);
    PrintBenchRow("true-cold spin=off", off);

    std::printf("[bench] true-cold spec gate p99.9<300us: %s (measured %.1fus)\n",
                on.P999Us < 300.0 ? "PASS" : "MISS", on.P999Us);
    // The 900µs band derives from the Windows 1ms timer backstop; macOS cold
    // wakes tail at 2-3ms with spin on OR off (i.e. scheduler latency, not
    // the wake path). REPORT-only off Windows until per-platform calibration.
#if defined(_WIN32)
    if (budget.Verdict == LatencyBudgetVerdict::Skip)
    {
        GTEST_SKIP() << budget.Describe();
    }
    EXPECT_LT(on.P999Us, budget.Scale(900.0)) << budget.Describe();
    EXPECT_LE(on.WorstUs, budget.Scale(900.0)) << budget.Describe();
#endif
}

namespace {

double MeasureIdleCpuPercent(uint32_t spinWindowUs, uint32_t maxSpinners)
{
    JobSystem::WorkStealingThreadPool pool(4);
    pool.SetSpinConfigForTest(spinWindowUs, maxSpinners);

    // Cycle every worker through the loop once, then let the pool settle into
    // its idle regime before sampling.
    std::function<void()> tasks[4] = {[] {}, [] {}, [] {}, [] {}};
    JobSystem::ParallelFor(&pool, 4, [&](size_t task) { tasks[task](); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

#if GE_TEST_HAS_WIN_TIMERS
    FILETIME creation{}, exitTime{}, kernel0{}, user0{}, kernel1{}, user1{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel0, &user0);
    const auto w0 = SteadyClock::now();
    std::this_thread::sleep_for(std::chrono::seconds(10));
    GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel1, &user1);
    const double wallSec = std::chrono::duration<double>(SteadyClock::now() - w0).count();

    auto deltaSec = [](const FILETIME& a, const FILETIME& b) {
        ULARGE_INTEGER ua;
        ua.LowPart = a.dwLowDateTime;
        ua.HighPart = a.dwHighDateTime;
        ULARGE_INTEGER ub;
        ub.LowPart = b.dwLowDateTime;
        ub.HighPart = b.dwHighDateTime;
        return static_cast<double>(ub.QuadPart - ua.QuadPart) / 1e7;
    };
    const double cpuSec = deltaSec(kernel0, kernel1) + deltaSec(user0, user1);
#else
    auto processCpuSec = [] {
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        auto toSec = [](const timeval& tv) {
            return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
        };
        return toSec(usage.ru_utime) + toSec(usage.ru_stime);
    };
    const double cpu0 = processCpuSec();
    const auto w0 = SteadyClock::now();
    std::this_thread::sleep_for(std::chrono::seconds(10));
    const double wallSec = std::chrono::duration<double>(SteadyClock::now() - w0).count();
    const double cpuSec = processCpuSec() - cpu0;
#endif
    const double cores = static_cast<double>(std::max(1u, std::thread::hardware_concurrency()));
    return 100.0 * cpuSec / (wallSec * cores);
}

} // namespace

// Idle-CPU gate: with the pool completely idle for 10s, enabling the spin path
// must not change process CPU consumption by more than 0.5% absolute. (Spin
// windows are gated on productivity: a backstop-timeout wake with an empty
// queue must NOT re-arm a spin window.)
TEST(IdleCpuTest, BENCHMARK_SpinOnVsOffDelta)
{
    const double on = MeasureIdleCpuPercent(100, 2);
    const double off = MeasureIdleCpuPercent(0, 0);
    std::printf("[idle-cpu] spin-on=%.4f%% spin-off=%.4f%% delta=%.4f%% (of whole machine)\n",
                on, off, on - off);
    EXPECT_LT(std::abs(on - off), 0.5);
}

} // namespace GameEngine::Tests
