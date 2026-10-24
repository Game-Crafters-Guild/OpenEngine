#pragma once

#include "PageStreaming/CpuPageDirectory.h"
#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageLoader.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{
class AssetIOService;
}

namespace GameEngine::PageStreaming
{

struct PageSampleRect;

/// The synchronous CPU fetch's share of a frame (design page-streaming D3): CPU window pages are
/// fetched on the main thread only while less than this has passed since the frame's Update began.
inline constexpr std::chrono::microseconds kCpuSyncFetchDeadline{500};

/// The clock the synchronous fetch's deadline is read from.
using PageFetchClock = std::function<std::chrono::steady_clock::time_point()>;

/// What the residency may spend, for every field of every terrain in the process together.
struct PageResidencyBudget
{
    /// The one CPU budget of decoded page samples (design D5's CPU cache: 64 MiB desktop, 32 MiB
    /// Steam Deck): the pages waiting for their upload and the pages the CPU readers sample alike.
    /// The placeable pages, this frame's CPU window pages and the CPU's pinned levels are kept past
    /// it, so the budget in force is the larger of this and their bytes; the least recently wanted
    /// others are dropped first.
    uint64 CpuBytes = 64ull * 1024ull * 1024ull;
    uint32 UploadsPerFrame = 64;          ///< new cache assignments a frame, all streams together
    uint32 UploadsPerFrameTeleport = 128; ///< the same after a teleport
    uint32 LoadsInFlight = 64;            ///< page reads and evaluations outstanding, all streams together
    /// CPU window pages fetched synchronously a frame, under kCpuSyncFetchDeadline (0: none; they
    /// then load like any other page and land at a later Update).
    uint32 CpuSyncFetchPages = 0;
};

/// The one page residency of a process: every field of every terrain streams through it under one
/// budget (design D5 and #3042). Each terrain's field is a stream with its own source; the streams
/// of one field (every terrain's height, say) share that field's physical cache (PageCache), whose
/// slots are the field's budget for the profile (design D5), whatever the number of terrains.
///
/// Each frame a stream's owner states the pages its level rule wants (Request). Update then:
/// - drains the loads that finished (a failed read is logged once with its reason and retried with
///   a backoff; only a page absent from its store is never retried);
/// - orders every stream's wanted pages in one order, pinned first, then nearest, then coarsest,
///   and keeps of each field only as many as its cache has slots (what it can place), so the
///   terrains of a field divide its slots by that order;
/// - starts loads in that order up to the loads-in-flight budget, and gives the per-frame upload cap
///   to the placeable loaded pages in that order;
/// - trims the CPU samples to their budget (the larger of PageResidencyBudget::CpuBytes and the
///   placeable pages' bytes), least recently wanted first, never a placeable page. The trim runs only
///   when there is a page to drop, so it is an event, not a per-frame cost.
/// - fetches the pages a stream's CPU readers keep (RequestCpu, design D3): the top page
///   synchronously when it is missing, so every walk down the pyramid ends on a page from the
///   first Update on; then the missing CPU window pages synchronously, nearest first and in turn
///   across streams, under the budget's page cap: the first while less than kCpuSyncFetchDeadline
///   has passed since the Update began, each later one only when the time passed plus the last
///   fetch's cost is under it (the clock is read before each fetch, so a fetch that costs more than
///   the last can still end past the deadline); the other CPU window pages, then the CPU pinned
///   levels' pages coarsest first, load like the wanted pages with at most a quarter of the loads
///   in flight, so the CPU never holds most of the queue the drawn pages wait on.
/// A page is fetched once for both sides: its samples stay on the CPU past its upload, for the CPU
/// readers (CpuPages) and for a later upload of the same page, until the trim drops them.
/// Every count it reports is edge-triggered, and a parked camera allocates nothing.
///
/// Main-thread only, except the load completions, which arrive on reader and job threads and are
/// queued under a lock until the next Update.
class PageResidencyManager
{
public:
    PageResidencyManager(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io, const PageResidencyBudget& budget,
                         PageFetchClock clock = &std::chrono::steady_clock::now);
    ~PageResidencyManager();

    PageResidencyManager(const PageResidencyManager&) = delete;
    PageResidencyManager& operator=(const PageResidencyManager&) = delete;

    /// Starts (or restarts, emptying it) field `field`'s physical cache of `slotCount` slots, the
    /// renderer's frames in flight and the arrival fade window. Its streams keep their loaded pages.
    void ConfigureField(uint32 field, uint32 slotCount, uint32 framesInFlight, float32 fadeSeconds);

