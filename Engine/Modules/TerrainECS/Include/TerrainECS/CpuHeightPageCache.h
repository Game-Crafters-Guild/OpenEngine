#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageLoader.h"
#include "PageStreaming/PageResidency.h"
#include "PageStreaming/PageStoreFormat.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
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
class HeightPageOverlay;
struct PageSampleRect;
} // namespace GameEngine::PageStreaming

// The CPU half of the height pages (design page-streaming D3, as page-streaming-planets 3.11 item 1
// amends it): the pages the CPU readers sample, kept apart from the GPU cache, whose samples the
// residency drops once uploaded. Each paged terrain is a stream, fed from the same page source and
// modifier overlay as its GPU uploads, so a CPU page holds the bytes its GPU slot holds.
namespace GameEngine::TerrainECS
{

/// The synchronous fetch's share of a frame (design D3): window pages are fetched on the main
/// thread only while less than this has passed since the frame's Update began.
inline constexpr std::chrono::microseconds kCpuSyncFetchDeadline{500};

/// What the CPU height pages may spend, for every stream together.
struct CpuHeightPageBudget
{
    /// Decoded samples kept. Pinned pages and this frame's window pages are kept past it, so the
    /// budget in force is the larger of this and their bytes; the least recently wanted of the
    /// other pages are dropped first.
    uint64 Bytes = 0;
    /// Window pages fetched synchronously a frame, under kCpuSyncFetchDeadline (0: none; every
    /// window page is then fetched on a job and lands at a later Update).
    uint32 SyncFetchPages = 0;
    /// Asynchronous fetches outstanding: a quarter of the GPU residency's, whose reader threads and
    /// Background jobs they share, so the CPU pages never hold most of the queue the drawn pages wait on.
    uint32 FetchesInFlight = PageStreaming::PageResidencyBudget{}.LoadsInFlight / 4u;
};

/// Where a page of a stream stands.
enum class CpuPageState : uint8
{
    Missing,  ///< not fetched yet, in flight, or its fetch failed and waits for a retry
    Resident, ///< its samples are here
    Absent,   ///< not in its source: its parent is the finest data there
};

/// The clock the fetch deadline is read from.
using CpuHeightPageClock = std::function<std::chrono::steady_clock::time_point()>;

/// The CPU height pages of every paged terrain, on the main thread. Each frame a stream states its
/// window pages (Want); Update then fetches:
/// - the stream's top page synchronously when it is missing, so every walk down the pyramid ends on
///   a page from the first Update on;
/// - the missing window pages synchronously, in the order wanted, while the clock reads less than
///   kCpuSyncFetchDeadline since the Update began and under the budget's page cap (the clock is read
///   once when the Update begins and once before each such fetch, so a frame overruns the deadline
///   by at most one page's fetch);
/// - every other missing window page, then the pinned levels' pages coarsest first, on Background
///   jobs (store pages through the asset reader threads), landing at a later Update.
/// The overlay is applied on the main thread as a page lands, the same way the GPU upload applies it.
/// A parked camera fetches nothing. Main thread only, except the fetch completions, which arrive on
/// reader and job threads and are queued under a lock until the next Update.
class CpuHeightPageCache
{
public:
    CpuHeightPageCache(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io, const CpuHeightPageBudget& budget,
                       CpuHeightPageClock clock = &std::chrono::steady_clock::now);
    ~CpuHeightPageCache();

    CpuHeightPageCache(const CpuHeightPageCache&) = delete;
    CpuHeightPageCache& operator=(const CpuHeightPageCache&) = delete;

    /// Starts (or restarts, dropping its pages and cancelling its fetches) stream `stream` over
    /// `source`, whose pyramid is `levels`; every page of level `firstPinnedLevel` and coarser is
    /// pinned. `overlay` (null: none) is added to every page as it lands.
    void Configure(uint32 stream, PageStreaming::PageSource source, std::span<const PageStreaming::PageStoreLevel> levels,
                   uint32 firstPinnedLevel, std::shared_ptr<const PageStreaming::HeightPageOverlay> overlay);

    /// Ends stream `stream`: drops its pages and cancels its fetches.
    void Forget(uint32 stream);

