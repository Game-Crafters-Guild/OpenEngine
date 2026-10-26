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

    /// The bytes each loaded page of this source holds: PagePayloadBytes of the store's format, or
    /// of a height page for the generated provider.
    uint32 PayloadBytes() const;
};

/// How one page fetch ended: the page's bytes, or why there are none.
struct PageFetchOutcome
{
    PageAddress Address;
    /// PageSource::PayloadBytes bytes (HeightSamples reads a height page's); null: the page did not load.
    std::shared_ptr<const std::vector<uint8>> Bytes;
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
    /// The decoded page's bytes (PageStoreReader::DecodePayload), on a Background job.
    std::function<void(const PageAddress&, std::vector<uint8>)> OnLoaded;
    /// Why the page did not load (absent, unreadable, cancelled, the service or the job pool
    /// stopped), on whichever thread found out. Exactly one of OnLoaded and OnFailed runs, also when
    /// the decode is dropped without running (a pool shutdown cancels queued work).
    std::function<void(const PageAddress&, const std::string&)> OnFailed;
};

/// Loads one page of any field: a byte-range read of the page on `io`'s reader threads at the
/// request's priority, then the decode with the store's codec on a Background job. An absent page
/// fails inline.
void SubmitPageLoad(AssetIOService& io, PageLoadRequest request);

/// Fetches page `address` of `source`: a store page through `io`'s reader threads, a generated page
/// (or any page when there is no `io`) on a Background job of `pool`, inline when there is no pool.
/// `done` runs exactly once; a set `cancel` ends the fetch with an error and no bytes.
void SubmitPageFetch(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io, const PageSource& source,
                     const PageAddress& address, std::shared_ptr<std::atomic<bool>> cancel,
                     std::function<void(PageFetchOutcome)> done);

} // namespace GameEngine::PageStreaming