    /// Starts (or restarts, dropping its pages) stream `stream` over `source`, its pages held in
    /// field `field`'s cache (configured first). A restart cancels the old source's outstanding
    /// loads; their samples never land.
    void Configure(uint32 stream, uint32 field, PageSource source);

    /// Ends stream `stream`, releases its pages from its field's cache and cancels its outstanding
    /// loads. They still count against the loads-in-flight budget until they resolve.
    void Forget(uint32 stream);

    /// The pages stream `stream` wants this frame (closed under parents: the level rule's set).
    void Request(uint32 stream, std::span<const PageWant> wanted);

    /// The pages stream `stream`'s CPU readers keep this frame: every page of level
    /// `firstPinnedLevel` and coarser, and `windowPages` (nearest first; overlapping windows may
    /// repeat a page, which keeps its first place). The window pages hold for the next Update;
    /// the pinned levels until the stream is configured again.
    void RequestCpu(uint32 stream, uint32 firstPinnedLevel, std::span<const PageAddress> windowPages);

    /// Fetches page `address` of stream `stream` now, on the calling thread, when it is not on the
    /// CPU, not in flight and not waiting for a retry: a reader that cannot wait for an Update (a
    /// pick) pays one page's read and decode. Returns whether the page is then on the CPU. Main
    /// thread, between Updates.
    bool FetchCpuPage(uint32 stream, const PageAddress& address);

    /// The content of stream `stream`'s pages over level-0 samples `level0Rect` changed in place
    /// (a modifier edit; its owner applies the change when it uploads a page). Each resident page
    /// that reads the rect (PageReadsLevel0Rect) is loaded again and rewritten into its slot
    /// (PageCache::Rewrite) once loaded, ahead of the wanted pages' loads; until then it keeps its
    /// slot and its old content, so an edit never opens a hole.
    void Refresh(uint32 stream, const PageSampleRect& level0Rect);

    /// One frame for every stream. `teleport` raises the upload cap for the frames after a jump.
    void Update(uint64 frameIndex, float32 deltaSeconds, bool teleport);

    /// Field `field`'s physical cache: every stream's assignments, uploads and transitions this
    /// frame, each page tagged with its stream.
    const PageCache& FieldCache(uint32 field) const;
    /// The decoded samples of a page of stream `stream` on the CPU (kPageSampleCount), or null.
    const std::vector<float32>* Samples(uint32 stream, const PageAddress& address) const;
    /// Stream `stream`'s CPU page directory, the one way a CPU reader looks a page up (empty when
    /// the stream is not configured or its source has no levels).
    CpuPageDirectory CpuPages(uint32 stream) const;
    /// The CPU window pages the last Update kept for stream `stream`, each once, nearest first (empty
    /// when the stream is not configured). Valid until the next Update.
    std::span<const PageAddress> CpuWindow(uint32 stream) const;

    uint32 LoadsStartedThisFrame() const { return m_LoadsStarted; }
    uint64 CpuBytes() const { return m_CpuBytes; }

private:
    struct LoadedPage
    {
        std::shared_ptr<const std::vector<float32>> Samples;
        uint64 LastWantedFrame = 0;
    };

    // A page whose load failed: when it may be tried again.
    struct LoadFailure
    {
        uint64 RetryAtFrame = 0;
        uint32 Attempts = 0;
    };

    // One finished load, from a reader or job thread.
    struct Completion
    {
        uint32 Stream = 0;
        uint64 Epoch = 0;
        PageFetchOutcome Outcome;
        bool Cpu = false; // counted against the CPU's share of the loads in flight
    };

