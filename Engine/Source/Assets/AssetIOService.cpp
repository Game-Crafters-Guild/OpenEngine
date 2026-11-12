#include "Assets/AssetIOService.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetDbProfiler.h"
#include "Assets/AssetTasks.h"
#include "Assets/AssetDecodeGate.h"
#include "JobSystem/TaskHandle.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Platform/Thread.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>

namespace GameEngine
{

namespace
{
// 2 readers, not 1: a single thread does not saturate a buffered NVMe small-
// file workload (measured on Win11: 2000x32KB warm-cache reads, 1 thread
// ~8-11k files/s, 2 threads ~15-16k, ~1.6x). Under read-then-submit the
// decode lane is paced by read throughput, so the reader count bounds
// cold-start scan feed rate. Two is the measured knee, so the front end is
// sized at 1-2 threads; GE_ASSET_IO_THREADS overrides.
constexpr size_t kDefaultReaderThreads = 2;
constexpr size_t kMaxReaderThreads = 4;

size_t ResolveReaderThreadCount(size_t requested)
{
    if (requested == 0)
    {
        if (const char* env = std::getenv("GE_ASSET_IO_THREADS"))
        {
            requested = static_cast<size_t>(std::strtoul(env, nullptr, 10));
        }
    }
    if (requested == 0)
        requested = kDefaultReaderThreads;
    return std::min(requested, kMaxReaderThreads);
}

bool IsCancelled(const AssetIOService::ReadRequest& request)
{
    return request.CancelRequested && request.CancelRequested->load(std::memory_order_acquire);
}

// A texture decode that misses the cook cache runs the import cook (CookTexture)
// inside its decode job, minutes for a large texture; the decode gate keeps
// these, with their cooks' helper bands, to its texture share.
AssetDecodeGate::Work DecodeWork(const AssetIOService::ReadRequest& request)
{
    return request.Metadata.Type == AssetType::Texture ? AssetDecodeGate::Work::Texture
                                                       : AssetDecodeGate::Work::Other;
}

// Callback invocations are fenced: a throwing completion callback must not
// escape into the reader thread (std::terminate) — the old pipeline ran the
// equivalent resolution under TaskHandle's catch-and-log wrapper.
void InvokeOnFailure(const AssetIOService::ReadRequest& request, const String& error)
{
    if (!request.OnFailure)
        return;
    try
    {
        request.OnFailure(error);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("AssetIOService: OnFailure callback threw for {}: {}",
                           request.AssetGuid.ToString(), e.what());
    }
    catch (...)
    {
        Logger::Log::Error("AssetIOService: OnFailure callback threw for {}", request.AssetGuid.ToString());
    }
}

void InvokeOnSubmitted(const AssetIOService::ReadRequest& request, const JobSystem::TaskHandle& handle)
{
    if (!request.OnSubmitted)
        return;
    try
    {
        request.OnSubmitted(handle);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("AssetIOService: OnSubmitted callback threw for {}: {}",
                           request.AssetGuid.ToString(), e.what());
    }
    catch (...)
    {
        Logger::Log::Error("AssetIOService: OnSubmitted callback threw for {}", request.AssetGuid.ToString());
    }
}
} // namespace

AssetIOService::~AssetIOService()
{
    Stop();
}

void AssetIOService::Start(JobSystem::WorkStealingThreadPool& jobSystem, size_t threadCount)
{
    Admission& admission = *m_Admission;
    {
        std::lock_guard<std::mutex> lock(admission.Mutex);
        if (admission.Running)
        {
            Logger::Log::Warning("AssetIOService: Start called while already running");
            return;
        }
        m_JobSystem = &jobSystem;
        admission.Running = true;
        admission.InlineMode = jobSystem.IsInlineMode();

        // Inline pool (0 workers, e.g. single-threaded wasm): no reader threads —
        // SubmitRead runs the read and its execute-at-publish decode continuation
        // on the calling thread. The decode gate stays unarmed: with no workers
        // to protect it only adds a slot a nested load could self-deadlock on.
        if (admission.InlineMode)
        {
            Logger::Log::Info("AssetIOService: started inline (JobSystem has no workers); reads run on the submitting thread");
            return;
        }

        // GE_ASSET_DECODE_MAX overrides the gate's size, for diagnostics.
        size_t maxWorkersOverride = 0;
        if (const char* env = std::getenv("GE_ASSET_DECODE_MAX"); env && env[0] != '\0')
        {
            const long v = std::strtol(env, nullptr, 10);
            if (v > 0)
                maxWorkersOverride = static_cast<size_t>(v);
        }
        // A released slot may admit a request a reader skipped. Notified under the
        // mutex, as every Changed notify is: a reader evaluates its claim under
        // it, so an unlocked notify could slip between its check and its park.
        m_DecodeGate = std::make_shared<AssetDecodeGate>(jobSystem.GetWorkerCount(), maxWorkersOverride,
                                                         [admission = m_Admission]
                                                         {
                                                             std::lock_guard<std::mutex> wake(admission->Mutex);
                                                             admission->Changed.notify_one();
                                                         });
    }

    const size_t count = ResolveReaderThreadCount(threadCount);
    m_Threads.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        m_Threads.emplace_back([this] { ReaderThreadMain(); });
    }

