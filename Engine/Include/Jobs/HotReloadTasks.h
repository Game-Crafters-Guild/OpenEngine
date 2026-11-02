#pragma once

#include "JobSystem/TaskHandle.h"
#include "JobSystem/TaskTypes.h"
#include "Logger/Logger.h"
#include "Types/Types.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <vector>

#include <mutex>

namespace JobSystem
{
class JobChannel;
}

namespace GameEngine
{

// Forward declarations for dependency injection interfaces
namespace Jobs
{
class ICompiler;
class IAssemblyLoader;
} // namespace Jobs

// Use fully qualified JobSystem types to avoid conflicts

/**
 * @brief Result codes for hot-reload operations
 */
enum class HotReloadResult
{
    Success = 0,

    // Compilation errors
    CompilationFailed = -1,
    CompilationTimeout = -2,
    ProjectFileNotFound = -3,
    DotNetNotFound = -4,

    // File I/O errors
    FileLoadFailed = -10,
    FileNotFound = -11,
    FileAccessDenied = -12,
    FileCorrupted = -13,
    FileTooLarge = -14,

    // Assembly preparation errors
    AssemblyPrepFailed = -20,
    AssemblyInvalid = -21,
    AssemblyIncompatible = -22,
    ContextCreationFailed = -23,
    DependencyMissing = -24,

    // Swap errors
    SwapFailed = -30,
    SwapTimeout = -31,
    SwapRollbackFailed = -32,

    // System errors
    OutOfMemory = -40,
    ThreadPoolExhausted = -41,
    SystemResourcesUnavailable = -42,

    // Cancellation
    Cancelled = -100,
    CancelledByUser = -101,
    CancelledByTimeout = -102,
    CancelledByNewOperation = -103,

    // Unknown/unexpected errors
    UnknownError = -999
};

/**
 * @brief Convert HotReloadResult to human-readable string
 */
inline const char* HotReloadResultToString(HotReloadResult result)
{
    switch (result)
    {
    case HotReloadResult::Success:
        return "Success";
    case HotReloadResult::CompilationFailed:
        return "Compilation Failed";
    case HotReloadResult::CompilationTimeout:
        return "Compilation Timeout";
    case HotReloadResult::ProjectFileNotFound:
        return "Project File Not Found";
    case HotReloadResult::DotNetNotFound:
        return "DotNet Not Found";
    case HotReloadResult::FileLoadFailed:
        return "File Load Failed";
    case HotReloadResult::FileNotFound:
        return "File Not Found";
    case HotReloadResult::FileAccessDenied:
        return "File Access Denied";
    case HotReloadResult::FileCorrupted:
        return "File Corrupted";
    case HotReloadResult::FileTooLarge:
        return "File Too Large";
    case HotReloadResult::AssemblyPrepFailed:
        return "Assembly Preparation Failed";
    case HotReloadResult::AssemblyInvalid:
        return "Assembly Invalid";
    case HotReloadResult::AssemblyIncompatible:
        return "Assembly Incompatible";
    case HotReloadResult::ContextCreationFailed:
        return "Context Creation Failed";
    case HotReloadResult::DependencyMissing:
        return "Dependency Missing";
    case HotReloadResult::SwapFailed:
        return "Swap Failed";
    case HotReloadResult::SwapTimeout:
        return "Swap Timeout";
    case HotReloadResult::SwapRollbackFailed:
        return "Swap Rollback Failed";
    case HotReloadResult::OutOfMemory:
        return "Out Of Memory";
    case HotReloadResult::ThreadPoolExhausted:
        return "Thread Pool Exhausted";
    case HotReloadResult::SystemResourcesUnavailable:
        return "System Resources Unavailable";
    case HotReloadResult::Cancelled:
        return "Cancelled";
    case HotReloadResult::CancelledByUser:
        return "Cancelled By User";
    case HotReloadResult::CancelledByTimeout:
        return "Cancelled By Timeout";
    case HotReloadResult::CancelledByNewOperation:
        return "Cancelled By New Operation";
    case HotReloadResult::UnknownError:
        return "Unknown Error";
    default:
        return "Invalid Result Code";
    }
}

/**
 * @brief True when a hot-reload ended because it was cancelled, not because it failed.
 *
 * A run superseded by a newer file change reports failure, but that is the debounce
 * working as designed — logging it at error level buries the real failures during a
 * rapid-edit storm. Callers deciding a log severity must ask this rather than testing
 * errorMessage text: the messages differ per producer and are not a stable contract.
 */
inline bool IsCancellation(HotReloadResult result)
{
    switch (result)
    {
    case HotReloadResult::Cancelled:
    case HotReloadResult::CancelledByUser:
    case HotReloadResult::CancelledByTimeout:
    case HotReloadResult::CancelledByNewOperation:
        return true;
    default:
        return false;
    }
}

namespace Jobs
{

/**
 * @brief Interface for compilation operations
 */
class ICompiler
{
  public:
    virtual ~ICompiler() = default;

