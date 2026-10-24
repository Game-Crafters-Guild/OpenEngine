#include "TerrainECS/CpuHeightPageCache.h"

#include "PageStreaming/HeightPageOverlay.h"

#include "Logger/Logger.h"

#include <algorithm>

namespace GameEngine::TerrainECS
{

namespace
{

using PageStreaming::PageAddress;

constexpr uint64 kPageBytes = static_cast<uint64>(PageStreaming::kPageSampleCount) * sizeof(float32);

// A page's key in its stream: face, level, column and row (24 bits each holds the most pages an
// axis has), so a level's pages sort together.
uint64 PageKey(const PageAddress& address)
{
    return (static_cast<uint64>(address.Face) << 56) | (static_cast<uint64>(address.Level) << 48) |
           (static_cast<uint64>(address.X) << 24) | static_cast<uint64>(address.Z);
}

PageAddress AddressOfKey(uint64 key)
{
    return PageAddress{static_cast<uint8>(key >> 56), static_cast<uint8>(key >> 48),
                       static_cast<uint32>(key >> 24) & 0xFFFFFFu, static_cast<uint32>(key) & 0xFFFFFFu};
}

} // namespace

CpuHeightPageCache::CpuHeightPageCache(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io,
                                       const CpuHeightPageBudget& budget, CpuHeightPageClock clock)
    : m_Pool(pool)
    , m_Io(io)
    , m_Budget(budget)
    , m_Clock(std::move(clock))
{
}

CpuHeightPageCache::~CpuHeightPageCache()
{
    for (auto& [id, stream] : m_Streams)
        stream.Cancel->store(true);
}

void CpuHeightPageCache::Configure(uint32 stream, PageStreaming::PageSource source,
                                   std::span<const PageStreaming::PageStoreLevel> levels, uint32 firstPinnedLevel,
                                   std::shared_ptr<const PageStreaming::HeightPageOverlay> overlay)
{
    Forget(stream);
    Stream& configured = m_Streams.GetOrInsert(stream);
    configured.Source = std::move(source);
    configured.Levels.assign(levels.begin(), levels.end());
    configured.FirstPinnedLevel = firstPinnedLevel;
    configured.Overlay = std::move(overlay);
    configured.Epoch = m_NextEpoch++;
    // The top page is fetched synchronously by the Update; the rest of the pinned levels, coarsest
    // first, on jobs.
    for (uint32 level = static_cast<uint32>(levels.size()); level-- > firstPinnedLevel;)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
                configured.PinnedPending.push_back(PageAddress{0, static_cast<uint8>(level), x, z});
}

void CpuHeightPageCache::Forget(uint32 stream)
{
    Stream* forgotten = m_Streams.Find(stream);
    if (!forgotten)
        return;
    forgotten->Cancel->store(true);
    for (const auto& [key, page] : forgotten->Pages)
        if (page.State == CpuPageState::Resident)
            m_Bytes -= kPageBytes;
    m_Streams.Erase(stream);
}

void CpuHeightPageCache::Refresh(uint32 stream, std::shared_ptr<const PageStreaming::HeightPageOverlay> overlay,
                                 const PageStreaming::PageSampleRect& level0Rect)
{
    Stream* refreshed = m_Streams.Find(stream);
    if (!refreshed)
        return;
    refreshed->Overlay = std::move(overlay);
    for (auto& [key, page] : refreshed->Pages)
    {
        if (page.State == CpuPageState::Resident && PageStreaming::PageReadsLevel0Rect(AddressOfKey(key), level0Rect))
        {
            page.Stale = true;
            refreshed->AnyStale = true;
        }
    }
}

void CpuHeightPageCache::Want(uint32 stream, std::span<const PageAddress> windowPages)
{
    Stream* wanted = m_Streams.Find(stream);
    if (!wanted)
        return;
    // Windows overlap (a cluster of bodies, a body beside the camera): each page once, at its first place.
    m_WantOrder.clear();
    for (uint32 index = 0; index < windowPages.size(); ++index)
        m_WantOrder.push_back({PageKey(windowPages[index]), index});
    std::sort(m_WantOrder.begin(), m_WantOrder.end());
    m_WantFirst.clear();
    for (std::size_t place = 0; place < m_WantOrder.size(); ++place)
        if (place == 0 || m_WantOrder[place].first != m_WantOrder[place - 1].first)
            m_WantFirst.push_back(m_WantOrder[place].second);
    std::sort(m_WantFirst.begin(), m_WantFirst.end());
    wanted->Window.clear();
    for (uint32 index : m_WantFirst)
        wanted->Window.push_back(windowPages[index]);
}

std::span<const PageStreaming::PageStoreLevel> CpuHeightPageCache::Levels(uint32 stream) const
{
    const Stream* found = m_Streams.Find(stream);
    if (!found)
        return {};
    return found->Levels;
}

CpuPageState CpuHeightPageCache::FindPage(uint32 stream, const PageAddress& address,
                                          std::span<const float32>& outSamples) const
{
    const Stream* found = m_Streams.Find(stream);
    const Page* page = found ? found->Pages.Find(PageKey(address)) : nullptr;
    if (!page)
        return CpuPageState::Missing;
    if (page->State == CpuPageState::Resident)
        outSamples = page->Samples;
    return page->State;
}

void CpuHeightPageCache::Update(uint64 frameIndex)
{
    const std::chrono::steady_clock::time_point start = m_Clock();
    DrainCompletions(frameIndex);
    for (auto& [id, stream] : m_Streams)
    {
        FetchTopPage(stream, frameIndex);
        for (const PageAddress& address : stream.Window)
            if (Page* page = stream.Pages.Find(PageKey(address)))
                page->LastWantedFrame = frameIndex;
    }
    FetchWindows(frameIndex, start);
    FetchRemaining(frameIndex);
    TrimToBudget(frameIndex);
}

void CpuHeightPageCache::DrainCompletions(uint64 frameIndex)
{
    m_Drained.clear();
    {
        std::lock_guard lock(m_Completions->Mutex);
        m_Drained.swap(m_Completions->Completions);
    }
    for (Completion& done : m_Drained)
    {
        --m_InFlight; // every fetch started is counted out once, its stream alive or not
        Stream* stream = m_Streams.Find(done.Stream);
        if (!stream || stream->Epoch != done.Epoch)
            continue; // a fetch of a stream since forgotten or restarted
        Land(*stream, frameIndex, done.Outcome);
    }
}

void CpuHeightPageCache::Land(Stream& stream, uint64 frameIndex, PageStreaming::PageFetchOutcome& outcome)
{
    Page& page = stream.Pages.GetOrInsert(PageKey(outcome.Address));
    page.InFlight = false;
    if (outcome.Samples)
    {
        if (page.State != CpuPageState::Resident)
            m_Bytes += kPageBytes;
        page.Samples.assign(outcome.Samples->begin(), outcome.Samples->end());
        if (stream.Overlay)
            stream.Overlay->ApplyToPage(outcome.Address, page.Samples);
        page.State = CpuPageState::Resident;
        page.Stale = false;
        page.FailedAttempts = 0;
        return;
    }
    if (outcome.Absent)
    {
        if (page.State == CpuPageState::Resident)
            m_Bytes -= kPageBytes;
        page.Samples = {};
        page.State = CpuPageState::Absent;
        page.Stale = false;
        return;
    }
    // A failed fetch: a stale page keeps its old samples; either is tried again after a backoff.
    if (page.FailedAttempts == 0)
        Logger::Log::Warning("Terrain CPU height page (level {}, {}, {}) did not load: {}. It is retried.",
                             outcome.Address.Level, outcome.Address.X, outcome.Address.Z, outcome.Error);
    page.RetryAtFrame = PageStreaming::NextPageFetchRetryFrame(frameIndex, page.FailedAttempts);
    ++page.FailedAttempts;
}

bool CpuHeightPageCache::NeedsFetch(const Page* page, uint64 frameIndex) const
{
    if (!page)
        return true;
    if (page->InFlight || frameIndex < page->RetryAtFrame)
        return false;
    return page->State == CpuPageState::Missing || page->Stale;
}

void CpuHeightPageCache::FetchNow(Stream& stream, uint64 frameIndex, const PageAddress& address)
{
    stream.Pages.GetOrInsert(PageKey(address)).LastWantedFrame = frameIndex;
    PageStreaming::PageFetchOutcome outcome = PageStreaming::FetchPage(stream.Source, address);
    Land(stream, frameIndex, outcome);
}

void CpuHeightPageCache::FetchLater(uint32 streamId, Stream& stream, uint64 frameIndex, const PageAddress& address)
{
    Page& page = stream.Pages.GetOrInsert(PageKey(address));
    page.InFlight = true;
    page.LastWantedFrame = std::max(page.LastWantedFrame, frameIndex);
    ++m_InFlight;
    const std::shared_ptr<CompletionQueue> queue = m_Completions;
    PageStreaming::SubmitPageFetch(m_Pool, m_Io, stream.Source, address, stream.Cancel,
                                   [queue, streamId, epoch = stream.Epoch](PageStreaming::PageFetchOutcome outcome) {
                                       std::lock_guard lock(queue->Mutex);
                                       queue->Completions.push_back(Completion{streamId, epoch, std::move(outcome)});
                                   });
}

void CpuHeightPageCache::FetchTopPage(Stream& stream, uint64 frameIndex)
{
    if (stream.Levels.empty())
        return;
    const uint32 top = static_cast<uint32>(stream.Levels.size()) - 1u;
    for (uint32 z = 0; z < stream.Levels[top].PagesZ; ++z)
        for (uint32 x = 0; x < stream.Levels[top].PagesX; ++x)
        {
            const PageAddress address{0, static_cast<uint8>(top), x, z};
            if (NeedsFetch(stream.Pages.Find(PageKey(address)), frameIndex))
                FetchNow(stream, frameIndex, address);
        }
}

void CpuHeightPageCache::FetchWindows(uint64 frameIndex, std::chrono::steady_clock::time_point start)
{
    // Every stream's first window page, then every stream's second, and so on: a second terrain
    // shares the frame's synchronous fetches with the first.
    std::size_t longest = 0;
    for (const auto& [id, stream] : m_Streams)
        longest = std::max(longest, stream.Window.size());
    uint32 fetched = 0;
    for (std::size_t rank = 0; rank < longest; ++rank)
        for (auto& [id, stream] : m_Streams)
        {
            if (rank >= stream.Window.size())
                continue;
            const PageAddress& address = stream.Window[rank];
            if (!NeedsFetch(stream.Pages.Find(PageKey(address)), frameIndex))
                continue;
            if (fetched >= m_Budget.SyncFetchPages || m_Clock() - start >= kCpuSyncFetchDeadline)
                return;
            FetchNow(stream, frameIndex, address);
            ++fetched;
        }
}

void CpuHeightPageCache::FetchRemaining(uint64 frameIndex)
{
    for (auto& [id, stream] : m_Streams)
    {
        for (const PageAddress& address : stream.Window)
            if (m_InFlight < m_Budget.FetchesInFlight && NeedsFetch(stream.Pages.Find(PageKey(address)), frameIndex))
                FetchLater(id, stream, frameIndex, address);

        std::erase_if(stream.PinnedPending, [&stream](const PageAddress& address) {
            const Page* page = stream.Pages.Find(PageKey(address));
            return page && page->State != CpuPageState::Missing;
        });
        for (const PageAddress& address : stream.PinnedPending)
            if (m_InFlight < m_Budget.FetchesInFlight && NeedsFetch(stream.Pages.Find(PageKey(address)), frameIndex))
                FetchLater(id, stream, frameIndex, address);

        if (!stream.AnyStale)
            continue;
        bool stale = false;
        for (auto& [key, page] : stream.Pages)
        {
            if (!page.Stale)
                continue;
            stale = true;
            if (m_InFlight < m_Budget.FetchesInFlight && NeedsFetch(&page, frameIndex))
                FetchLater(id, stream, frameIndex, AddressOfKey(key));
        }
        stream.AnyStale = stale;
    }
}

void CpuHeightPageCache::TrimToBudget(uint64 frameIndex)
{
    if (m_Bytes <= m_Budget.Bytes)
        return;
    m_TrimOrder.clear();
    for (auto& [id, stream] : m_Streams)
    {
        // A failed page no longer wanted holds only its retry wait: dropped with the trim.
        stream.Pages.EraseIf([frameIndex](uint64, const Page& page) {
            return page.State == CpuPageState::Missing && !page.InFlight && page.LastWantedFrame < frameIndex;
        });
        for (const auto& [key, page] : stream.Pages)
            if (page.State == CpuPageState::Resident && !page.InFlight && page.LastWantedFrame < frameIndex &&
                AddressOfKey(key).Level < stream.FirstPinnedLevel)
                m_TrimOrder.push_back(TrimCandidate{page.LastWantedFrame, id, key});
    }
    std::sort(m_TrimOrder.begin(), m_TrimOrder.end(), [](const TrimCandidate& a, const TrimCandidate& b) {
        return a.LastWantedFrame < b.LastWantedFrame;
    });
    for (const TrimCandidate& candidate : m_TrimOrder)
    {
        if (m_Bytes <= m_Budget.Bytes)
            break;
        m_Streams.Find(candidate.Stream)->Pages.Erase(candidate.Key);
        m_Bytes -= kPageBytes;
    }
}

} // namespace GameEngine::TerrainECS
