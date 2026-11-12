#include "PageStreaming/PageLoader.h"

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageStore/PageStoreAccess.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "PageStreaming/PageStore/PageStoreReader.h"

#include "Bc5PageCodec.h"

#include "AssetCore/Asset.h"
#include "Assets/AssetIOService.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Memory/ObjectPool.h"
#include "Types/ByteBuffer.h"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <string_view>
#include <utility>

namespace GameEngine::PageStreaming
{
namespace
{

struct FetchState;

// One fetch in flight. Its outcome is written once, by the first End or Take.
struct PendingPageFetch
{
    std::shared_ptr<FetchState> State; // keeps the fetcher's state alive while the fetch is in flight
    PageSource Source;                 // a store fetch holds only the store
    PageAddress Address;
    std::shared_ptr<std::atomic<bool>> Cancel;
    PageFetchTag Tag;
    PageFetchOutcome Outcome;
    std::atomic<bool> Ended{false};
    std::atomic<uint32> Holders{0};
};

// The fetches of one fetcher, kept alive by the fetcher and by every fetch in flight.
struct FetchState
{
    std::mutex Mutex;
    std::vector<PendingPageFetch*> Ended;  // under Mutex: the fetches whose last holder let go
    std::vector<PendingPageFetch*> Taking; // the owner's thread: swapped with Ended by Finished
    // Under Mutex: the buffers Reuse gave back, reserved to SpareLimit so a push never allocates.
    std::vector<std::shared_ptr<std::vector<uint8>>> Spares;
    uint32 SpareLimit = 0;
    // New and Delete run on the owner's thread. The pool destroys the fetches nobody took.
    Memory::ObjectPool<PendingPageFetch> Fetches{"PageStreaming.PageFetches"};
};

// A spare buffer that holds `bytes` bytes, or a new empty one.
std::shared_ptr<std::vector<uint8>> TakeSpare(FetchState& state, std::size_t bytes)
{
    std::lock_guard lock(state.Mutex);
    for (std::shared_ptr<std::vector<uint8>>& spare : state.Spares)
        if (spare->size() == bytes)
        {
            std::shared_ptr<std::vector<uint8>> taken = std::move(spare);
            spare = std::move(state.Spares.back());
            state.Spares.pop_back();
            return taken;
        }
    return std::make_shared<std::vector<uint8>>();
}

bool IsSet(const std::shared_ptr<std::atomic<bool>>& cancel)
{
    return cancel && cancel->load(std::memory_order_acquire);
}

// One reference to a fetch in flight, held by each callback that can end it. It is one pointer, so a
// callback that holds only this fits in std::function's own storage. The last holder to go queues the
// fetch for Finished, ended as cancelled if nothing ended it (a decode job dropped without running).
class PageFetchHolder
{
public:
    explicit PageFetchHolder(PendingPageFetch& fetch) noexcept
        : m_Fetch(&fetch)
    {
        m_Fetch->Holders.fetch_add(1, std::memory_order_relaxed);
    }

    PageFetchHolder(const PageFetchHolder& other) noexcept
        : m_Fetch(other.m_Fetch)
    {
        m_Fetch->Holders.fetch_add(1, std::memory_order_relaxed);
    }

    PageFetchHolder& operator=(const PageFetchHolder&) = delete;

    ~PageFetchHolder()
    {
        if (m_Fetch->Holders.fetch_sub(1, std::memory_order_acq_rel) != 1)
            return;
        End("the page fetch was cancelled", false);
        // A queued fetch holds no state: the state may go with the fetcher, and its pool with the fetch.
        const std::shared_ptr<FetchState> state = std::move(m_Fetch->State);
        std::lock_guard lock(state->Mutex);
        state->Ended.push_back(m_Fetch);
    }

    PendingPageFetch& Fetch() const { return *m_Fetch; }

    void End(std::string_view error, bool absent) const
    {
        if (m_Fetch->Ended.exchange(true))
            return;
        m_Fetch->Outcome.Error = error;
        m_Fetch->Outcome.Absent = absent;
    }

