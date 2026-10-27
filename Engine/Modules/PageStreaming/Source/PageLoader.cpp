#include "PageStreaming/PageLoader.h"

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageStore/PageStoreAccess.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "PageStreaming/PageStore/PageStoreReader.h"

#include "AssetCore/Asset.h"
#include "Assets/AssetIOService.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::PageStreaming
{
namespace
{

// One load in flight, shared by the read's decode continuation and its failure callback. The
// first of Load and Fail resolves it; a load destroyed unresolved (its decode cancelled without
// running, as a pool shutdown does to queued work) fails as cancelled when the last holder lets
// go, on that holder's thread. So exactly one of OnLoaded and OnFailed runs, whatever happens.
class PendingPageLoad
{
public:
    explicit PendingPageLoad(PageLoadRequest request)
        : m_Request(std::move(request))
    {
    }

    ~PendingPageLoad() { Fail("the page load was cancelled"); }

    PendingPageLoad(const PendingPageLoad&) = delete;
    PendingPageLoad& operator=(const PendingPageLoad&) = delete;

    const PageLoadRequest& Request() const { return m_Request; }

    bool IsCancelled() const { return m_Request.Cancel && m_Request.Cancel->load(std::memory_order_acquire); }

    void Load(std::vector<uint8> payload)
    {
        if (!m_Resolved.exchange(true))
            m_Request.OnLoaded(m_Request.Address, std::move(payload));
    }

    void Fail(const std::string& reason)
    {
        if (!m_Resolved.exchange(true))
            m_Request.OnFailed(m_Request.Address, reason);
    }

private:
    PageLoadRequest m_Request;
    std::atomic<bool> m_Resolved{false};
};

// The decode continuation: the page's stored bytes to its loaded bytes. A page is not an asset, so the
// continuation's own result is always null.
SharedPtr<Asset> DecodeLoadedPage(PendingPageLoad& load, Vector<uint8> bytes)
{
    if (load.IsCancelled())
    {
        load.Fail("the page load was cancelled");
        return nullptr;
    }
    std::vector<uint8> payload;
    if (!load.Request().Store->DecodePayload(load.Request().Address, bytes, payload))
    {
        load.Fail("the page's bytes are not its stored size");
        return nullptr;
    }
    load.Load(std::move(payload));
    return nullptr;
}

} // namespace

void SubmitPageLoad(AssetIOService& io, PageLoadRequest request)
{
    const PageIndexEntry* entry = request.Store ? request.Store->Layout().FindPresent(request.Address) : nullptr;
    if (!entry)
    {
        request.OnFailed(request.Address, "the page is not in the store");
        return;
    }

    auto load = std::make_shared<PendingPageLoad>(std::move(request));
    const PageLoadRequest& pending = load->Request();
    AssetIOService::ReadRequest read;
    read.AssetGuid = pending.StoreAsset;
    read.Metadata.Path = pending.Store->File();
    read.Priority = pending.Priority;
    read.RangeOffset = pending.Store->BaseOffset() + entry->Offset;
    read.RangeBytes = pending.Store->Layout().StoredBytes(*entry);
    read.CancelRequested = pending.Cancel;
    read.ProcessData = [load](Vector<uint8> bytes) { return DecodeLoadedPage(*load, std::move(bytes)); };
    read.OnFailure = [load](const String& error) { load->Fail(error); };
    io.SubmitRead(std::move(read));
}

uint32 PageSource::PayloadBytes() const
{
    assert((Store || Generated || Access) && "a page source has a store, a generated provider or a store access");
    if (Access)
        return PagePayloadBytes(Access->Format());
    return PagePayloadBytes(Store ? Store->Layout().Header.Format : PageFieldFormat::HeightSteps);
}

uint64 NextPageFetchRetryFrame(uint64 frameIndex, uint32 failedAttempts)
{
    constexpr uint32 kMaxRetryFrames = 64;
    return frameIndex + std::min(kMaxRetryFrames, 1u << std::min(failedAttempts, 6u));
}

PageFetchOutcome FetchPage(const PageSource& source, const PageAddress& address)
{
    if (source.Access)
        return source.Access->Fetch(address);
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
    std::vector<uint8> stored;
    if (source.Store && source.Store->ReadPageBytes(address, stored) &&
        source.Store->DecodePayload(address, stored, *payload))
        outcome.Bytes = std::move(payload);
    else
        outcome.Error = "the page store could not be read";
    return outcome;
}

void SubmitPageFetch(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io, const PageSource& source,
                     const PageAddress& address, std::shared_ptr<std::atomic<bool>> cancel,
                     std::function<void(PageFetchOutcome)> done)
{
    if (source.Store && io)
    {
        auto shared = std::make_shared<std::function<void(PageFetchOutcome)>>(std::move(done));
        PageLoadRequest request;
        request.Store = source.Store;
        request.Address = address;
        request.Priority = AssetLoadPriority::High;
        request.Cancel = std::move(cancel);
        request.OnLoaded = [shared](const PageAddress& page, std::vector<uint8> payload) {
            (*shared)(PageFetchOutcome{page, std::make_shared<const std::vector<uint8>>(std::move(payload)), {}, false});
        };
        request.OnFailed = [shared, store = source.Store](const PageAddress& page, const std::string& error) {
            (*shared)(PageFetchOutcome{page, nullptr, error, !store->Layout().FindPresent(page)});
        };
        SubmitPageLoad(*io, std::move(request));
        return;
    }
    auto fetch = [source, address, cancel = std::move(cancel), done = std::move(done)]() {
        if (cancel && cancel->load())
        {
            done(PageFetchOutcome{address, nullptr, "the stream was restarted", false});
            return;
        }
        done(FetchPage(source, address));
    };
    if (pool)
        pool->Submit(std::move(fetch), JobSystem::JobPriority::Background);
    else
        fetch();
}

} // namespace GameEngine::PageStreaming
