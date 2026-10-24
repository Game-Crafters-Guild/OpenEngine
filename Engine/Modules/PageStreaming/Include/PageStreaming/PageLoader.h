#pragma once

#include "PageStreaming/PageAddress.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <string>
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

class GeneratedHeightPageProvider;
class PageStoreReader;

/// Where one field's pages come from: a cooked store (read through the asset reader threads) or a
/// generated provider (evaluated on Background jobs). Exactly one is set.
struct PageSource
{
    std::shared_ptr<const PageStoreReader> Store;
    std::shared_ptr<const GeneratedHeightPageProvider> Generated;
};

/// How one page fetch ended: the samples, or why there are none.
struct PageFetchOutcome
{
    PageAddress Address;
    std::shared_ptr<const std::vector<float32>> Samples; ///< kPageSampleCount; null: the page did not load
    std::string Error;   ///< why it did not load, when it is in its source
    bool Absent = false; ///< not in its source at all (a sparse store, or outside the generated pyramid)
};

/// The frame a page whose fetch failed `failedAttempts` times before this failure, at frame
/// `frameIndex`, is fetched again: the wait doubles from one frame up to 64.
uint64 NextPageFetchRetryFrame(uint64 frameIndex, uint32 failedAttempts);

/// Fetches page `address` of `source` on the calling thread: a store's read and decode, or the
/// generated provider's evaluation. Thread-safe.
PageFetchOutcome FetchPage(const PageSource& source, const PageAddress& address);

/// One asynchronous page load.
struct PageLoadRequest
{
    /// The open store to read from; kept alive until the load resolves.
    std::shared_ptr<const PageStoreReader> Store;
    /// The store's identity for the reader queue (cancellation and logs): the asset it was cooked from.
    GUID StoreAsset;
    PageAddress Address;
    AssetLoadPriority Priority = AssetLoadPriority::Normal;
    /// Set to drop the load: a queued read is skipped, a finished read is not decoded.
    std::shared_ptr<std::atomic<bool>> Cancel;
    /// The decoded page's kPageSampleCount samples, on a Background job.
    std::function<void(const PageAddress&, std::vector<float32>)> OnLoaded;
    /// Why the page did not load (absent, unreadable, cancelled, the service or the job pool
    /// stopped), on whichever thread found out. Exactly one of OnLoaded and OnFailed runs, also when
    /// the decode is dropped without running (a pool shutdown cancels queued work).
    std::function<void(const PageAddress&, const std::string&)> OnFailed;
};

/// Loads one height page: a byte-range read of the page on `io`'s reader threads at the request's
/// priority, then the decode on a Background job. An absent page fails inline.
void SubmitHeightPageLoad(AssetIOService& io, PageLoadRequest request);

/// Fetches page `address` of `source` asynchronously: a store page through `io`'s reader threads
/// (SubmitHeightPageLoad), a generated page (or a store page when there is no `io`) on a Background
/// job of `pool`, inline when there is no pool. `done` runs exactly once, on whichever thread
/// finished; a set `cancel` ends the fetch with an error and no samples.
void SubmitPageFetch(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io, const PageSource& source,
                     const PageAddress& address, std::shared_ptr<std::atomic<bool>> cancel,
                     std::function<void(PageFetchOutcome)> done);

} // namespace GameEngine::PageStreaming
