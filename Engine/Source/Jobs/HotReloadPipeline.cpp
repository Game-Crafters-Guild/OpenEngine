#include "Jobs/HotReloadTasks.h"
#include "Core/Engine.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Scripting/ScriptManager.h"

#include <chrono>
#include <condition_variable>
#include <format>
#include <future>
#include <mutex>
#include <span>
#include <stdexcept>

namespace GameEngine {

namespace {
// Generous: a cold compile server boots in seconds and a large project
// compiles for tens of seconds.
constexpr auto kRunTimeout = std::chrono::minutes(10);

HotReloadPipeline::PipelineStats SubmissionRefused() {
    HotReloadPipeline::PipelineStats refused;
    refused.result = HotReloadResult::Cancelled;
    refused.errorMessage = "Pipeline stages could not be submitted (thread pool shutting down)";
    return refused;
}

std::chrono::milliseconds ToMilliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration);
}
} // namespace

// One run's state, shared by its jobs and continuations. It holds no handle of
// the run: the handles' callbacks hold the Run.
struct HotReloadPipeline::Run {
    String AssemblyPath;
    std::shared_ptr<std::atomic<bool>> CancelToken = std::make_shared<std::atomic<bool>>(false);
    std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();
    std::promise<PipelineStats> Result;
    std::atomic<bool> Published{false};
    // Set by the compile job's first statement: a compile handle that ends
    // neither Completed nor started was cancelled before the job ran.
    std::atomic<bool> CompileStarted{false};
    // The compile job writes these and the preparation job, which runs after
    // it, reads them.
    std::vector<uint8_t> AssemblyBytes;
    std::chrono::steady_clock::time_point CompileEnd;
    std::chrono::steady_clock::time_point ReadEnd;
    std::chrono::steady_clock::time_point PrepEnd;
    std::shared_ptr<PreloadSync> Preload = std::make_shared<PreloadSync>();

    // The "Script compiles" job. A failed compile throws, which fails the
    // handle with the compiler's message.
    void CompileAndRead(CompilationTask& compilation, Jobs::IAssemblyLoader* loader) {
        CompileStarted.store(true);
        compilation.Execute();
        CompileEnd = std::chrono::steady_clock::now();
        AssemblyBytes = ReadAssemblyBytes(AssemblyPath, loader, CancelToken.get());
        ReadEnd = std::chrono::steady_clock::now();
    }

    // The preparation job. A compile that was terminal when this job was
    // submitted added no dependency edge, so the job can run after a failed
    // compile; it then fails too.
    void Prepare(const JobSystem::TaskHandle& compile) {
        if (!compile.IsCompleted()) {
            throw std::runtime_error("The compile did not complete");
        }
        AssemblyPrepTask prep(AssemblyPath, std::move(AssemblyBytes), Preload, CancelToken);
        prep.Execute();
        PrepEnd = std::chrono::steady_clock::now();
    }
};

// Tells the continuations whether the pipeline still exists. They can run
// after it is destroyed: a stage's callbacks run after its waiters wake, and a
// queued swap runs when the main thread next pumps.
struct HotReloadPipeline::Lifetime {
    std::mutex Mutex;
    std::condition_variable Idle;
    bool Closed = false;
    int Active = 0;

    // Holds the pipeline for one continuation. Held is false once JoinRuns
    // closed the lifetime; the continuation then does nothing.
    struct Lease {
        Lifetime& Owner;
        bool Held;
        explicit Lease(Lifetime& owner) : Owner(owner), Held(owner.TryEnter()) {}
        ~Lease() {
            if (Held) {
                Owner.Exit();
            }
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
    };

    bool TryEnter() {
        std::lock_guard<std::mutex> lock(Mutex);
        if (Closed) {
            return false;
        }
        ++Active;
        return true;
    }

    void Exit() {
        {
            std::lock_guard<std::mutex> lock(Mutex);
            --Active;
        }
        Idle.notify_all();
    }

