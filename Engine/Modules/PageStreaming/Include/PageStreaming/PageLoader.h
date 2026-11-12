#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"

#include "AssetCore/AssetTypes.h"

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
class PageStoreAccess;
class PageStoreReader;

/// The planes of one lattice generated per page (a planet's surface). Fill writes each plane of page
/// `address` in the order of Formats, row-major, a Bc5 plane as its two channels before the encode;
/// false outside the pyramid. Called on Background jobs, any number at once.
struct GeneratedPagePlanes
{
    std::vector<PageStoreLevel> Levels; ///< the lattice of every plane
    uint32 FaceCount = 0;
    std::vector<PageFieldFormat> Formats;
    /// The samples of its level past its owned area that a page's fill reads: a change there rewrites it.
    uint32 MarginSamples = kPageApronSamples;
    std::function<bool(const PageAddress& address, std::span<const std::span<uint8>> planes)> Fill;
};

/// Where one field's pages come from: a cooked store (read through the asset reader threads), a
/// generated provider (evaluated on Background jobs), the planes of one lattice that a bake writes
/// while they are read (read on Background jobs) or generated planes (filled and encoded on Background
/// jobs). Exactly one of Store, Generated, Planes and GeneratedPlanes is set.
struct PageSource
{
    std::shared_ptr<const PageStoreReader> Store;
    std::shared_ptr<const GeneratedHeightPageProvider> Generated;
    /// One store access per plane, all on one lattice. A page loads with every plane or not at all.
    std::vector<std::shared_ptr<const PageStoreAccess>> Planes;
    std::shared_ptr<const GeneratedPagePlanes> GeneratedPlanes;

    /// The bytes each loaded page of this source holds: PagePayloadBytes of the store's format, of a
    /// height page for the generated provider, or of each plane in turn, in the order of Planes or of
    /// the generated planes' Formats (a Bc5 plane as its blocks, as a store loads them).
    uint32 PayloadBytes() const;
    /// The sum of the planes' PageStoreAccess::Version: it changes when a page of any plane can
    /// become readable. 0 for a source without planes.
    uint64 PlanesVersion() const;
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

/// Fetches page `address` of `source` on the calling thread: a store's read and decode, the
/// generated provider's evaluation, each plane's read, or the generated planes' fill and Bc5 encode.
/// Thread-safe. A plane's read waits while its bake changes the store's layout; the page is absent
/// while any plane does not hold it.
PageFetchOutcome FetchPage(const PageSource& source, const PageAddress& address);

/// Why a fetch was started, returned with its outcome: the stream, its restart and its source version.
struct PageFetchTag
{
    uint32 Stream = 0;
    uint64 Epoch = 0;
    uint64 SourceVersion = 0;
    bool Cpu = false; ///< started for the CPU's pages
};

/// One finished fetch.
struct FinishedPageFetch
{
    PageFetchTag Tag;
    PageFetchOutcome Outcome;
};

/// Fetches pages off the calling thread and returns each outcome once, through Finished: a store page
/// on `io`'s reader threads at High priority, then a Background job of `pool`; any other page (every
/// page without `io`) on a Background job, inline without a pool. Called on one thread at a time.
class PageFetcher
{
public:
    /// `loadsInFlight`: the most fetches the owner keeps in flight. The fetcher sizes its lists for
    /// them once and keeps as many buffers that Reuse gives back, so a store page that streams in
    /// again allocates no fetch state and no payload.
    PageFetcher(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io, uint32 loadsInFlight);
    /// The fetches still in flight end on their own threads; their outcomes are dropped.
    ~PageFetcher();

    PageFetcher(const PageFetcher&) = delete;
    PageFetcher& operator=(const PageFetcher&) = delete;

    /// Starts fetching page `address` of `source`. A set `cancel` ends the fetch with an error and no
    /// bytes. Its outcome comes out of a later Finished exactly once, also when its read or decode is
    /// dropped without running (the reader service stopped, the job pool shut down).
    void Fetch(const PageSource& source, const PageAddress& address, std::shared_ptr<std::atomic<bool>> cancel,
               const PageFetchTag& tag);
    /// Moves the fetches that ended since the last call into `finished`, which it clears first.
    void Finished(std::vector<FinishedPageFetch>& finished);
    /// Gives back the bytes of a dropped page that came from this fetcher, for a later store page of
    /// the same size to decode into. The bytes go when the spares are full or another owner holds them.
    void Reuse(std::shared_ptr<const std::vector<uint8>> bytes);

private:
    struct Shared;

    JobSystem::WorkStealingThreadPool* m_Pool = nullptr;
    AssetIOService* m_Io = nullptr;
    std::shared_ptr<Shared> m_Shared;
};

} // namespace GameEngine::PageStreaming
