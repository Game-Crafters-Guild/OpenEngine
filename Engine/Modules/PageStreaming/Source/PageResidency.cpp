#include "PageStreaming/PageResidency.h"

#include "PageStreaming/HeightPageOverlay.h"

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageLoader.h"
#include "PageStreaming/PageStore/PageStoreAccess.h"
#include "PageStreaming/PageStore/PageStoreReader.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::PageStreaming
{
namespace
{

// The changes a stream's CPU page directory records (48 KB of addresses): many frames of landings
// for a reader that reads the record every frame.
constexpr std::size_t kCpuChangeRecordLength = 4096;

std::span<const PageStoreLevel> LevelsOf(const PageSource& source)
{
    if (source.Store)
        return source.Store->Layout().Levels;
    if (source.Access)
        return source.Access->Levels();
    if (source.Generated)
        return source.Generated->Levels();
    return {};
}

uint32 FaceCountOf(const PageSource& source)
{
    if (source.Store)
        return source.Store->Layout().Header.FaceCount;
    if (source.Access)
        return source.Access->FaceCount();
    return source.Generated ? source.Generated->FaceCount() : 1u;
}

uint32 TopLevelOf(const PageSource& source)
{
    const std::span<const PageStoreLevel> levels = LevelsOf(source);
    return levels.empty() ? 0u : static_cast<uint32>(levels.size()) - 1u;
}

// A page's sort key in its stream: face, level, column and row, so equal pages sort together.
uint64 PageKey(const PageAddress& address)
{
    return (static_cast<uint64>(address.Face) << 56) | (static_cast<uint64>(address.Level) << 48) |
           (static_cast<uint64>(address.X) << 24) | static_cast<uint64>(address.Z);
}

// The one order of wanted pages: pinned first, then nearest, then coarsest (a parent's footprint
// holds its child's, so it is never farther, and the level breaks the tie).
bool LoadsBefore(const PageWant& a, const PageWant& b)
{
    if (a.Pinned != b.Pinned)
        return a.Pinned;
    if (a.Distance != b.Distance)
        return a.Distance < b.Distance;
    return a.Address.Level > b.Address.Level;
}

} // namespace

PageResidencyManager::PageResidencyManager(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io,
                                           const PageResidencyBudget& budget, PageFetchClock clock)
    : m_Pool(pool)
    , m_Io(io)
    , m_Budget(budget)
    , m_Clock(std::move(clock))
{
}

PageResidencyManager::~PageResidencyManager()
{
    for (auto& [id, stream] : m_Streams)
        stream.Cancel->store(true);
}

void PageResidencyManager::ConfigureField(uint32 field, uint32 slotCount, uint32 framesInFlight, float32 fadeSeconds)
{
    m_Fields[field].Configure(slotCount, framesInFlight, fadeSeconds);
    m_CandidatesStale = true; // what each field can place changed
}

void PageResidencyManager::Configure(uint32 stream, uint32 field, PageSource source)
{
    assert(m_Fields.count(field) && "a stream's field is configured before the stream");
    Forget(stream);
    Stream& state = m_Streams[stream];
    state.Source = std::move(source);
    state.Field = field;
    state.TopLevel = TopLevelOf(state.Source);
    state.PageBytes = state.Source.PayloadBytes();
    state.Epoch = m_NextEpoch++;
    state.SourceVersion = state.Source.Access ? state.Source.Access->Version() : 0u;
    state.EntriesPerFace = static_cast<uint32>(DensePageCount(LevelsOf(state.Source)));
    // The CPU directory is built by the stream's first RequestCpu: a stream with no CPU reader holds none.
    state.CpuDirectory.clear();
    state.CpuChanges.assign(kCpuChangeRecordLength, PageAddress{});
    state.KeptCpuWindow.clear(); // the old source's window
}

void PageResidencyManager::Forget(uint32 stream)
{
    const auto it = m_Streams.find(stream);
    if (it == m_Streams.end())
        return;
    // Its outstanding loads resolve as cancelled and are counted out of the budget as they drain.
    it->second.Cancel->store(true);
    m_CpuBytes -= static_cast<uint64>(it->second.Loaded.size()) * it->second.PageBytes;
    if (const auto field = m_Fields.find(it->second.Field); field != m_Fields.end())
        field->second.ReleaseStream(stream);
    m_Streams.erase(it);
    m_CandidatesStale = true; // the candidate list names the forgotten stream
}

void PageResidencyManager::Request(uint32 stream, std::span<const PageWant> wanted)
{
    const auto it = m_Streams.find(stream);
    assert(it != m_Streams.end() && "Request on a stream that was not configured");
    if (it == m_Streams.end())
        return;
    m_Stamped.assign(wanted.begin(), wanted.end());
    for (PageWant& want : m_Stamped)
    {
        want.Stream = stream;
        want.Top = want.Address.Level >= it->second.TopLevel;
    }
    if (m_Stamped == it->second.Wanted)
        return;
    it->second.Wanted.swap(m_Stamped);
    it->second.WantedChanged = true;
}

void PageResidencyManager::RequestCpu(uint32 stream, uint32 firstPinnedLevel, std::span<const PageAddress> windowPages)
{
    const auto it = m_Streams.find(stream);
    assert(it != m_Streams.end() && "RequestCpu on a stream that was not configured");
    if (it == m_Streams.end())
        return;
    Stream& state = it->second;
    // A CPU read runs on the frame thread, and a read of a store being baked waits on the bake.
    assert(!state.Source.Access && "RequestCpu on a stream over a PageStoreAccess: read its pages through Request, on workers");
    if (state.Source.Access)
        return;
    // The CPU readers (SurfaceHeightQuery) read planar terrain: the pins and fetches walk face 0 only.
    assert(FaceCountOf(state.Source) == 1u && "CPU height pages of a stream with more than one face");
    if (state.CpuDirectory.empty())
        BuildCpuDirectory(state);
    if (state.CpuPinnedLevel != firstPinnedLevel)
    {
        // The top page is fetched synchronously by the Update; the rest of the pinned levels,
        // coarsest first, load like the wanted pages.
        state.CpuPinnedLevel = firstPinnedLevel;
        state.CpuPinnedPending.clear();
        const std::span<const PageStoreLevel> levels = LevelsOf(state.Source);
        for (uint32 level = static_cast<uint32>(levels.size()); level-- > firstPinnedLevel;)
            for (uint32 z = 0; z < levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < levels[level].PagesX; ++x)
                    state.CpuPinnedPending.push_back(PageAddress{0, static_cast<uint8>(level), x, z});
    }
    // Each page once, at its first place.
    m_CpuOrder.clear();
    for (uint32 place = 0; place < windowPages.size(); ++place)
        m_CpuOrder.emplace_back(PageKey(windowPages[place]), place);
    std::sort(m_CpuOrder.begin(), m_CpuOrder.end());
    m_CpuFirst.clear();
    for (std::size_t index = 0; index < m_CpuOrder.size(); ++index)
        if (index == 0 || m_CpuOrder[index].first != m_CpuOrder[index - 1].first)
            m_CpuFirst.push_back(m_CpuOrder[index].second);
    std::sort(m_CpuFirst.begin(), m_CpuFirst.end());
    state.CpuWindow.clear();
    for (uint32 place : m_CpuFirst)
        state.CpuWindow.push_back(windowPages[place]);
}

CpuPageDirectory PageResidencyManager::CpuPages(uint32 stream) const
{
    const auto it = m_Streams.find(stream);
    if (it == m_Streams.end())
        return CpuPageDirectory{};
    // A read before the first RequestCpu gets an empty directory: every page is Missing.
    assert((!it->second.CpuDirectory.empty() || LevelsOf(it->second.Source).empty()) &&
           "CpuPages before the stream's first RequestCpu: request the CPU pages first");
    return DirectoryOf(it->second);
}

std::span<const PageAddress> PageResidencyManager::CpuWindow(uint32 stream) const
{
    const auto it = m_Streams.find(stream);
    return it == m_Streams.end() ? std::span<const PageAddress>{} : std::span<const PageAddress>(it->second.KeptCpuWindow);
}

CpuPageDirectory PageResidencyManager::DirectoryOf(const Stream& stream)
{
    const bool empty = stream.CpuDirectory.empty();
    return CpuPageDirectory{LevelsOf(stream.Source), stream.CpuDirectory, stream.EntriesPerFace,
                            empty ? 0u : stream.Epoch, stream.CpuChanges, empty ? nullptr : &stream.CpuChangeCount,
                            stream.PageBytes};
}

void PageResidencyManager::BuildCpuDirectory(Stream& stream)
{
    const std::span<const PageStoreLevel> levels = LevelsOf(stream.Source);
    assert((levels.empty() || (levels.front().SamplesX <= kCpuDirectoryMaxSamplesPerAxis &&
                               levels.front().SamplesZ <= kCpuDirectoryMaxSamplesPerAxis)) &&
           "a store past the CPU directory's limit is refused before it streams");
    stream.CpuDirectory.assign(static_cast<std::size_t>(FaceCountOf(stream.Source)) * stream.EntriesPerFace, CpuPageEntry{});
    const CpuPageDirectory directory = DirectoryOf(stream);
    for (const auto& [address, page] : stream.Loaded)
        stream.CpuDirectory[directory.Index(address)] = CpuPageEntry{page.Bytes->data(), CpuPageState::Resident};
    for (const PageAddress& address : stream.Absent)
        stream.CpuDirectory[directory.Index(address)] = CpuPageEntry{nullptr, CpuPageState::Absent};
}

void PageResidencyManager::SetCpuEntry(Stream& stream, const PageAddress& address, const CpuPageEntry& entry)
{
    if (stream.CpuDirectory.empty())
        return; // no CPU reader yet: BuildCpuDirectory takes the loaded pages when one asks
    const std::size_t index = DirectoryOf(stream).Index(address);
    assert(index < stream.CpuDirectory.size() && "a page outside its stream's pyramid");
    if (index >= stream.CpuDirectory.size())
        return;
    stream.CpuDirectory[index] = entry;
    // The ring overwrites its oldest change: a reader further behind than the ring answers anew.
    stream.CpuChanges[stream.CpuChangeCount % stream.CpuChanges.size()] = address;
    ++stream.CpuChangeCount;
}

const PageCache& PageResidencyManager::FieldCache(uint32 field) const
{
    return m_Fields.at(field);
}

const std::vector<uint8>* PageResidencyManager::Bytes(uint32 stream, const PageAddress& address) const
{
    const auto it = m_Streams.find(stream);
    if (it == m_Streams.end())
        return nullptr;
    const auto page = it->second.Loaded.find(address);
    return page == it->second.Loaded.end() ? nullptr : page->second.Bytes.get();
}

void PageResidencyManager::DrainCompletions(uint64 frameIndex)
{
    m_Drained.clear();
    {
        std::lock_guard lock(m_Completions->Mutex);
        m_Drained.swap(m_Completions->Completions);
    }
    for (Completion& done : m_Drained)
    {
        --m_InFlightCount; // every load started is counted out once, its stream alive or not
        if (done.Cpu)
            --m_CpuInFlightCount;
        const auto it = m_Streams.find(done.Stream);
        if (it == m_Streams.end() || it->second.Epoch != done.Epoch)
            continue; // a load of a stream since forgotten or restarted
        it->second.InFlight.erase(done.Outcome.Address);
        // Absent at an older version of a store being baked: the page is fetched again.
        if (done.Outcome.Absent && done.SourceVersion != it->second.SourceVersion)
            continue;
        Land(it->second, frameIndex, done.Outcome);
    }
}

void PageResidencyManager::Land(Stream& stream, uint64 frameIndex, PageFetchOutcome& outcome)
{
    if (outcome.Bytes)
    {
        const auto [page, inserted] = stream.Loaded.try_emplace(outcome.Address);
        page->second = LoadedPage{std::move(outcome.Bytes), std::max(page->second.LastWantedFrame, frameIndex)};
        assert(page->second.Bytes->size() == stream.PageBytes && "a loaded page holds its source's page bytes");
        SetCpuEntry(stream, outcome.Address, CpuPageEntry{page->second.Bytes->data(), CpuPageState::Resident});
        stream.Failures.erase(outcome.Address);
        if (inserted)
            m_CpuBytes += stream.PageBytes;
        return;
    }
    if (outcome.Absent)
    {
        stream.Absent.insert(outcome.Address);
        SetCpuEntry(stream, outcome.Address, CpuPageEntry{nullptr, CpuPageState::Absent});
        return;
    }
    LoadFailure& failure = stream.Failures[outcome.Address];
    if (failure.Attempts == 0)
        Logger::Log::Warning("Terrain page (level {}, {}, {}) did not load: {}. It is retried.", outcome.Address.Level,
                             outcome.Address.X, outcome.Address.Z, outcome.Error);
    failure.RetryAtFrame = NextPageFetchRetryFrame(frameIndex, failure.Attempts);
    ++failure.Attempts;
}

bool PageResidencyManager::NeedsLoad(const Stream& stream, const PageAddress& address, uint64 frameIndex) const
{
    if (stream.Loaded.count(address) || stream.InFlight.count(address) || stream.Absent.count(address))
        return false;
    const auto failed = stream.Failures.find(address);
    return failed == stream.Failures.end() || frameIndex >= failed->second.RetryAtFrame;
}

void PageResidencyManager::RetryAbsentPages()
{
    for (auto& [id, stream] : m_Streams)
    {
        if (!stream.Source.Access)
            continue;
        const uint64 version = stream.Source.Access->Version();
        if (version == stream.SourceVersion)
            continue;
        stream.SourceVersion = version;
        stream.Absent.clear();
    }
}

void PageResidencyManager::FetchNow(Stream& stream, uint64 frameIndex, const PageAddress& address)
{
    assert(!stream.Source.Access && "a store being baked is read on workers: a read here waits on the bake");
    PageFetchOutcome outcome = FetchPage(stream.Source, address);
    Land(stream, frameIndex, outcome);
}

bool PageResidencyManager::FetchCpuPage(uint32 streamId, const PageAddress& address)
{
    const auto found = m_Streams.find(streamId);
    if (found == m_Streams.end())
        return false;
    Stream& stream = found->second;
    if (NeedsLoad(stream, address, m_FrameIndex))
        FetchNow(stream, m_FrameIndex, address);
    return stream.Loaded.count(address) != 0;
}

void PageResidencyManager::FetchCpuNow(uint64 frameIndex, std::chrono::steady_clock::time_point start)
{
    // The top pages of every stream with CPU readers, whatever the clock says: every walk ends on one.
    std::size_t longest = 0;
    for (auto& [id, stream] : m_Streams)
    {
        if (stream.CpuPinnedLevel == ~0u)
            continue;
        const std::span<const PageStoreLevel> levels = LevelsOf(stream.Source);
        if (!levels.empty())
            for (uint32 z = 0; z < levels.back().PagesZ; ++z)
                for (uint32 x = 0; x < levels.back().PagesX; ++x)
                {
                    const PageAddress address{0, static_cast<uint8>(stream.TopLevel), x, z};
                    if (NeedsLoad(stream, address, frameIndex))
                        FetchNow(stream, frameIndex, address);
                }
        for (const PageAddress& address : stream.CpuWindow)
            if (const auto page = stream.Loaded.find(address); page != stream.Loaded.end())
                page->second.LastWantedFrame = frameIndex;
        longest = std::max(longest, stream.CpuWindow.size());
    }
    // Every stream's first window page, then every stream's second, and so on: a second terrain
    // shares the frame's synchronous fetches with the first. The first fetch starts while the clock
    // is under the deadline; each later one only when the last fetch's cost, added to the time spent,
    // still ends under it (a generated page takes about 0.36 ms, so one a frame; a store page read
    // from the file cache about 8 us, so the page cap).
    uint32 fetched = 0;
    std::chrono::steady_clock::time_point lastStart{};
    for (std::size_t rank = 0; rank < longest; ++rank)
        for (auto& [id, stream] : m_Streams)
        {
            if (rank >= stream.CpuWindow.size() || !NeedsLoad(stream, stream.CpuWindow[rank], frameIndex))
                continue;
            const std::chrono::steady_clock::time_point now = m_Clock();
            const std::chrono::steady_clock::duration lastCost =
                fetched == 0 ? std::chrono::steady_clock::duration::zero() : now - lastStart;
            if (fetched >= m_Budget.CpuSyncFetchPages || now - start + lastCost >= kCpuSyncFetchDeadline)
                return;
            lastStart = now;
            FetchNow(stream, frameIndex, stream.CpuWindow[rank]);
            ++fetched;
        }
}

void PageResidencyManager::StartCpuLoads(uint64 frameIndex, bool pinned)
{
    const uint32 share = std::max(1u, m_Budget.LoadsInFlight / 4u);
    for (auto& [id, stream] : m_Streams)
    {
        if (pinned)
            std::erase_if(stream.CpuPinnedPending, [&stream](const PageAddress& address) {
                return stream.Loaded.count(address) != 0 || stream.Absent.count(address) != 0;
            });
        for (const PageAddress& address : pinned ? stream.CpuPinnedPending : stream.CpuWindow)
        {
            if (m_CpuInFlightCount >= share || m_InFlightCount >= m_Budget.LoadsInFlight)
                return;
            if (NeedsLoad(stream, address, frameIndex))
                StartLoad(id, stream, address, true);
        }
    }
}

void PageResidencyManager::RebuildCandidates()
{
    bool changed = m_CandidatesStale;
    m_CandidatesStale = false;
    for (auto& [id, stream] : m_Streams)
    {
        changed = changed || stream.WantedChanged;
        stream.WantedChanged = false;
    }
    if (!changed)
        return; // a parked camera: the order stands
    m_Candidates.clear();
    for (auto& [id, stream] : m_Streams)
    {
        stream.Placeable.clear();
        stream.PlaceableSet.clear();
        for (const PageWant& want : stream.Wanted)
            m_Candidates.push_back(Candidate{id, want});
    }
    std::sort(m_Candidates.begin(), m_Candidates.end(),
              [](const Candidate& a, const Candidate& b) { return LoadsBefore(a.Want, b.Want); });
    // Each field places as many pages as its cache has slots, its streams taking them in the one order.
    for (auto& [field, taken] : m_FieldPlaced)
        taken = 0;
    std::size_t kept = 0;
    for (const Candidate& candidate : m_Candidates)
    {
        Stream& stream = m_Streams.at(candidate.Stream);
        uint32& taken = m_FieldPlaced[stream.Field];
        if (taken >= m_Fields.at(stream.Field).SlotCount())
            continue;
        ++taken;
        stream.Placeable.push_back(candidate.Want);
        stream.PlaceableSet.insert(candidate.Want.Address);
        m_Candidates[kept++] = candidate;
    }
    m_Candidates.resize(kept);
}

void PageResidencyManager::StartLoad(uint32 streamId, Stream& stream, const PageAddress& address, bool cpu)
{
    const std::shared_ptr<CompletionQueue> queue = m_Completions;
    const uint64 epoch = stream.Epoch;
    const uint64 version = stream.SourceVersion;
    stream.InFlight.insert(address);
    ++m_InFlightCount;
    if (cpu)
        ++m_CpuInFlightCount;
    ++m_LoadsStarted;
    SubmitPageFetch(m_Pool, m_Io, stream.Source, address, stream.Cancel,
                    [queue, streamId, epoch, version, cpu](PageFetchOutcome outcome) {
                        std::lock_guard lock(queue->Mutex);
                        queue->Completions.push_back(Completion{streamId, epoch, version, std::move(outcome), cpu});
                    });
}

void PageResidencyManager::Refresh(uint32 streamId, const PageSampleRect& level0Rect)
{
    const auto it = m_Streams.find(streamId);
    if (it == m_Streams.end())
        return;
    Stream& stream = it->second;
    for (const CachedPage& page : m_Fields.at(stream.Field).Resident())
    {
        if (page.Stream != streamId || !PageReadsLevel0Rect(page.Address, level0Rect))
            continue;
        if (PageSampleRect* stale = stream.Stale.Find(page.Address))
            *stale = Union(*stale, level0Rect);
        else
            stream.Stale.InsertOrAssign(page.Address, level0Rect);
    }
}

void PageResidencyManager::StartLoads(uint64 frameIndex)
{
    // A refreshed page is shown with its old content until it is rewritten: reload those first.
    for (auto& [id, stream] : m_Streams)
        for (const auto& [address, level0Rect] : stream.Stale)
        {
            if (m_InFlightCount >= m_Budget.LoadsInFlight)
                return;
            if (!stream.Loaded.count(address) && !stream.InFlight.count(address))
                StartLoad(id, stream, address, false);
        }
    for (const Candidate& candidate : m_Candidates)
    {
        if (m_InFlightCount >= m_Budget.LoadsInFlight)
            return;
        Stream& stream = m_Streams.at(candidate.Stream);
        const PageAddress& address = candidate.Want.Address;
        if (const auto it = stream.Loaded.find(address); it != stream.Loaded.end())
        {
            it->second.LastWantedFrame = frameIndex;
            continue;
        }
        if (m_Fields.at(stream.Field).SlotOf(candidate.Want.Page()) != kNoResidentSlot)
            continue; // resident: its samples were uploaded and trimmed since
        if (NeedsLoad(stream, address, frameIndex))
            StartLoad(candidate.Stream, stream, address, false);
    }
}

void PageResidencyManager::AssignLoaded(uint64 frameIndex, float32 deltaSeconds, bool teleport)
{
    // The upload cap goes to the loaded, placeable, not yet resident pages in the one order.
    uint32 uploads = teleport ? m_Budget.UploadsPerFrameTeleport : m_Budget.UploadsPerFrame;
    for (auto& [field, allowance] : m_Allowance)
        allowance = 0;
    for (const Candidate& candidate : m_Candidates)
    {
        if (uploads == 0)
            break;
        const Stream& stream = m_Streams.at(candidate.Stream);
        if (stream.Loaded.count(candidate.Want.Address) &&
            m_Fields.at(stream.Field).SlotOf(candidate.Want.Page()) == kNoResidentSlot)
        {
            ++m_Allowance[stream.Field];
            --uploads;
        }
    }
    for (auto& [field, cache] : m_Fields)
    {
        m_LoadedPlaceable.clear();
        for (const auto& [id, stream] : m_Streams)
        {
            if (stream.Field != field)
                continue;
            for (const PageWant& want : stream.Placeable)
                if (stream.Loaded.count(want.Address) || cache.SlotOf(want.Page()) != kNoResidentSlot)
                    m_LoadedPlaceable.push_back(want);
        }
        const auto allowance = m_Allowance.find(field);
        cache.Update(frameIndex, deltaSeconds, m_LoadedPlaceable, allowance == m_Allowance.end() ? 0u : allowance->second);
        RewriteRefreshed(field, cache);
    }
}

void PageResidencyManager::RewriteRefreshed(uint32 field, PageCache& cache)
{
    for (auto& entry : m_Streams)
    {
        const uint32 id = entry.first;
        Stream& stream = entry.second;
        if (stream.Field != field)
            continue;
        // A page released meanwhile is dropped (a later assign loads it afresh); a loaded one is
        // rewritten; the rest wait for their load.
        stream.Stale.EraseIf([id, &stream, &cache](const PageAddress& address, const PageSampleRect& level0Rect) {
            const CachedPage cached{id, address};
            return cache.SlotOf(cached) == kNoResidentSlot ||
                   (stream.Loaded.count(address) != 0 && cache.Rewrite(cached, level0Rect));
        });
    }
}

void PageResidencyManager::TrimCpuPages(uint64 frameIndex)
{
    // Kept past the budget: the placeable pages (to upload, or uploaded and wanted again), this
    // frame's CPU window pages (wanted at this frame) and the CPU pinned levels.
    if (m_CpuBytes <= m_Budget.CpuBytes)
        return;
    m_TrimOrder.clear();
    for (const auto& [id, stream] : m_Streams)
        for (const auto& [address, page] : stream.Loaded)
            if (!stream.PlaceableSet.count(address) && page.LastWantedFrame < frameIndex &&
                address.Level < stream.CpuPinnedLevel)
                m_TrimOrder.push_back(TrimCandidate{page.LastWantedFrame, id, address});
    std::sort(m_TrimOrder.begin(), m_TrimOrder.end(), [](const TrimCandidate& a, const TrimCandidate& b) {
        return a.LastWantedFrame < b.LastWantedFrame;
    });
    for (const TrimCandidate& candidate : m_TrimOrder)
    {
        if (m_CpuBytes <= m_Budget.CpuBytes)
            break;
        Stream& stream = m_Streams.at(candidate.Stream);
        stream.Loaded.erase(candidate.Address);
        SetCpuEntry(stream, candidate.Address, CpuPageEntry{});
        m_CpuBytes -= stream.PageBytes;
    }
}

void PageResidencyManager::Update(uint64 frameIndex, float32 deltaSeconds, bool teleport)
{
    const std::chrono::steady_clock::time_point start = m_Clock();
    m_FrameIndex = frameIndex;
    m_LoadsStarted = 0;
    RetryAbsentPages();
    DrainCompletions(frameIndex);
    FetchCpuNow(frameIndex, start);
    RebuildCandidates();
    StartCpuLoads(frameIndex, false);
    StartLoads(frameIndex);
    StartCpuLoads(frameIndex, true);
    AssignLoaded(frameIndex, deltaSeconds, teleport);
    TrimCpuPages(frameIndex);
    for (auto& [id, stream] : m_Streams)
    {
        // Each frame states its windows; the ones this Update kept stay readable until the next.
        stream.KeptCpuWindow.swap(stream.CpuWindow);
        stream.CpuWindow.clear();
    }
}

} // namespace GameEngine::PageStreaming