    struct Stream
    {
        PageSource Source;
        uint32 Field = 0;
        uint32 TopLevel = 0;
        uint64 Epoch = 0; // bumps on Configure, so a restarted stream ignores its old loads
        std::shared_ptr<std::atomic<bool>> Cancel = std::make_shared<std::atomic<bool>>(false);
        std::vector<PageWant> Wanted;
        bool WantedChanged = true;
        std::vector<PageWant> Placeable; // the wanted pages its field's cache can hold, in priority order
        std::unordered_set<PageAddress, PageAddressHash> PlaceableSet;
        std::unordered_map<PageAddress, LoadedPage, PageAddressHash> Loaded;
        std::unordered_set<PageAddress, PageAddressHash> InFlight;
        std::unordered_set<PageAddress, PageAddressHash> Absent;
        std::unordered_set<PageAddress, PageAddressHash> Stale; // resident pages to load and rewrite (Refresh)
        std::unordered_map<PageAddress, LoadFailure, PageAddressHash> Failures;
        uint32 CpuPinnedLevel = ~0u;               // the CPU keeps every page of this level and coarser
        std::vector<PageAddress> CpuWindow;        // this frame's CPU window pages, nearest first
        std::vector<PageAddress> KeptCpuWindow;    // the CPU window pages the last Update kept
        std::vector<PageAddress> CpuPinnedPending; // CPU pinned pages not loaded yet, coarsest first
        // The CpuPageDirectory's entries, sized once by Configure and set where Loaded and Absent change.
        std::vector<CpuPageEntry> CpuDirectory;
        uint32 EntriesPerFace = 0;
        // The directory's change record: a ring of the latest pages SetCpuEntry wrote, change number
        // n at CpuChanges[n % CpuChanges.size()], sized once by Configure and never reallocated while
        // the stream lives, so a held directory reads it after any later change.
        std::vector<PageAddress> CpuChanges;
        uint64 CpuChangeCount = 0; // the number of the next change
    };

    // A wanted page of some stream, in the one order across streams.
    struct Candidate
    {
        uint32 Stream = 0;
        PageWant Want;
    };

    struct CompletionQueue
    {
        std::mutex Mutex;
        std::vector<Completion> Completions;
    };

    void DrainCompletions(uint64 frameIndex);
    void Land(Stream& stream, uint64 frameIndex, PageFetchOutcome& outcome);
    static CpuPageDirectory DirectoryOf(const Stream& stream);
    static void SetCpuEntry(Stream& stream, const PageAddress& address, const CpuPageEntry& entry);
    bool NeedsLoad(const Stream& stream, const PageAddress& address, uint64 frameIndex) const;
    void FetchNow(Stream& stream, uint64 frameIndex, const PageAddress& address);
    void FetchCpuNow(uint64 frameIndex, std::chrono::steady_clock::time_point start);
    void StartCpuLoads(uint64 frameIndex, bool pinned);
    void RebuildCandidates();
    void StartLoads(uint64 frameIndex);
    void StartLoad(uint32 streamId, Stream& stream, const PageAddress& address, bool cpu);
    void AssignLoaded(uint64 frameIndex, float32 deltaSeconds, bool teleport);
    void RewriteRefreshed(uint32 field, PageCache& cache);
    void TrimCpuSamples(uint64 frameIndex);

    JobSystem::WorkStealingThreadPool* m_Pool = nullptr;
    AssetIOService* m_Io = nullptr;
    PageResidencyBudget m_Budget;
    PageFetchClock m_Clock;
    // Node-based on purpose: a CpuPageDirectory points at its stream's CpuChangeCount, so a Stream
    // stays where it is while it lives.
    std::unordered_map<uint32, Stream> m_Streams;
    std::unordered_map<uint32, PageCache> m_Fields;
    std::vector<PageWant> m_Stamped; // the request being compared, its pages tagged (reused)
    std::shared_ptr<CompletionQueue> m_Completions = std::make_shared<CompletionQueue>();
    std::vector<Completion> m_Drained;     // reused each frame
    std::vector<Candidate> m_Candidates;   // every stream's placeable pages, in the one order
    std::unordered_map<uint32, uint32> m_Allowance; // per field: assignments its cache may make this frame
    std::unordered_map<uint32, uint32> m_FieldPlaced; // per field: pages placed while rebuilding the order
    std::vector<PageWant> m_LoadedPlaceable;        // reused per field each frame
    // A loaded page the trim may drop, least recently wanted first.
    struct TrimCandidate
    {
        uint64 LastWantedFrame = 0;
        uint32 Stream = 0;
        PageAddress Address;
    };

    std::vector<TrimCandidate> m_TrimOrder;                 // reused by the trim
    std::vector<std::pair<uint64, uint32>> m_CpuOrder;      // reused by RequestCpu: (page key, place)
    std::vector<uint32> m_CpuFirst;                         // reused by RequestCpu: each page's first place
    bool m_CandidatesStale = false; // a stream was forgotten or restarted
    uint64 m_CpuBytes = 0;
    uint64 m_FrameIndex = 0; // the last Update's frame
    uint32 m_InFlightCount = 0;
    uint32 m_CpuInFlightCount = 0; // the loads in flight started for the CPU pages
    uint32 m_LoadsStarted = 0;
    uint64 m_NextEpoch = 1;
};

} // namespace GameEngine::PageStreaming