    struct CompilationResult
    {
        bool success;
        String output;
        std::vector<String> warnings;
        std::vector<String> errors;
    };

    /**
     * @brief Compile the scripts backing @p assemblyPath.
     *
     * @param cancelRequested Per-run supersede token (may be null). Cancel()
     * on a running compile never aborts it (F12: a running task completes),
     * so implementations must consult this token before any externally
     * visible side effect — in particular before writing the output DLL: a
     * superseded compile that ran anyway must never clobber the newer
     * compile's output on disk.
     */
    virtual CompilationResult compile(const String& assemblyPath, const std::atomic<bool>* cancelRequested) = 0;
};

/**
 * @brief Mutex serializing the final supersede check and the output-DLL write
 * across every writer of script assemblies (CompileServerCompiler,
 * ScriptManager::CompileProject and the prebuilt package-module copy).
 *
 * ScriptManager's "Script compiles" channel already runs its background
 * compiles one at a time. The writers it does not order are the synchronous
 * compiles on their caller's thread (RecompileAndReload, the initial build
 * inside Initialize when it is not deferred) and the prebuilt package copy;
 * each can land beside a channel compile that targets the same output path.
 */
std::mutex& AssemblyOutputWriteMutex();

/**
 * @brief Interface for assembly loading operations
 */
class IAssemblyLoader
{
  public:
    virtual ~IAssemblyLoader() = default;

    virtual std::vector<uint8_t> loadAssemblyBytes(const String& assemblyPath) = 0;
};

} // namespace Jobs

/**
 * @brief Base class for the hot-reload pipeline stages. The pipeline calls a
 * stage's Execute inside the job that runs the stage.
 */
class HotReloadTask
{
  public:
    // Lightweight cancellation exception to short-circuit task work
    struct CancellationException : public std::exception
    {
        const char* what() const noexcept override { return "HotReload task cancelled"; }
    };

    HotReloadTask(const String& assemblyPath, const std::shared_ptr<std::atomic<bool>>& cancelToken = nullptr)
        : m_AssemblyPath(assemblyPath), m_StartTime(std::chrono::high_resolution_clock::now()), m_CancelToken(cancelToken) {}

    /**
     * @brief Get the assembly path being processed
     */
    const String& GetAssemblyPath() const { return m_AssemblyPath; }

    /**
     * @brief Get task execution duration
     */
    std::chrono::milliseconds GetExecutionDuration() const
    {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - m_StartTime);
    }

    /**
     * @brief Get the result of task execution
     */
    HotReloadResult GetResult() const { return m_Result; }

    /**
     * @brief Get error message if task failed
     */
    const String& GetErrorMessage() const { return m_ErrorMessage; }

  protected:
    /**
     * @brief Set the task result and notify thread pool
     */
    void SetResult(HotReloadResult result, const String& errorMessage = "")
    {
        m_Result = result;
        m_ErrorMessage = errorMessage;

        bool success = (result == HotReloadResult::Success);
        Logger::Log::Debug("[HotReload] Stage for {} completed with result: {}",
                           m_AssemblyPath, success ? "success" : "failed");
    }

