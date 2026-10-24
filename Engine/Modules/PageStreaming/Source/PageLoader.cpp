#include "PageStreaming/PageLoader.h"

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/HeightPageCodec.h"
#include "PageStreaming/PageStoreFormat.h"
#include "PageStreaming/PageStoreReader.h"

#include "AssetCore/Asset.h"
#include "Assets/AssetIOService.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>

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

    void Load(std::vector<float32> samples)
    {
        if (!m_Resolved.exchange(true))
            m_Request.OnLoaded(m_Request.Address, std::move(samples));
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

// The decode continuation: the page's bytes to samples. A page is not an asset, so the
// continuation's own result is always null.
SharedPtr<Asset> DecodeLoadedPage(PendingPageLoad& load, Vector<uint8> bytes)
{
    if (load.IsCancelled())
    {
        load.Fail("the page load was cancelled");
        return nullptr;
    }
    std::vector<float32> samples(kPageSampleCount);
    DecodeHeightPage(load.Request().Store->Layout().Header, bytes, samples);
    load.Load(std::move(samples));
    return nullptr;
}

} // namespace

void SubmitHeightPageLoad(AssetIOService& io, PageLoadRequest request)
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
    read.RangeBytes = entry->Size;
    read.CancelRequested = pending.Cancel;
    read.ProcessData = [load](Vector<uint8> bytes) { return DecodeLoadedPage(*load, std::move(bytes)); };
    read.OnFailure = [load](const String& error) { load->Fail(error); };
    io.SubmitRead(std::move(read));
}

uint64 NextPageFetchRetryFrame(uint64 frameIndex, uint32 failedAttempts)
{
    constexpr uint32 kMaxRetryFrames = 64;
    return frameIndex + std::min(kMaxRetryFrames, 1u << std::min(failedAttempts, 6u));
}

PageFetchOutcome FetchPage(const PageSource& source, const PageAddress& address)
{
    PageFetchOutcome outcome;
    outcome.Address = address;
    if (source.Store && !source.Store->Layout().FindPresent(address))
    {
        outcome.Absent = true;
        return outcome;
    }
    auto samples = std::make_shared<std::vector<float32>>(kPageSampleCount);
    if (source.Generated)
    {
        if (source.Generated->FillPage(address, *samples))
            outcome.Samples = std::move(samples);
        else
            outcome.Absent = true; // outside the generated pyramid
        return outcome;
    }
    if (source.Store && source.Store->ReadHeightPage(address, *samples))
        outcome.Samples = std::move(samples);
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
        request.OnLoaded = [shared](const PageAddress& page, std::vector<float32> samples) {
            (*shared)(PageFetchOutcome{page, std::make_shared<const std::vector<float32>>(std::move(samples)), {}, false});
        };
        request.OnFailed = [shared, store = source.Store](const PageAddress& page, const std::string& error) {
            (*shared)(PageFetchOutcome{page, nullptr, error, !store->Layout().FindPresent(page)});
        };
        SubmitHeightPageLoad(*io, std::move(request));
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