    Logger::Log::Info("AssetIOService: started with {} reader thread(s); decode gate {} of {} pool worker(s), "
                      "{} of them for texture work",
                      count, m_DecodeGate->MaxWorkers(), jobSystem.GetWorkerCount(), m_DecodeGate->MaxTextureWorkers());
}

void AssetIOService::Stop()
{
    Admission& admission = *m_Admission;
    {
        std::lock_guard<std::mutex> lock(admission.Mutex);
        if (!admission.Running)
            return;
        admission.Running = false;
        // Under the mutex: a reader evaluates its wait under it, so an unlocked
        // notify could slip between its check and its park (lost wakeup).
        admission.Changed.notify_all();
    }

    for (auto& thread : m_Threads)
    {
        if (thread.joinable())
            thread.join();
    }
    m_Threads.clear();

    // Fail any requests still queued so their waiters resolve. Readers are
    // joined and SubmitRead rejects once Running is false; the requests leave
    // the lock first because OnFailure re-enters AssetManager state.
    std::vector<ReadRequest> abandoned;
    {
        std::lock_guard<std::mutex> lock(admission.Mutex);
        for (auto& queue : admission.Queues)
        {
            for (auto& request : queue)
                abandoned.push_back(std::move(request));
            queue.clear();
        }
    }
    for (const auto& request : abandoned)
    {
        InvokeOnFailure(request, "Asset read abandoned: AssetIOService stopped");
    }

    if (!abandoned.empty())
    {
        Logger::Log::Info("AssetIOService: abandoned {} queued read(s) during shutdown", abandoned.size());
    }
}

void AssetIOService::SubmitRead(ReadRequest request)
{
    Admission& admission = *m_Admission;
    bool runInline = false;
    {
        std::lock_guard<std::mutex> lock(admission.Mutex);
        if (admission.Running)
        {
            if (admission.InlineMode)
            {
                runInline = true;
            }
            else
            {
                const auto level = static_cast<size_t>(request.Priority);
                admission.Queues[level < kPriorityLevels ? level : kPriorityLevels - 1].push_back(std::move(request));
                admission.Changed.notify_one();
                return;
            }
        }
    }

    if (runInline)
    {
        ExecuteRead(request, /*holdsDecodeSlot=*/false);
        return;
    }

    // Not running: resolve inline so no waiter strands.
    InvokeOnFailure(request, "Asset read rejected: AssetIOService not running");
}