    void Take(PageFetchOutcome outcome) const
    {
        if (!m_Fetch->Ended.exchange(true))
            m_Fetch->Outcome = std::move(outcome);
    }

private:
    PendingPageFetch* m_Fetch;
};

// The decode continuation of a store page: its stored bytes to its loaded bytes.
void DecodeStorePage(const PageFetchHolder& holder, const ByteBuffer& bytes)
{
    PendingPageFetch& fetch = holder.Fetch();
    if (IsSet(fetch.Cancel))
    {
        holder.End("the page load was cancelled", false);
        return;
    }
    std::shared_ptr<std::vector<uint8>> payload = TakeSpare(*fetch.State, fetch.Source.PayloadBytes());
    if (!fetch.Source.Store->DecodePayload(fetch.Address, bytes, *payload))
    {
        holder.End("the page's bytes are not its stored size", false);
        return;
    }
    holder.Take(PageFetchOutcome{fetch.Address, std::move(payload), {}, false});
}

// Reads the store page of `holder`'s fetch on the reader threads and decodes it on a Background job.
// A page that is not in the store ends the fetch here.
void ReadStorePage(AssetIOService& io, const PageFetchHolder& holder)
{
    PendingPageFetch& fetch = holder.Fetch();
    const PageStoreReader& store = *fetch.Source.Store;
    const PageIndexEntry* entry = store.Layout().FindPresent(fetch.Address);
    if (!entry)
    {
        holder.End("the page is not in the store", true);
        return;
    }
    AssetIOService::ReadRequest read;
    read.Metadata.Path = store.File();
    read.Priority = AssetLoadPriority::High;
    read.RangeOffset = store.BaseOffset() + entry->Offset;
    read.RangeBytes = store.Layout().StoredBytes(*entry);
    read.CancelRequested = fetch.Cancel;
    read.ProcessData = [holder](ByteBuffer bytes) {
        DecodeStorePage(holder, bytes);
        return SharedPtr<Asset>(); // a page is not an asset
    };
    read.OnFailure = [holder](const String& error) { holder.End(error, false); };
    io.SubmitRead(std::move(read));
}

// A generated or plane page, or a store page without the reader threads, on the calling thread.
void RunFetch(const PageFetchHolder& holder)
{
    PendingPageFetch& fetch = holder.Fetch();
    if (IsSet(fetch.Cancel))
    {
        holder.End("the stream was restarted", false);
        return;
    }
    holder.Take(FetchPage(fetch.Source, fetch.Address));
}

// Page `address` of every plane of `source`, the planes' bytes one after the other. A single plane's
// bytes are returned as read.
PageFetchOutcome FetchPlanes(const PageSource& source, const PageAddress& address)
{
    if (source.Planes.size() == 1u)
        return source.Planes.front()->Fetch(address);
    PageFetchOutcome outcome;
    outcome.Address = address;
    auto payload = std::make_shared<std::vector<uint8>>();
    payload->reserve(source.PayloadBytes());
    for (const std::shared_ptr<const PageStoreAccess>& plane : source.Planes)
    {
        PageFetchOutcome own = plane->Fetch(address);
        if (!own.Bytes)
        {
            own.Address = address;
            return own;
        }
        payload->insert(payload->end(), own.Bytes->begin(), own.Bytes->end());
    }
    outcome.Bytes = std::move(payload);
    return outcome;
}

// Page `address` of `planes`, generated on the calling thread: a plane of texels is filled in its
// place in the payload; a Bc5 plane is filled as its two channels, then encoded to its blocks here.
PageFetchOutcome FetchGeneratedPlanes(const GeneratedPagePlanes& planes, uint32 payloadBytes, const PageAddress& address)
{
    PageFetchOutcome outcome;
    outcome.Address = address;
    auto payload = std::make_shared<std::vector<uint8>>(payloadBytes);
    std::vector<std::vector<uint8>> unencoded(planes.Formats.size());
    std::vector<std::span<uint8>> spans;
    spans.reserve(planes.Formats.size());
    std::size_t offset = 0;
    for (std::size_t plane = 0; plane < planes.Formats.size(); ++plane)
    {
        const PageFieldFormat format = planes.Formats[plane];
        const uint32 stride = PageStrideTexels(format);
        const std::size_t texelBytes = static_cast<std::size_t>(stride) * stride * PageTexelBytes(format);
        if (IsBlockFormat(format))
        {
            unencoded[plane].resize(texelBytes);
            spans.push_back(unencoded[plane]);
        }
        else
            spans.emplace_back(payload->data() + offset, texelBytes);
        offset += PagePayloadBytes(format);
    }
    if (!planes.Fill(address, spans))
    {
        outcome.Absent = true; // outside the generated pyramid
        return outcome;
    }
    const Bc5PageCodec codec;
    EncodedPage encoded;
    offset = 0;
    for (std::size_t plane = 0; plane < planes.Formats.size(); ++plane)
    {
        const PageFieldFormat format = planes.Formats[plane];
        if (IsBlockFormat(format))
        {
            assert(format == PageFieldFormat::Bc5 && "a generated block plane is Bc5");
            codec.Encode(address.Level, unencoded[plane], PageFootprint{}, encoded);
            std::copy(encoded.Bytes.begin(), encoded.Bytes.end(), payload->begin() + static_cast<std::ptrdiff_t>(offset));
        }
        offset += PagePayloadBytes(format);
    }
    outcome.Bytes = std::move(payload);
    return outcome;
}

} // namespace

struct PageFetcher::Shared : FetchState
{
};

PageFetcher::PageFetcher(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io, uint32 loadsInFlight)
    : m_Pool(pool)
    , m_Io(io)
    , m_Shared(std::make_shared<Shared>())
{
    m_Shared->Ended.reserve(loadsInFlight);
    m_Shared->Taking.reserve(loadsInFlight);
    m_Shared->Spares.reserve(loadsInFlight);
    m_Shared->SpareLimit = loadsInFlight;
}

PageFetcher::~PageFetcher() = default;

void PageFetcher::Fetch(const PageSource& source, const PageAddress& address,
                        std::shared_ptr<std::atomic<bool>> cancel, const PageFetchTag& tag)
{
    PendingPageFetch& fetch = *m_Shared->Fetches.New();
    fetch.State = m_Shared;
    fetch.Address = address;
    fetch.Cancel = std::move(cancel);
    fetch.Tag = tag;
    fetch.Outcome.Address = address;
    const PageFetchHolder holder(fetch);
    if (source.Store && m_Io)
    {
        fetch.Source.Store = source.Store;
        ReadStorePage(*m_Io, holder);
        return;
    }
    fetch.Source = source;
    if (m_Pool)
        m_Pool->Submit([holder]() { RunFetch(holder); }, JobSystem::JobPriority::Background);
    else
        RunFetch(holder);
}

void PageFetcher::Finished(std::vector<FinishedPageFetch>& finished)
{
    finished.clear();
    FetchState& state = *m_Shared;
    {
        std::lock_guard lock(state.Mutex);
        state.Taking.swap(state.Ended);
    }
    for (PendingPageFetch* fetch : state.Taking)
    {
        finished.push_back(FinishedPageFetch{fetch->Tag, std::move(fetch->Outcome)});
        state.Fetches.Delete(fetch);
    }
    state.Taking.clear();
}

void PageFetcher::Reuse(std::shared_ptr<const std::vector<uint8>> bytes)
{
    if (!bytes || bytes.use_count() != 1)
        return;
    FetchState& state = *m_Shared;
    std::lock_guard lock(state.Mutex);
    // Every fetch makes its bytes as a mutable vector, so a later decode may write them again.
    if (state.Spares.size() < state.SpareLimit)
        state.Spares.push_back(std::const_pointer_cast<std::vector<uint8>>(std::move(bytes)));
}

uint32 PageSource::PayloadBytes() const
{
    assert((Store || Generated || !Planes.empty() || GeneratedPlanes) &&
           "a page source has a store, a generated provider, planes or generated planes");
    uint32 bytes = 0;
    if (GeneratedPlanes)
    {
        for (PageFieldFormat format : GeneratedPlanes->Formats)
            bytes += PagePayloadBytes(format);
        return bytes;
    }
    if (Planes.empty())
        return PagePayloadBytes(Store ? Store->Layout().Header.Format : PageFieldFormat::HeightSteps);
    for (const std::shared_ptr<const PageStoreAccess>& plane : Planes)
        bytes += PagePayloadBytes(plane->Format());
    return bytes;
}

uint64 PageSource::PlanesVersion() const
{
    uint64 version = 0;
    for (const std::shared_ptr<const PageStoreAccess>& plane : Planes)
        version += plane->Version();
    return version;
}

uint64 NextPageFetchRetryFrame(uint64 frameIndex, uint32 failedAttempts)
{
    constexpr uint32 kMaxRetryFrames = 64;
    return frameIndex + std::min(kMaxRetryFrames, 1u << std::min(failedAttempts, 6u));
}

PageFetchOutcome FetchPage(const PageSource& source, const PageAddress& address)
{
    if (!source.Planes.empty())
        return FetchPlanes(source, address);
    if (source.GeneratedPlanes)
        return FetchGeneratedPlanes(*source.GeneratedPlanes, source.PayloadBytes(), address);
    PageFetchOutcome outcome;
    outcome.Address = address;
    if (source.Store && !source.Store->Layout().FindPresent(address))
    {
        outcome.Absent = true;
        return outcome;
    }
    auto payload = std::make_shared<std::vector<uint8>>();
    if (source.Generated)
    {
        payload->resize(source.PayloadBytes());
        if (source.Generated->FillPage(address, HeightSamples(*payload)))
            outcome.Bytes = std::move(payload);
        else
            outcome.Absent = true; // outside the generated pyramid
        return outcome;
    }
    ByteBuffer stored;
    if (source.Store && source.Store->ReadPageBytes(address, stored) &&
        source.Store->DecodePayload(address, stored, *payload))
        outcome.Bytes = std::move(payload);
    else
        outcome.Error = "the page store could not be read";
    return outcome;
}

} // namespace GameEngine::PageStreaming