    /**
     * @brief Cooperative cancellation check
     */
    void CheckCancellation() const
    {
        if (m_CancelToken && m_CancelToken->load(std::memory_order_relaxed))
        {
            throw CancellationException();
        }
    }

  protected:
    String m_AssemblyPath;
    std::chrono::high_resolution_clock::time_point m_StartTime;
    HotReloadResult m_Result{HotReloadResult::Success};
    String m_ErrorMessage;
    std::shared_ptr<std::atomic<bool>> m_CancelToken;
};

/**
 * @brief Stage 1: Background assembly compilation task
 */
class CompilationTask : public HotReloadTask
{
  public:
    CompilationTask(const String& assemblyPath,
                    std::unique_ptr<Jobs::ICompiler> compiler = nullptr,
                    const std::shared_ptr<std::atomic<bool>>& cancelToken = nullptr)
        : HotReloadTask(assemblyPath, cancelToken), m_Compiler(std::move(compiler))
    {
        if (!m_Compiler)
        {
            m_Compiler = CreateDefaultCompiler();
        }
    }

    /**
     * @brief Run the compiler.
     *
     * @throws std::runtime_error with the compiler's errors when the compile
     * fails, so the task handle reports Failed and the dependent stages never
     * run. A cancelled compile returns normally with result Cancelled.
     */
    void Execute();

    /**
     * @brief Get compilation output
     */
    const String& GetCompilationOutput() const { return m_CompilationOutput; }

    /**
     * @brief Get compilation warnings
     */
    const std::vector<String>& GetWarnings() const { return m_Warnings; }

    /**
     * @brief Get compilation errors
     */
    const std::vector<String>& GetErrors() const { return m_Errors; }

  private:
    /**
     * @brief Create default compiler implementation
     */
    std::unique_ptr<Jobs::ICompiler> CreateDefaultCompiler();

  private:
    String m_CompilationOutput;
    std::vector<String> m_Warnings;
    std::vector<String> m_Errors;
    std::unique_ptr<Jobs::ICompiler> m_Compiler;
};

/**
 * @brief Read the compiled assembly's bytes and check that they start with a
 * PE signature. The compile job calls it after the compile.
 * @param loader Supplies the bytes instead of the file when set (tests).
 * @return The bytes. Empty when the file cannot be read, is not an assembly,
 * or the run is cancelled; the preparation stage then reads the file itself.
 */
std::vector<uint8_t> ReadAssemblyBytes(const String& assemblyPath, Jobs::IAssemblyLoader* loader,
                                       const std::atomic<bool>* cancelToken);

// Per-pipeline synchronization for preloaded context readiness (replaces previous static sync)
struct PreloadSync
{
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> ready{false};
};

/**
 * @brief Stage 2: Background assembly preparation task
 */
class AssemblyPrepTask : public HotReloadTask
{
  public:
    AssemblyPrepTask(const String& assemblyPath, std::vector<uint8_t> assemblyBytes,
                     std::shared_ptr<PreloadSync> preloadSync,
                     const std::shared_ptr<std::atomic<bool>>& cancelToken = nullptr)
        : HotReloadTask(assemblyPath, cancelToken), m_AssemblyBytes(std::move(assemblyBytes)), m_PreloadSync(std::move(preloadSync)) {}

    void Execute();

    /**
     * @brief Get prepared assembly context (only valid after successful execution)
     */
    void* GetPreparedContext() const { return m_PreparedContext; }

    /**
     * @brief Get assembly metadata
     */
    const String& GetAssemblyName() const { return m_AssemblyName; }
    const String& GetAssemblyVersion() const { return m_AssemblyVersion; }

  private:
    /**
     * @brief Create collectible assembly context
     */
    bool CreateCollectibleContext();

    /**
     * @brief Load assembly from memory stream
     */
    bool LoadAssemblyFromStream();

    /**
     * @brief Validate assembly integrity and compatibility
     */
    bool ValidateAssembly();

    /**
     * @brief Extract assembly metadata
     */
    void ExtractAssemblyMetadata();