    void CloseAndWait() {
        std::unique_lock<std::mutex> lock(Mutex);
        Closed = true;
        Idle.wait(lock, [this] { return Active == 0; });
    }
};

HotReloadPipeline::HotReloadPipeline(JobSystem::WorkStealingThreadPool& threadPool, JobSystem::JobChannel& compiles)
    : m_ThreadPool(threadPool)
    , m_Compiles(compiles)
    , m_Lifetime(std::make_shared<Lifetime>()) {
    Logger::Log::Info("HotReloadPipeline initialized");
}

HotReloadPipeline::~HotReloadPipeline() {
    if (m_IsExecuting.load()) {
        Logger::Log::Info("Cancelling pipeline execution during destruction");
    }
    Cancel();
    JoinRuns();
    Logger::Log::Info("HotReloadPipeline destroyed");
}

void HotReloadPipeline::JoinRuns() {
    std::vector<JobSystem::TaskHandle> stages;
    {
        std::lock_guard<std::mutex> lock(m_RunMutex);
        stages.swap(m_RunStages);
    }
    for (auto& stage : stages) {
        stage.Wait();
    }
    m_Lifetime->CloseAndWait();
}

std::future<HotReloadPipeline::PipelineStats> HotReloadPipeline::ExecuteAsync(const String& assemblyPath) {
    auto run = std::make_shared<Run>();
    run->AssemblyPath = assemblyPath;
    std::future<PipelineStats> result = run->Result.get_future();

    // Last wins: the run in flight is superseded before this one starts.
    std::shared_ptr<Run> superseded;
    std::array<JobSystem::TaskHandle, 2> supersededStages;
    {
        std::lock_guard<std::mutex> lock(m_RunMutex);
        superseded = std::exchange(m_CurrentRun, run);
        supersededStages = std::exchange(m_CurrentStages, {});
        std::erase_if(m_RunStages, [](const JobSystem::TaskHandle& stage) { return stage.IsDone(); });
        m_IsExecuting.store(true);
    }
    if (superseded) {
        Logger::Log::Warning("Pipeline already executing: superseding the current run with the latest request");
        CancelRun(*superseded, supersededStages, HotReloadResult::CancelledByNewOperation, "Superseded by newer change");
    }
    m_Progress.store(0.0f);
    Logger::Log::Info("🚀 Starting async hot-reload pipeline for: {}", assemblyPath);

    // Stage 1 is one "Script compiles" job that compiles and reads the bytes:
    // it queues behind any other compile of the process instead of meeting it
    // at the compile server's pipe, and it holds no compute worker.
    auto compilation = std::make_unique<CompilationTask>(assemblyPath, m_CompilerFactory ? m_CompilerFactory() : nullptr,
                                                         run->CancelToken);
    std::unique_ptr<Jobs::IAssemblyLoader> loader = m_LoaderFactory ? m_LoaderFactory() : nullptr;
    JobSystem::TaskHandle compile = m_Compiles.Submit(
        [run, compilation = std::move(compilation), loader = std::move(loader)] { run->CompileAndRead(*compilation, loader.get()); });
    if (!compile.IsValid()) {
        Publish(*run, SubmissionRefused());
        return result;
    }
    compile.OnComplete([this, lifetime = m_Lifetime, run] {
        Lifetime::Lease lease(*lifetime);
        if (lease.Held && !run->Published.load()) {
            m_Progress.store(0.5f);
        }
    });
    compile.OnFailure([this, lifetime = m_Lifetime, run](const String& message) {
        Lifetime::Lease lease(*lifetime);
        if (lease.Held) {
            OnCompileFailed(*run, message);
        }
    });

    // Stage 2 prepares the assembly on a compute worker after the compile; its
    // completion starts the swap, so no thread waits for the run.
    const JobSystem::TaskHandle prepDependencies[] = {compile};
    JobSystem::TaskHandle prep = m_ThreadPool.Submit([run, compile] { run->Prepare(compile); },
                                                     std::span<const JobSystem::TaskHandle>(prepDependencies));
    if (prep.IsValid()) {
        prep.OnComplete([this, lifetime = m_Lifetime, run] {
            Lifetime::Lease lease(*lifetime);
            if (lease.Held) {
                StartSwap(run);
            }
        });
        prep.OnFailure([this, lifetime = m_Lifetime, run, compile](const String& message) {
            Lifetime::Lease lease(*lifetime);
            if (lease.Held) {
                compile.IsCompleted() ? OnPrepFailed(*run, message) : OnCompileFailed(*run, compile.GetErrorMessage());
            }
        });
    }

    {
        std::lock_guard<std::mutex> lock(m_RunMutex);
        m_RunStages.push_back(compile);
        if (prep.IsValid()) {
            m_RunStages.push_back(prep);
        }
        if (m_CurrentRun == run) {
            m_CurrentStages = {compile, prep};
        }
    }
    if (!prep.IsValid()) {
        compile.Cancel();
        Publish(*run, SubmissionRefused());
    }
    // A Cancel that ran during the submissions saw no handles; retire the
    // queued stages here.
    if (run->CancelToken->load()) {
        compile.Cancel();
        if (prep.IsValid()) {
            prep.Cancel();
        }
    }
    return result;
}

void HotReloadPipeline::StartSwap(const std::shared_ptr<Run>& run) {
    if (run->Published.load() || run->CancelToken->load()) {
        return;
    }
    m_Progress.store(0.75f);

    // With no frame loop (no engine, or a standalone host such as dotnet
    // test) no main thread pumps the queue or ticks managed code, so the swap
    // runs here.
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized() || engine.IsStandaloneHosted()) {
        RunSwap(*run);
        return;
    }
    // The swap runs module initializers under coreclr's file-load lock. On a
    // worker it deadlocks with managed code that the main thread ticks and that
    // waits for the same lock, so the main thread runs it between ticks.
    engine.GetScriptManager().QueueMainThreadTask([this, lifetime = m_Lifetime, run] {
        Lifetime::Lease lease(*lifetime);
        if (lease.Held) {
            RunSwap(*run);
        }
    });
}

