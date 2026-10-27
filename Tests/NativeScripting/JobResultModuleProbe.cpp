// Stands in for a hot-reloadable user module that returns a value from a job or hands one to a continuation.
// The value type's destructor, its std::any type table and the continuation are this image's code.
#include "Core/Engine.h"
#include "Core/EngineLoggerBridge.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
struct ProbeResult
{
    std::atomic<int>* Live;
    explicit ProbeResult(std::atomic<int>* live) : Live(live) { Live->fetch_add(1); }
    ProbeResult(const ProbeResult& other) : ProbeResult(other.Live) {}
    ~ProbeResult() { Live->fetch_sub(1); }
};

std::atomic<int> s_EngineResultsLive{0};

// The value of the job on the engine's pool. Its last copy logs when it dies.
struct EngineProbeResult
{
    ProbeResult Counted{&s_EngineResultsLive};
    EngineProbeResult() = default;
    EngineProbeResult(const EngineProbeResult&) = default;
    ~EngineProbeResult()
    {
        if (s_EngineResultsLive.load() == 1)
            Logger::Log::Info("JobResultProbe: engine job result destroyed");
    }
};

// At load into an initialized EngineCore, completes a value-returning job on the engine's pool and drops
// its handle. Only the pool's registry then holds the result.
struct SubmitOnEngineLoad
{
    SubmitOnEngineLoad()
    {
        auto& engine = GameEngine::EngineCore::GetInstance();
        if (!engine.IsInitialized())
            return;
        Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
        JobSystem::WorkStealingThreadPool& pool = engine.GetJobSystem();
        {
            JobSystem::TaskHandle handle = pool.Submit([] { return EngineProbeResult(); });
            handle.Wait();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (pool.GetPendingTasksApprox() != 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        Logger::Log::Info("JobResultProbe: engine job completed");
    }
} s_SubmitOnEngineLoad;
} // namespace

// Submits a value-returning job, waits for it and drops the handle.
extern "C" __declspec(dllexport) void GE_JobResultProbe_SubmitAndWait(
    JobSystem::WorkStealingThreadPool* pool, std::atomic<int>* live)
{
    JobSystem::TaskHandle handle = pool->Submit([live] { return ProbeResult(live); });
    handle.Wait();
}

// Submits a job on a pool whose worker is busy, adds continuations of both kinds, cancels the job and adds
// both kinds again. No continuation can fire; each owns a counted value.
extern "C" __declspec(dllexport) void GE_JobResultProbe_CancelWithContinuations(
    JobSystem::WorkStealingThreadPool* pool, std::atomic<int>* live)
{
    JobSystem::TaskHandle handle = pool->Submit([] {});
    handle.OnComplete([value = ProbeResult(live)] {});
    handle.OnComplete([value = ProbeResult(live)](const JobSystem::TaskHandle&) {});
    handle.Cancel();
    handle.OnComplete([value = ProbeResult(live)] {});
    handle.OnComplete([value = ProbeResult(live)](const JobSystem::TaskHandle&) {});
}