  private:
    // The compile job's read; empty when that read failed.
    std::vector<uint8_t> m_AssemblyBytes;
    void* m_PreparedContext{nullptr};
    void* m_PreparedAssembly{nullptr};
    String m_AssemblyName;
    String m_AssemblyVersion;
    std::vector<String> m_ExportedTypes;

  private:
    std::shared_ptr<PreloadSync> m_PreloadSync;
};

/**
 * @brief Stage 3: swap the prepared assembly in, on the calling thread.
 *
 * The pipeline calls Execute on the main thread when a frame loop runs, and
 * on the thread that prepared the assembly when none runs.
 */
class AssemblySwapTask : public HotReloadTask
{
  public:
    /**
     * @param jobSystem Runs the old context's cleanup; null skips the cleanup.
     */
    AssemblySwapTask(JobSystem::WorkStealingThreadPool* jobSystem, const String& assemblyPath,
                     std::shared_ptr<PreloadSync> preloadSync,
                     const std::shared_ptr<std::atomic<bool>>& cancelToken = nullptr)
        : HotReloadTask(assemblyPath, cancelToken), m_JobSystem(jobSystem), m_PreloadSync(std::move(preloadSync)) {}

    void Execute();

    /**
     * @brief Get swap duration (should be <1ms)
     */
    std::chrono::milliseconds GetSwapDuration() const { return m_SwapDuration; }

    /**
     * @brief Check if swap met performance target
     */
    bool MetPerformanceTarget() const { return m_SwapDuration.count() < 1; }

  private:
    /**
     * @brief Perform atomic assembly context swap
     */
    bool PerformAtomicSwap();

    /**
     * @brief Schedule old context cleanup on background thread
     */
    void ScheduleOldContextCleanup(void* oldContext);

    /**
     * @brief Update assembly references in engine systems
     */
    void UpdateAssemblyReferences();

  private:
    JobSystem::WorkStealingThreadPool* m_JobSystem;
    void* m_OldContext{nullptr};
    std::chrono::milliseconds m_SwapDuration{0};
    bool m_SwapCompleted{false};
    std::shared_ptr<PreloadSync> m_PreloadSync;
};

/**
 * @brief Hot-reload pipeline coordinator
 *
 * A run is one "Script compiles" job that compiles and reads the assembly's
 * bytes, one pool job that prepares the assembly after it, and the swap. The
 * preparation job's completion queues the swap on the main thread, so no
 * thread waits for a run and nothing polls it.
 */
class HotReloadPipeline
{
  public:
    /**
     * @brief Pipeline execution statistics
     */
    struct PipelineStats
    {
        std::chrono::milliseconds totalDuration{0};
        std::chrono::milliseconds compilationDuration{0};
        std::chrono::milliseconds fileLoadDuration{0};
        std::chrono::milliseconds assemblyPrepDuration{0};
        std::chrono::milliseconds swapDuration{0};
        size_t assemblySize{0};
        bool success{false};
        HotReloadResult result{HotReloadResult::Success};
        String errorMessage;
    };

    /**
     * @param threadPool Runs the preparation stage and the stages' continuations.
     * @param compiles The process's "Script compiles" channel: the job that
     *        compiles and reads the bytes is a job of it, so it never runs
     *        beside another compile of the process at the compile server's
     *        pipe and never holds a compute worker while the server compiles.
     *        Must outlive the pipeline.
     */
    HotReloadPipeline(JobSystem::WorkStealingThreadPool& threadPool, JobSystem::JobChannel& compiles);

    /**
     * @brief Cancels the current run and joins every run this pipeline started.
     *
     * Blocks until every stage job of every run has finished, including a
     * compile that was Running when its run was cancelled or superseded, and
     * until no continuation of a run still uses the pipeline. A swap still
     * queued for the main thread does nothing when it runs later. Stages hold
     * what the factories gave them, such as a compiler that points at its
     * owner, so the owner must destroy the pipeline before anything those
     * stages reference.
     */
    ~HotReloadPipeline();