void HotReloadPipeline::RunSwap(Run& run) {
    // Cancel and supersede end the run first; its swap then never runs.
    if (run.Published.load() || run.CancelToken->load()) {
        Logger::Log::Info("[Pipeline] ℹ️ Swap skipped: the run ended before the swap ran");
        return;
    }
    AssemblySwapTask swap(&m_ThreadPool, run.AssemblyPath, run.Preload, run.CancelToken);
    swap.Execute();
    if (run.CancelToken->load()) {
        return;
    }
    PipelineStats stats;
    if (swap.GetResult() == HotReloadResult::Success) {
        stats.success = true;
    } else {
        // The old scripts stay loaded. Nothing retries the swap; the next script change starts a new run.
        stats.result = swap.GetResult();
        stats.errorMessage = std::format("The edited scripts compiled but did not load: {}. Save a script again to retry.", swap.GetErrorMessage());
    }
    stats.compilationDuration = ToMilliseconds(run.CompileEnd - run.Start);
    stats.fileLoadDuration = ToMilliseconds(run.ReadEnd - run.CompileEnd);
    stats.assemblyPrepDuration = ToMilliseconds(run.PrepEnd - run.ReadEnd);
    stats.swapDuration = swap.GetSwapDuration();
    m_Progress.store(1.0f);
    Publish(run, std::move(stats));
}

void HotReloadPipeline::OnCompileFailed(Run& run, const String& message) {
    PipelineStats stats;
    if (run.CancelToken->load()) {
        stats.result = HotReloadResult::CancelledByNewOperation;
        stats.errorMessage = "Superseded by newer change";
    } else if (!run.CompileStarted.load()) {
        stats.result = HotReloadResult::Cancelled;
        stats.errorMessage = "Compilation cancelled before it ran (thread pool shutting down)";
    } else {
        stats.result = HotReloadResult::CompilationFailed;
        stats.errorMessage = message;
        m_Progress.store(1.0f);
    }
    Publish(run, std::move(stats));
}