    /// The content of stream `stream`'s pages over level-0 samples `level0Rect` changed (a modifier
    /// edit), now described by `overlay`. Each resident page that reads the rect is fetched again,
    /// keeping its old samples until the new ones land.
    void Refresh(uint32 stream, std::shared_ptr<const PageStreaming::HeightPageOverlay> overlay,
                 const PageStreaming::PageSampleRect& level0Rect);

    /// This frame's window pages of stream `stream`, nearest first. A stream not given windows
    /// before an Update has none at it.
    void Want(uint32 stream, std::span<const PageStreaming::PageAddress> windowPages);

    /// One frame for every stream: lands the finished fetches, fetches as described above, then drops
    /// pages over the budget.
    void Update(uint64 frameIndex);

    /// The pyramid of stream `stream`, empty when it is not configured.
    std::span<const PageStreaming::PageStoreLevel> Levels(uint32 stream) const;

    /// Where page `address` of stream `stream` stands; its kPageSampleCount samples (row-major,
    /// apron included, normalized heights) go to `outSamples` when it is resident.
    CpuPageState FindPage(uint32 stream, const PageStreaming::PageAddress& address,
                          std::span<const float32>& outSamples) const;

private:
    struct Page
    {
        std::vector<float32> Samples; // empty unless resident
        CpuPageState State = CpuPageState::Missing;
        bool InFlight = false;
        bool Stale = false; // resident with samples an edit has changed: fetched again
        uint64 LastWantedFrame = 0;
        uint64 RetryAtFrame = 0;
        uint32 FailedAttempts = 0;
    };

    struct Stream
    {
        PageStreaming::PageSource Source;
        std::vector<PageStreaming::PageStoreLevel> Levels;
        uint32 FirstPinnedLevel = 0;
        std::shared_ptr<const PageStreaming::HeightPageOverlay> Overlay;
        uint64 Epoch = 0;
        std::shared_ptr<std::atomic<bool>> Cancel = std::make_shared<std::atomic<bool>>(false);
        FlatMap<uint64, Page> Pages;
        std::vector<PageStreaming::PageAddress> Window;
        std::vector<PageStreaming::PageAddress> PinnedPending; // pinned pages not yet landed, coarsest first
        bool AnyStale = false; // a Refresh marked pages that are not fetched again yet
    };

    struct Completion
    {
        uint32 Stream = 0;
        uint64 Epoch = 0;
        PageStreaming::PageFetchOutcome Outcome;
    };

    struct CompletionQueue
    {
        std::mutex Mutex;
        std::vector<Completion> Completions;
    };

    void DrainCompletions(uint64 frameIndex);
    void Land(Stream& stream, uint64 frameIndex, PageStreaming::PageFetchOutcome& outcome);
    bool NeedsFetch(const Page* page, uint64 frameIndex) const;
    void FetchNow(Stream& stream, uint64 frameIndex, const PageStreaming::PageAddress& address);
    void FetchLater(uint32 streamId, Stream& stream, uint64 frameIndex, const PageStreaming::PageAddress& address);
    void FetchTopPage(Stream& stream, uint64 frameIndex);
    void FetchWindows(uint64 frameIndex, std::chrono::steady_clock::time_point start);
    void FetchRemaining(uint64 frameIndex);
    void TrimToBudget(uint64 frameIndex);

    JobSystem::WorkStealingThreadPool* m_Pool = nullptr;
    AssetIOService* m_Io = nullptr;
    CpuHeightPageBudget m_Budget;
    CpuHeightPageClock m_Clock;
    FlatMap<uint32, Stream> m_Streams;
    std::shared_ptr<CompletionQueue> m_Completions = std::make_shared<CompletionQueue>();
    std::vector<Completion> m_Drained; // reused each frame
    std::vector<std::pair<uint64, uint32>> m_WantOrder; // reused by Want: (page key, place)
    std::vector<uint32> m_WantFirst;                    // reused by Want: each page's first place
    // A resident page the trim may drop, least recently wanted first.
    struct TrimCandidate
    {
        uint64 LastWantedFrame = 0;
        uint32 Stream = 0;
        uint64 Key = 0;
    };

    std::vector<TrimCandidate> m_TrimOrder; // reused by the trim
    uint64 m_Bytes = 0;
    uint32 m_InFlight = 0;
    uint64 m_NextEpoch = 1;
};

} // namespace GameEngine::TerrainECS