    /**
     * @brief Start a run, superseding the run in flight (last wins).
     * @param assemblyPath Path to the assembly to reload
     * @return Future that resolves when the run ends
     */
    std::future<PipelineStats> ExecuteAsync(const String& assemblyPath);

    // Dependency injection hooks for tests/tools
    void SetCompilerFactory(std::function<std::unique_ptr<Jobs::ICompiler>()> factory) { m_CompilerFactory = std::move(factory); }
    void SetAssemblyLoaderFactory(std::function<std::unique_ptr<Jobs::IAssemblyLoader>()> factory) { m_LoaderFactory = std::move(factory); }

    /// Called with a run's stats on the thread that ends the run (the main thread after a swap, a pool
    /// thread after a failed stage, the caller of Cancel), before the run's future is ready. Set before
    /// the first ExecuteAsync.
    void SetRunCompletedCallback(std::function<void(const PipelineStats&)> callback) { m_RunCompletedCallback = std::move(callback); }

    /**
     * @brief Cancel the current run and return without waiting.
     *
     * When this returns, the run's future has resolved as
     * CancelledByNewOperation. Queued stages never run; a stage that is already Running
     * finishes in the background, and the destructor joins it.
     */
    void Cancel();

    /**
     * @brief End the current run as timed out when it started 10 minutes or
     * more before @p now. ScriptManager's per-frame pump calls it.
     */
    void CancelOverdueRun(std::chrono::steady_clock::time_point now);

    /**
     * @brief Check if pipeline is currently executing
     */
    bool IsExecuting() const { return m_IsExecuting.load(); }

    /**
     * @brief Get current pipeline progress (0.0 to 1.0)
     */
    float GetProgress() const;

  private:
    struct Run;
    struct Lifetime;

    void OnCompileFailed(Run& run, const String& message);
    void OnPrepFailed(Run& run, const String& message);
    void StartSwap(const std::shared_ptr<Run>& run);
    void RunSwap(Run& run);
    void CancelRun(Run& run, const std::array<JobSystem::TaskHandle, 2>& stages, HotReloadResult result, const String& message);

    /**
     * @brief End @p run with @p stats: the first call for a run wins and later
     * calls do nothing. Releases the pipeline when @p run is the current run,
     * then calls the run-completed callback and resolves the run's future.
     * @return True when this call ended the run.
     */
    bool Publish(Run& run, PipelineStats stats);

    /**
     * @brief Block until every stage job has finished and no continuation uses the pipeline.
     */
    void JoinRuns();

  private:
    JobSystem::WorkStealingThreadPool& m_ThreadPool;
    JobSystem::JobChannel& m_Compiles;
    std::atomic<bool> m_IsExecuting{false};
    std::atomic<float> m_Progress{0.0f};

    // Continuations (stage callbacks and the queued swap) hold it and use the
    // pipeline only while JoinRuns has not closed it.
    std::shared_ptr<Lifetime> m_Lifetime;

    // m_RunMutex guards the three members below. ExecuteAsync runs on the file
    // watcher and on the main thread, Cancel on the main thread and in the
    // destructor, and Publish on any thread.
    std::mutex m_RunMutex;
    // The run in flight, and its compile and preparation handles, retained so
    // Cancel and supersede retire queued stages at once (F12). The handles
    // stay out of Run: the stages' callbacks hold the Run.
    std::shared_ptr<Run> m_CurrentRun;
    std::array<JobSystem::TaskHandle, 2> m_CurrentStages;
    // Every run's stage handles until they are terminal; JoinRuns waits on them.
    std::vector<JobSystem::TaskHandle> m_RunStages;

    // Optional factories (used primarily in tests)
    std::function<std::unique_ptr<Jobs::ICompiler>()> m_CompilerFactory;
    std::function<std::unique_ptr<Jobs::IAssemblyLoader>()> m_LoaderFactory;
    std::function<void(const PipelineStats&)> m_RunCompletedCallback;

    DISALLOW_COPY_AND_ASSIGN(HotReloadPipeline);
};

} // namespace GameEngine