void HotReloadPipeline::OnPrepFailed(Run& run, const String& message) {
    PipelineStats stats;
    if (run.CancelToken->load()) {
        stats.result = HotReloadResult::CancelledByNewOperation;
        stats.errorMessage = "Superseded by newer change";
    } else {
        stats.result = HotReloadResult::AssemblyPrepFailed;
        stats.errorMessage = message;
        m_Progress.store(1.0f);
    }
    Publish(run, std::move(stats));
}

void HotReloadPipeline::CancelRun(Run& run, const std::array<JobSystem::TaskHandle, 2>& stages, HotReloadResult result,
                                  const String& message) {
    // The token first: it defuses a stage that is already Running. The result
    // next, before a cancelled handle's failure callback can publish its own.
    // Cancelling the handles makes queued stages never run (F12).
    run.CancelToken->store(true);
    PipelineStats stats;
    stats.result = result;
    stats.errorMessage = message;
    Publish(run, std::move(stats));
    for (JobSystem::TaskHandle stage : stages) {
        if (stage.IsValid()) {
            stage.Cancel();
        }
    }
}

bool HotReloadPipeline::Publish(Run& run, PipelineStats stats) {
    if (run.Published.exchange(true)) {
        return false;
    }
    stats.totalDuration = ToMilliseconds(std::chrono::steady_clock::now() - run.Start);
    {
        std::lock_guard<std::mutex> lock(m_RunMutex);
        if (m_CurrentRun.get() == &run) {
            m_CurrentRun.reset();
            m_CurrentStages = {};
            m_IsExecuting.store(false);
        }
    }

    if (stats.success) {
        Logger::Log::Info("[Pipeline] ✅ Hot-reload pipeline completed in {}ms (compile {}ms, read {}ms, prepare {}ms, swap {}ms)",
                          stats.totalDuration.count(), stats.compilationDuration.count(), stats.fileLoadDuration.count(),
                          stats.assemblyPrepDuration.count(), stats.swapDuration.count());
    } else if (IsCancellation(stats.result)) {
        Logger::Log::Info("[Pipeline] ℹ️ Hot-reload pipeline cancelled: {}", stats.errorMessage);
    } else {
        Logger::Log::Error("[Pipeline] ❌ Hot-reload pipeline failed: {}", stats.errorMessage);
    }

    if (m_RunCompletedCallback) {
        m_RunCompletedCallback(stats);
    }
    run.Result.set_value(std::move(stats));
    return true;
}

void HotReloadPipeline::Cancel() {
    Logger::Log::Info("Hot-reload pipeline cancellation requested");
    std::shared_ptr<Run> run;
    std::array<JobSystem::TaskHandle, 2> stages;
    {
        std::lock_guard<std::mutex> lock(m_RunMutex);
        run = m_CurrentRun;
        stages = m_CurrentStages;
    }
    if (run) {
        CancelRun(*run, stages, HotReloadResult::CancelledByNewOperation, "Superseded by newer change");
    }
    m_Progress.store(0.0f);
}

void HotReloadPipeline::CancelOverdueRun(std::chrono::steady_clock::time_point now) {
    std::shared_ptr<Run> run;
    std::array<JobSystem::TaskHandle, 2> stages;
    {
        std::lock_guard<std::mutex> lock(m_RunMutex);
        run = m_CurrentRun;
        stages = m_CurrentStages;
    }
    if (!run || now - run->Start < kRunTimeout) {
        return;
    }
    Logger::Log::Error("[Pipeline] ❌ Pipeline timeout after {}ms", ToMilliseconds(kRunTimeout).count());
    CancelRun(*run, stages, HotReloadResult::UnknownError, "Pipeline execution timeout");
}

float HotReloadPipeline::GetProgress() const {
    return m_Progress.load();
}

} // namespace GameEngine