bool AssetIOService::CancelRequest(const GUID& guid, const std::shared_ptr<std::atomic<bool>>& cancelRequested)
{
    std::vector<ReadRequest> removed;
    {
        std::lock_guard<std::mutex> lock(m_Admission->Mutex);
        for (auto& queue : m_Admission->Queues)
        {
            for (auto it = queue.begin(); it != queue.end();)
            {
                if (it->AssetGuid == guid && it->CancelRequested == cancelRequested)
                {
                    removed.push_back(std::move(*it));
                    it = queue.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }

    // Resolve outside the queue lock: OnFailure re-enters AssetManager state
    // (promise fulfilment, in-flight bookkeeping).
    for (const auto& request : removed)
    {
        InvokeOnFailure(request, "Asset load cancelled");
    }
    return !removed.empty();
}

size_t AssetIOService::GetQueuedReadCount() const
{
    std::lock_guard<std::mutex> lock(m_Admission->Mutex);
    size_t queued = 0;
    for (const auto& queue : m_Admission->Queues)
    {
        queued += queue.size();
    }
    return queued;
}

bool AssetIOService::TryClaim(Admission& admission, AssetDecodeGate& gate, ReadRequest& request)
{
    // Highest priority first, FIFO within a level, skipping requests whose class
    // the gate has full: a texture behind a full texture share must not hold up
    // the material queued after it.
    bool classFull[2] = {false, false}; // indexed by AssetDecodeGate::Work
    for (size_t level = kPriorityLevels; level-- > 0;)
    {
        auto& queue = admission.Queues[level];
        for (auto it = queue.begin(); it != queue.end(); ++it)
        {
            const AssetDecodeGate::Work work = DecodeWork(*it);
            bool& full = classFull[static_cast<size_t>(work)];
            if (full)
                continue;
            if (!gate.TryAcquire(work))
            {
                full = true;
                if (classFull[0] && classFull[1])
                    return false;
                continue;
            }
            request = std::move(*it);
            queue.erase(it);
            return true;
        }
    }
    return false;
}

void AssetIOService::ReaderThreadMain()
{
    Platform::SetCurrentThreadName("Asset IO");

    Admission& admission = *m_Admission;
    for (;;)
    {
        ReadRequest request;
        {
            std::unique_lock<std::mutex> lock(admission.Mutex);
            for (;;)
            {
                if (!admission.Running)
                    return; // remaining queue entries are drained by Stop()
                if (TryClaim(admission, *m_DecodeGate, request))
                    break;
                admission.Changed.wait(lock);
            }
        }

        ExecuteRead(request, /*holdsDecodeSlot=*/true);
    }
}

void AssetIOService::ExecuteRead(ReadRequest& request, bool holdsDecodeSlot)
{
    // The claim's gate slot rides the decode lambda as a move-only token
    // released on the lambda's DESTRUCTION, not its execution: a decode task
    // can end executed, cancelled-before-run, or rejected at submit, and each
    // path destroys the envelope exactly once. Releasing on execution alone
    // leaks a slot per cancelled load (panel churn cancels freely) until the
    // gate wedges shut. Every early return below releases it the same way.
    struct SlotToken
    {
        std::shared_ptr<AssetDecodeGate> Gate;
        AssetDecodeGate::Work Work;

        SlotToken(std::shared_ptr<AssetDecodeGate> gate, AssetDecodeGate::Work work) : Gate(std::move(gate)), Work(work) {}
        SlotToken(SlotToken&& other) noexcept : Gate(std::move(other.Gate)), Work(other.Work) { other.Gate.reset(); }
        SlotToken& operator=(SlotToken&&) = delete;
        SlotToken(const SlotToken&) = delete;
        SlotToken& operator=(const SlotToken&) = delete;
        ~SlotToken()
        {
            if (Gate)
                Gate->Release(Work);
        }
    };

    std::optional<SlotToken> slot;
    if (holdsDecodeSlot)
        slot.emplace(m_DecodeGate, DecodeWork(request));

    // A cancel that landed while the request was queued (but lost the
    // queue-removal race to this claim) still wins here: skip the read.
    if (IsCancelled(request))
    {
        InvokeOnFailure(request, "Asset load cancelled");
        return;
    }

    const auto startTime = std::chrono::high_resolution_clock::now();

    AssetDecodeInputFiles inputs;
    const bool readsWithoutBytes = ReadDecodeInputsWithoutBytes(request, inputs);
    ByteBuffer data;
    String readError;
    bool read = readsWithoutBytes;
    if (!readsWithoutBytes)
    {
        inputs = {};
        AssetDbProfiler::StageScope profileScope(AssetDbProfiler::Bucket::StageFileIO);
        read = request.RangeBytes > 0 ? ReadFileRange(request, data, readError)
                                      : ReadFileWithRetry(request.Metadata, data, readError);
    }
    if (!read)
    {
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - startTime);

        Logger::Log::Error("AssetIOService: read failed for {} ({}) after {}μs: {}",
                           request.Metadata.Path.string(), request.AssetGuid.ToString(),
                           duration.count(), readError);
        AssetTaskPerformanceMonitor::GetInstance().RecordTaskExecution(
            "AssetFileIOTask", duration.count() / 1000.0, false);
        InvokeOnFailure(request, readError);
        return;
    }

    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - startTime);
    // The level check comes first, so a read below Debug level builds no GUID string.
    if (Logger::Log::ShouldLog(Logger::LogLevel::Debug))
    {
        Logger::Log::Debug("AssetIOService: read {} bytes for {} in {}μs",
                           data.Size(), request.AssetGuid.ToString(), duration.count());
    }
    AssetTaskPerformanceMonitor::GetInstance().RecordTaskExecution(
        "AssetFileIOTask", duration.count() / 1000.0, true);

    // A claimed read runs to completion (mirroring F12: canceller wins only
    // while queued), but its result is discarded here — a decode job is never
    // submitted for a cancelled request.
    if (IsCancelled(request))
    {
        InvokeOnFailure(request, "Asset load cancelled");
        return;
    }

    // The files the parser names for the decode are read here too, so the decode job does
    // not block its worker on them; it takes them from the published inputs.
    if (!readsWithoutBytes && request.ResolveDecodeInputs)
    {
        String inputError;
        bool inputsRead = false;
        {
            AssetDbProfiler::StageScope profileScope(AssetDbProfiler::Bucket::StageFileIO);
            inputsRead = ReadDecodeInputs(
                request, [&request, &data] { return request.ResolveDecodeInputs(data); }, inputs, inputError);
        }
        if (!inputsRead)
        {
            InvokeOnFailure(request, inputError);
            return;
        }
    }

    // Read-then-submit: sequencing by construction, no dependency-graph edges.
    // Background priority keeps scan floods off the Normal (frame work) lane
    // in the QUEUE; the gate slot taken at the claim is what bounds RUNNING
    // occupancy — priority cannot preempt a cook that already holds a worker.
    JobSystem::TaskHandle handle = m_JobSystem->Submit(
        [process = std::move(request.ProcessData), bytes = std::move(data), inputs = std::move(inputs),
         slot = std::move(slot)]() mutable -> SharedPtr<Asset>
        {
            const ScopedAssetDecodeInputFiles published(&inputs);
            return process(std::move(bytes));
        },
        JobSystem::JobPriority::Background);

    if (!handle.IsValid())
    {
        // F13a shutdown gate: the pool refused the submission. Resolve the
        // load here — there is no handle whose callbacks could do it.
        InvokeOnFailure(request, "Asset decode rejected: JobSystem shutting down");
        return;
    }

    InvokeOnSubmitted(request, handle);
}

bool AssetIOService::ReadDecodeInputsWithoutBytes(const ReadRequest& request, AssetDecodeInputFiles& outFiles)
{
    if (!request.ResolveDecodeInputsWithoutBytes || request.RangeBytes > 0)
        return false;
    String error;
    {
        AssetDbProfiler::StageScope profileScope(AssetDbProfiler::Bucket::StageFileIO);
        if (!ReadDecodeInputs(request, [&request] { return request.ResolveDecodeInputsWithoutBytes(); }, outFiles,
                              error))
            return false;
    }
    return !outFiles.Files.empty() &&
           std::ranges::all_of(outFiles.Files, [](const auto& file) { return file.second.has_value(); });
}

bool AssetIOService::ReadDecodeInputs(const ReadRequest& request, FunctionRef<AssetDecodeInputs()> resolve,
                                      AssetDecodeInputFiles& outFiles, String& outError)
{
    AssetDecodeInputs named;
    try
    {
        named = resolve();
    }
    catch (const std::exception& e)
    {
        outError = "Resolving the decode inputs of " + request.Metadata.Path.string() + " failed: " + e.what();
        return false;
    }
    catch (...)
    {
        outError = "Resolving the decode inputs of " + request.Metadata.Path.string() + " failed";
        return false;
    }

    outFiles.Files.reserve(named.Files.size());
    for (const AssetDecodeInput& input : named.Files)
    {
        if (IsCancelled(request))
        {
            outError = "Asset load cancelled";
            return false;
        }
        // A path named twice is read once.
        const auto [file, added] = outFiles.Files.try_emplace(input.Path.generic_string());
        if (!added)
            continue;
        try
        {
            if (ByteBuffer fileBytes; ReadAssetDecodeInputFromDisk(input, fileBytes))
                file->second = std::move(fileBytes);
        }
        catch (const std::exception& e)
        {
            outError = "Cannot read " + input.Path.string() + ", a file the decode of " +
                       request.Metadata.Path.string() + " reads: " + e.what();
            return false;
        }
    }
    if (IsCancelled(request))
    {
        outError = "Asset load cancelled";
        return false;
    }
    outFiles.Plan = std::move(named.Plan);
    return true;
}

bool AssetIOService::ReadFileRange(const ReadRequest& request, ByteBuffer& outData, String& outError)
{
    SharedFileReader reader;
    if (!reader.Open(request.Metadata.Path))
    {
        outError = "Cannot open file: " + request.Metadata.Path.string();
        return false;
    }
    const int64 fileBytes = reader.Size();
    if (fileBytes < 0 || request.RangeOffset > static_cast<uint64>(fileBytes) ||
        request.RangeBytes > static_cast<uint64>(fileBytes) - request.RangeOffset)
    {
        outError = "Short read: " + std::to_string(request.RangeBytes) + " bytes at offset " +
                   std::to_string(request.RangeOffset) + " lie past the end of " + request.Metadata.Path.string();
        return false;
    }
    outData = ByteBuffer(static_cast<size_t>(request.RangeBytes));
    if (!reader.SeekTo(request.RangeOffset) ||
        reader.Read(outData.Data(), request.RangeBytes) != static_cast<int64>(request.RangeBytes))
    {
        outData = ByteBuffer();
        outError = "Short read of " + std::to_string(request.RangeBytes) + " bytes at offset " +
                   std::to_string(request.RangeOffset) + " of " + request.Metadata.Path.string();
        return false;
    }
    return true;
}

bool AssetIOService::ReadFileWithRetry(const AssetMetadata& metadata, ByteBuffer& outData, String& outError)
{
    // Reads open with full sharing (see AssetCore/SharedFileRead.h), so an
    // external safe-save rename never fails against this handle. The retry
    // loop covers the inverse direction: in-place writers that briefly hold
    // the file exclusively while rewriting it, and the delete-then-rewrite
    // window in which the path is transiently absent.
    constexpr int kMaxRetries = 5;
    constexpr int kRetrySleepMs = 10;

    outData = ByteBuffer();
    outError.clear();

    bool opened = false;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt)
    {
        if (ReadFileBytesShared(metadata.Path, outData))
        {
            opened = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kRetrySleepMs));
    }
    if (!opened)
    {
        outError = "Failed to read file: " + metadata.Path.string();
        return false;
    }

    if (outData.Size() == 0)
    {
        // A truncate-then-write save (VS Code style) can be observed opened
        // but empty. The retry loop above only tolerates open/read *failures*,
        // so give the writer one retry tick (same window constants) before
        // treating emptiness as a hard failure.
        std::this_thread::sleep_for(std::chrono::milliseconds(kRetrySleepMs));
        (void)ReadFileBytesShared(metadata.Path, outData);
        if (outData.Size() == 0)
        {
            outError = "Asset data is empty: " + metadata.Path.string();
            return false;
        }
    }

    // AssetMetadata::FileSize is advisory: registry/cache entries can lag
    // behind hot-reloaded or rebuilt assets. Accept the read when the on-disk
    // size confirms it, or when the asset is a text type that is commonly
    // edited in place (UI styles/layout).
    if (outData.Size() != metadata.FileSize)
    {
        std::error_code ec;
        const auto currentSize = std::filesystem::file_size(metadata.Path, ec);
        const bool sizeConfirmed = !ec && currentSize == outData.Size();
        if (!sizeConfirmed && !IsTextBasedAssetType(metadata.Type))
        {
            outError = "File size mismatch for " + metadata.Path.string() +
                       ": expected " + std::to_string(metadata.FileSize) +
                       ", got " + std::to_string(outData.Size());
            outData = ByteBuffer();
            return false;
        }
        Logger::Log::Debug("AssetIOService: accepting stale fileSize for {} (expected {}, got {})",
                           metadata.Path.string(), metadata.FileSize, outData.Size());
    }

    return true;
}

} // namespace GameEngine
