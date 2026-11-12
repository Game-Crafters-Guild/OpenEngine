#include "PageStreaming/PageResidency.h"

#include "PageStreaming/Height/HeightPageLayer.h"

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

// True when `commit` names page `page`: the page is of its level or coarser and reads its rect, with the
// samples up to `marginSamples` past its owned area (PageReadsLevel0Rect).
bool CommitNames(const PageStoreCommit& commit, const PageAddress& page, uint32 marginSamples)
{
    return page.Level >= commit.Level && PageReadsLevel0Rect(page, commit.Level0Rect, marginSamples);
}

// True when one of `commits` names page `page` of a plane stream, which reads its apron.
bool AnyCommitNames(std::span<const PageStoreCommit> commits, const PageAddress& page)
{
    return std::ranges::any_of(commits, [&page](const PageStoreCommit& commit)
                               { return CommitNames(commit, page, kPageApronSamples); });
}

std::span<const PageStoreLevel> LevelsOf(const PageSource& source)
{
    if (source.Store)
        return source.Store->Layout().Levels;
    if (!source.Planes.empty())
        return source.Planes.front()->Levels();
    if (source.GeneratedPlanes)
        return source.GeneratedPlanes->Levels;
    if (source.Generated)
        return source.Generated->Levels();
    return {};
}

// True when every plane of `source` has the faces and the levels of its first plane.
[[maybe_unused]] bool PlanesShareLattice(const PageSource& source)
{
    for (const std::shared_ptr<const PageStoreAccess>& plane : source.Planes)
    {
        const std::span<const PageStoreLevel> own = plane->Levels();
        const std::span<const PageStoreLevel> first = source.Planes.front()->Levels();
        if (plane->FaceCount() != source.Planes.front()->FaceCount() || own.size() != first.size())
            return false;
        for (std::size_t level = 0; level < own.size(); ++level)
            if (own[level].SamplesX != first[level].SamplesX || own[level].SamplesZ != first[level].SamplesZ)
                return false;
    }
    return true;
}

uint32 FaceCountOf(const PageSource& source)
{
    if (source.Store)
        return source.Store->Layout().Header.FaceCount;
    if (!source.Planes.empty())
        return source.Planes.front()->FaceCount();
    if (source.GeneratedPlanes)
        return source.GeneratedPlanes->FaceCount;
    return source.Generated ? source.Generated->FaceCount() : 1u;
}

uint32 TopLevelOf(const PageSource& source)
{
    const std::span<const PageStoreLevel> levels = LevelsOf(source);
    return levels.empty() ? 0u : static_cast<uint32>(levels.size()) - 1u;
}

// True when `address` names a page of `source`'s pyramid.
bool PageInPyramid(const PageSource& source, const PageAddress& address)
{
    const std::span<const PageStoreLevel> levels = LevelsOf(source);
    return address.Face < FaceCountOf(source) && address.Level < levels.size() &&
           address.X < levels[address.Level].PagesX && address.Z < levels[address.Level].PagesZ;
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
    : m_Budget(budget)
    , m_Clock(std::move(clock))
    , m_Fetcher(pool, io, budget.LoadsInFlight)
{
    m_Drained.reserve(budget.LoadsInFlight); // every load in flight can end between two Updates
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
    assert(PlanesShareLattice(source) && "a stream's planes are on one lattice");
    Forget(stream);
    Stream& state = m_Streams[stream];
    state.Source = std::move(source);
    state.Field = field;
    state.TopLevel = TopLevelOf(state.Source);
    state.PageBytes = state.Source.PayloadBytes();
    state.Epoch = m_NextEpoch++;
    state.SourceVersion = state.Source.PlanesVersion();
    state.PlaneVersions.clear();
    for (const std::shared_ptr<const PageStoreAccess>& plane : state.Source.Planes)
        state.PlaneVersions.push_back(plane->Version());
    // The CPU directory is built by the stream's first RequestCpu: a stream with no CPU reader holds none.
    state.CpuDirectory.Clear();
    state.HasCpuDirectory = false;
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
    m_CpuBytes -= static_cast<uint64>(it->second.Loaded.Size()) * it->second.PageBytes;
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
    assert(state.Source.Planes.empty() && "RequestCpu on a stream over a PageStoreAccess: read its pages through Request, on workers");
    if (!state.Source.Planes.empty())
        return;
    // The readers (SurfaceHeightQuery) walk the pyramid through the directory: an entry only for a page
    // the CPU holds or knows is absent. A stream over faces pins no level; its page bodies read by address.
    [[maybe_unused]] const bool multiFace = FaceCountOf(state.Source) > 1u;
    assert((!multiFace || firstPinnedLevel >= LevelsOf(state.Source).size()) && "a stream over faces pins no CPU level");
    if (!state.HasCpuDirectory)
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
    assert((it->second.HasCpuDirectory || LevelsOf(it->second.Source).empty()) &&
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
    if (!stream.HasCpuDirectory)
        return CpuPageDirectory{nullptr, 0u, stream.CpuChanges, nullptr, stream.PageBytes};
    return CpuPageDirectory{&stream.CpuDirectory, stream.Epoch, stream.CpuChanges, &stream.CpuChangeCount,
                            stream.PageBytes};
}

void PageResidencyManager::BuildCpuDirectory(Stream& stream)
{
    // A source with no levels has no page: its directory stays empty.
    stream.HasCpuDirectory = !LevelsOf(stream.Source).empty();
    stream.CpuDirectory.Clear();
    stream.CpuDirectory.Reserve(stream.Loaded.Size() + stream.Absent.size());
    for (const auto& [address, page] : stream.Loaded)
        stream.CpuDirectory.InsertOrAssign(CpuPageDirectory::Key(address), ResidentEntry(page.Bytes->data()));
    for (const PageAddress& address : stream.Absent)
        stream.CpuDirectory.InsertOrAssign(CpuPageDirectory::Key(address), CpuPageEntry::Absent);
}

void PageResidencyManager::SetCpuEntry(Stream& stream, const PageAddress& address, CpuPageEntry entry)
{
    if (!stream.HasCpuDirectory)
        return; // no CPU reader yet: BuildCpuDirectory takes the loaded pages when one asks
    const bool inPyramid = PageInPyramid(stream.Source, address);
    assert(inPyramid && "a page outside its stream's pyramid");
    if (!inPyramid)
        return;
    // A Missing page has no entry.
    if (entry == CpuPageEntry::Missing)
        stream.CpuDirectory.Erase(CpuPageDirectory::Key(address));
    else
        stream.CpuDirectory.InsertOrAssign(CpuPageDirectory::Key(address), entry);
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
    const LoadedPage* page = it->second.Loaded.Find(address);
    return page ? page->Bytes.get() : nullptr;
}

void PageResidencyManager::DrainCompletions(uint64 frameIndex)
{
    m_Fetcher.Finished(m_Drained);
    m_LoadsChanged = m_LoadsChanged || !m_Drained.empty();
    for (FinishedPageFetch& done : m_Drained)
    {
        --m_InFlightCount; // every load started is counted out once, its stream alive or not
        if (done.Tag.Cpu)
            --m_CpuInFlightCount;
        const auto it = m_Streams.find(done.Tag.Stream);
        if (it == m_Streams.end())
            continue; // a load of a stream since forgotten
        if (done.Outcome.Bytes)
            ++it->second.PagesProduced;
        if (it->second.Epoch != done.Tag.Epoch)
            continue; // a load of a stream since restarted
        it->second.InFlight.Erase(done.Outcome.Address);
        // A commit wrote the page after its load began: the page is fetched again.
        if (it->second.OutdatedLoads.Erase(done.Outcome.Address))
            continue;
        // Absent at an older version of a store being baked: the page is fetched again.
        if (done.Outcome.Absent && done.Tag.SourceVersion != it->second.SourceVersion)
            continue;
        Land(it->second, frameIndex, done.Outcome);
    }
}

void PageResidencyManager::Land(Stream& stream, uint64 frameIndex, PageFetchOutcome& outcome)
{
    if (outcome.Bytes)
    {
        const bool inserted = !stream.Loaded.Contains(outcome.Address);
        LoadedPage& page = stream.Loaded.GetOrInsert(outcome.Address);
        if (stream.Source.Store)
            m_Fetcher.Reuse(std::move(page.Bytes)); // a reloaded page's old bytes
        page = LoadedPage{std::move(outcome.Bytes), std::max(page.LastWantedFrame, frameIndex)};
        assert(page.Bytes->size() == stream.PageBytes && "a loaded page holds its source's page bytes");
        SetCpuEntry(stream, outcome.Address, ResidentEntry(page.Bytes->data()));
        stream.Failures.erase(outcome.Address);
        m_ChangedFields.Insert(stream.Field);
        if (inserted)
            m_CpuBytes += stream.PageBytes;
        return;
    }
    if (outcome.Absent)
    {
        stream.Absent.insert(outcome.Address);
        SetCpuEntry(stream, outcome.Address, CpuPageEntry::Absent);
        return;
    }
    LoadFailure& failure = stream.Failures[outcome.Address];
    if (failure.Attempts == 0)
        Logger::Log::Warning("Terrain page (level {}, {}, {}) did not load: {}. It is retried.", outcome.Address.Level,
                             outcome.Address.X, outcome.Address.Z, outcome.Error);
    failure.RetryAtFrame = NextPageFetchRetryFrame(frameIndex, failure.Attempts);
    m_NextRetryFrame = std::min(m_NextRetryFrame, failure.RetryAtFrame);
    ++failure.Attempts;
}

bool PageResidencyManager::NeedsLoad(const Stream& stream, const PageAddress& address, uint64 frameIndex) const
{
    if (stream.Loaded.Contains(address) || stream.InFlight.Contains(address) || stream.Absent.count(address))
        return false;
    const auto failed = stream.Failures.find(address);
    return failed == stream.Failures.end() || frameIndex >= failed->second.RetryAtFrame;
}

void PageResidencyManager::RetryAbsentPages()
{
    for (auto& [id, stream] : m_Streams)
    {
        if (stream.Source.Planes.empty() || stream.Source.PlanesVersion() == stream.SourceVersion)
            continue;
        m_ReadableCommits.clear();
        bool whole = false;
        uint64 reached = 0;
        for (std::size_t plane = 0; plane < stream.Source.Planes.size(); ++plane)
        {
            uint64& version = stream.PlaneVersions[plane];
            version = stream.Source.Planes[plane]->ReadableSince(version, m_ReadableCommits, whole);
            reached += version;
        }
        stream.SourceVersion = reached;
        const std::size_t absent = stream.Absent.size();
        if (whole)
            stream.Absent.clear();
        else if (!m_ReadableCommits.empty())
        {
            m_Visits += static_cast<uint32>(absent);
            std::erase_if(stream.Absent,
                          [this](const PageAddress& page) { return AnyCommitNames(m_ReadableCommits, page); });
        }
        // A commit wrote its pages again: a resident page that the commit names (CommitNames) reloads,
        // of any face, as the absent pages do. A publication changes no page, so `whole` refreshes none.
        for (const PageStoreCommit& commit : m_ReadableCommits)
            MarkStale(id, stream, std::nullopt, commit);
        DropOutdatedLoads(id, stream);
        m_LoadsChanged = m_LoadsChanged || stream.Absent.size() != absent;
    }
}

void PageResidencyManager::DropOutdatedLoads(uint32 streamId, Stream& stream)
{
    if (m_ReadableCommits.empty())
        return;
    m_Visits += static_cast<uint32>(stream.InFlight.Size() + stream.Loaded.Size());
    for (const PageAddress& address : stream.InFlight)
        if (AnyCommitNames(m_ReadableCommits, address))
            stream.OutdatedLoads.Insert(address);
    // A plane stream has no CPU readers: a loaded page that is not resident waits for its upload only.
    const PageCache& cache = m_Fields.at(stream.Field);
    const std::size_t dropped = stream.Loaded.EraseIf([&](const PageAddress& address, const LoadedPage&) {
        return cache.SlotOf(CachedPage{streamId, address}) == kNoResidentSlot && AnyCommitNames(m_ReadableCommits, address);
    });
    m_CpuBytes -= static_cast<uint64>(dropped) * stream.PageBytes;
    m_LoadsChanged = m_LoadsChanged || dropped != 0;
}

void PageResidencyManager::DropUploadedPlaneBytes()
{
    // The owner copied them after that Update. A page released or refreshed since loads again.
    for (const CachedPage& page : m_UploadedPlanePages)
    {
        const auto it = m_Streams.find(page.Stream);
        if (it != m_Streams.end() && it->second.Loaded.Erase(page.Address))
            m_CpuBytes -= it->second.PageBytes;
    }
    m_UploadedPlanePages.clear();
}

void PageResidencyManager::ListUploadedPlanePages()
{
    for (const auto& [field, cache] : m_Fields)
        for (const PageUploadRequest& upload : cache.Uploads())
            if (const PageSource& source = m_Streams.at(upload.Page.Stream).Source; !source.Planes.empty() || source.GeneratedPlanes)
                m_UploadedPlanePages.push_back(upload.Page);
}

void PageResidencyManager::FetchNow(Stream& stream, uint64 frameIndex, const PageAddress& address)
{
    assert(stream.Source.Planes.empty() && "a store being baked is read on workers: a read here waits on the bake");
    PageFetchOutcome outcome = FetchPage(stream.Source, address);
    if (outcome.Bytes)
        ++stream.PagesProduced;
    Land(stream, frameIndex, outcome);
}

uint64 PageResidencyManager::PagesProduced(uint32 stream) const
{
    const auto it = m_Streams.find(stream);
    return it == m_Streams.end() ? 0u : it->second.PagesProduced;
}

bool PageResidencyManager::FetchCpuPage(uint32 streamId, const PageAddress& address)
{
    const auto found = m_Streams.find(streamId);
    if (found == m_Streams.end())
        return false;
    Stream& stream = found->second;
    if (NeedsLoad(stream, address, m_FrameIndex))
        FetchNow(stream, m_FrameIndex, address);
    return stream.Loaded.Contains(address);
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
        if (stream.CpuPinnedLevel < levels.size())
            for (uint32 z = 0; z < levels.back().PagesZ; ++z)
                for (uint32 x = 0; x < levels.back().PagesX; ++x)
                {
                    const PageAddress address{0, static_cast<uint8>(stream.TopLevel), x, z};
                    if (NeedsLoad(stream, address, frameIndex))
                        FetchNow(stream, frameIndex, address);
                }
        for (const PageAddress& address : stream.CpuWindow)
            if (LoadedPage* page = stream.Loaded.Find(address))
                page->LastWantedFrame = frameIndex;
        if (FaceCountOf(stream.Source) == 1u)
            longest = std::max(longest, stream.CpuWindow.size());
    }
    // Planar streams take turns, one window page each, under the deadline: a fetch starts only when the
    // last fetch's cost still ends under it. A planet's window pages load on workers; its page bodies
    // wait for them, so the frame does not.
    uint32 fetched = 0;
    std::chrono::steady_clock::time_point lastStart{};
    for (std::size_t rank = 0; rank < longest; ++rank)
        for (auto& [id, stream] : m_Streams)
        {
            if (rank >= stream.CpuWindow.size() || FaceCountOf(stream.Source) != 1u ||
                !NeedsLoad(stream, stream.CpuWindow[rank], frameIndex))
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
                return stream.Loaded.Contains(address) || stream.Absent.count(address) != 0;
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

void PageResidencyManager::RebuildCandidates(uint64 lastFrame)
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
    m_LoadsChanged = true;
    for (const auto& [field, cache] : m_Fields)
        m_ChangedFields.Insert(field);
    m_Candidates.clear();
    for (auto& [id, stream] : m_Streams)
    {
        // The trim drops the least recently placeable pages first: these were placeable at the last Update.
        m_Visits += static_cast<uint32>(stream.Placeable.size() + stream.Wanted.size());
        for (const PageWant& want : stream.Placeable)
            if (LoadedPage* page = stream.Loaded.Find(want.Address))
                page->LastWantedFrame = lastFrame;
        stream.Placeable.clear();
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
        m_Candidates[kept++] = candidate;
    }
    m_Candidates.resize(kept);
}

void PageResidencyManager::StartLoad(uint32 streamId, Stream& stream, const PageAddress& address, bool cpu)
{
    stream.InFlight.Insert(address);
    ++m_InFlightCount;
    if (cpu)
        ++m_CpuInFlightCount;
    ++m_LoadsStarted;
    m_Fetcher.Fetch(stream.Source, address, stream.Cancel, PageFetchTag{streamId, stream.Epoch, stream.SourceVersion, cpu});
}

void PageResidencyManager::Refresh(uint32 streamId, uint8 face, const PageSampleRect& level0Rect)
{
    const auto it = m_Streams.find(streamId);
    if (it == m_Streams.end())
        return;
    MarkStale(streamId, it->second, face, PageStoreCommit{level0Rect, 0u});
}

void PageResidencyManager::ChangePlanes(uint32 streamId, PageSource source)
{
    const auto it = m_Streams.find(streamId);
    assert(it != m_Streams.end() && "ChangePlanes on a stream that was not configured");
    if (it == m_Streams.end())
        return;
    Stream& stream = it->second;
    assert(!stream.Source.Planes.empty() && !source.Planes.empty() && PlanesShareLattice(source) &&
           "ChangePlanes keeps a stream over planes");
    ReplaceSource(stream, std::move(source));
    const PageStoreLevel& level0 = LevelsOf(stream.Source).front();
    MarkStale(streamId, stream, std::nullopt,
              PageStoreCommit{PageSampleRect{0u, 0u, level0.SamplesX - 1u, level0.SamplesZ - 1u}, 0u});
}

void PageResidencyManager::ChangeGeneratedPlanes(uint32 streamId, PageSource source)
{
    const auto it = m_Streams.find(streamId);
    assert(it != m_Streams.end() && "ChangeGeneratedPlanes on a stream that was not configured");
    if (it == m_Streams.end())
        return;
    assert(it->second.Source.GeneratedPlanes && source.GeneratedPlanes &&
           "ChangeGeneratedPlanes keeps a stream over generated planes");
    ReplaceSource(it->second, std::move(source));
}

void PageResidencyManager::ReplaceSource(Stream& stream, PageSource source)
{
    assert(LevelsOf(source).size() == LevelsOf(stream.Source).size() &&
           FaceCountOf(source) == FaceCountOf(stream.Source) && "a stream changes its source on its lattice");
    // The old source's loads end cancelled and, of an older epoch, never land.
    stream.Cancel->store(true);
    stream.Cancel = std::make_shared<std::atomic<bool>>(false);
    stream.Epoch = m_NextEpoch++;
    stream.InFlight.Clear();
    stream.OutdatedLoads.Clear();
    m_CpuBytes -= static_cast<uint64>(stream.Loaded.Size()) * stream.PageBytes;
    stream.Loaded.Clear();
    stream.Absent.clear();
    stream.Failures.clear();
    stream.Source = std::move(source);
    stream.PageBytes = stream.Source.PayloadBytes();
    stream.SourceVersion = stream.Source.PlanesVersion();
    stream.PlaneVersions.clear();
    for (const std::shared_ptr<const PageStoreAccess>& plane : stream.Source.Planes)
        stream.PlaneVersions.push_back(plane->Version());
    m_LoadsChanged = true;
}

bool PageResidencyManager::Refreshing(uint32 streamId) const
{
    const auto it = m_Streams.find(streamId);
    return it != m_Streams.end() && !it->second.Stale.Empty();
}

void PageResidencyManager::MarkStale(uint32 streamId, Stream& stream, std::optional<uint8> face,
                                     const PageStoreCommit& commit)
{
    const uint32 margin = stream.Source.GeneratedPlanes ? stream.Source.GeneratedPlanes->MarginSamples : kPageApronSamples;
    for (const CachedPage& page : m_Fields.at(stream.Field).Resident())
    {
        if (page.Stream != streamId || (face && page.Address.Face != *face) ||
            !CommitNames(commit, page.Address, margin))
            continue;
        if (PageSampleRect* stale = stream.Stale.Find(page.Address))
            *stale = Union(*stale, commit.Level0Rect);
        else
            stream.Stale.InsertOrAssign(page.Address, commit.Level0Rect);
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
            if (!stream.Loaded.Contains(address) && !stream.InFlight.Contains(address))
                StartLoad(id, stream, address, false);
        }
    // A candidate needs a load only after the order, a load, a residency change or a retry's frame.
    for (const auto& [field, cache] : m_Fields)
        m_LoadsChanged = m_LoadsChanged || !cache.Transitions().empty();
    if (!m_LoadsChanged && frameIndex < m_NextRetryFrame)
        return;
    m_LoadsChanged = false; // a pass cut short by the budget runs again at the next completion
    m_NextRetryFrame = ~0ull;
    for (const Candidate& candidate : m_Candidates)
    {
        if (m_InFlightCount >= m_Budget.LoadsInFlight)
            return;
        ++m_Visits;
        Stream& stream = m_Streams.at(candidate.Stream);
        const PageAddress& address = candidate.Want.Address;
        if (stream.Loaded.Contains(address))
            continue;
        if (m_Fields.at(stream.Field).SlotOf(candidate.Want.Page()) != kNoResidentSlot)
            continue; // resident: its samples were uploaded and trimmed since
        if (NeedsLoad(stream, address, frameIndex))
            StartLoad(candidate.Stream, stream, address, false);
        else if (const auto failed = stream.Failures.find(address);
                 failed != stream.Failures.end() && frameIndex < failed->second.RetryAtFrame)
            m_NextRetryFrame = std::min(m_NextRetryFrame, failed->second.RetryAtFrame);
    }
}

bool PageResidencyManager::FieldChanged(uint32 field, const PageCache& cache) const
{
    return m_ChangedFields.Contains(field) || cache.DeferredThisFrame() != 0 || !cache.Transitions().empty();
}

void PageResidencyManager::AssignLoaded(uint64 frameIndex, float32 deltaSeconds, bool teleport)
{
    // The upload cap goes to the loaded, placeable, not yet resident pages in the one order. A field
    // whose pages did not change since an Update that placed them all has none of them.
    uint32 uploads = teleport ? m_Budget.UploadsPerFrameTeleport : m_Budget.UploadsPerFrame;
    for (auto& [field, allowance] : m_Allowance)
        allowance = 0;
    const bool anyChanged =
        std::ranges::any_of(m_Fields, [this](const auto& entry) { return FieldChanged(entry.first, entry.second); });
    for (const Candidate& candidate : m_Candidates)
    {
        if (uploads == 0 || !anyChanged)
            break;
        ++m_Visits;
        const Stream& stream = m_Streams.at(candidate.Stream);
        if (stream.Loaded.Contains(candidate.Want.Address) &&
            m_Fields.at(stream.Field).SlotOf(candidate.Want.Page()) == kNoResidentSlot)
        {
            ++m_Allowance[stream.Field];
            --uploads;
        }
    }
    for (auto& [field, cache] : m_Fields)
    {
        if (!FieldChanged(field, cache))
        {
            cache.Tick(frameIndex, deltaSeconds);
            RewriteRefreshed(field, cache);
            continue;
        }
        m_LoadedPlaceable.clear();
        for (const auto& [id, stream] : m_Streams)
        {
            if (stream.Field != field)
                continue;
            m_Visits += static_cast<uint32>(stream.Placeable.size());
            for (const PageWant& want : stream.Placeable)
                if (stream.Loaded.Contains(want.Address) || cache.SlotOf(want.Page()) != kNoResidentSlot)
                    m_LoadedPlaceable.push_back(want);
        }
        const auto allowance = m_Allowance.find(field);
        cache.Update(frameIndex, deltaSeconds, m_LoadedPlaceable, allowance == m_Allowance.end() ? 0u : allowance->second);
        RewriteRefreshed(field, cache);
    }
    m_ChangedFields.Clear();
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
                   (stream.Loaded.Contains(address) && cache.Rewrite(cached, level0Rect));
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
    for (auto& [id, stream] : m_Streams)
    {
        // A placeable page is wanted at this frame, as a CPU window page is.
        for (const PageWant& want : stream.Placeable)
            if (LoadedPage* page = stream.Loaded.Find(want.Address))
                page->LastWantedFrame = frameIndex;
        for (const auto& [address, page] : stream.Loaded)
            if (page.LastWantedFrame < frameIndex && address.Level < stream.CpuPinnedLevel)
                m_TrimOrder.push_back(TrimCandidate{page.LastWantedFrame, id, address});
    }
    std::sort(m_TrimOrder.begin(), m_TrimOrder.end(), [](const TrimCandidate& a, const TrimCandidate& b) {
        return a.LastWantedFrame < b.LastWantedFrame;
    });
    for (const TrimCandidate& candidate : m_TrimOrder)
    {
        if (m_CpuBytes <= m_Budget.CpuBytes)
            break;
        Stream& stream = m_Streams.at(candidate.Stream);
        // A store page's bytes go back to the fetcher, the one place that decodes store pages.
        if (LoadedPage* page = stream.Loaded.Find(candidate.Address); page && stream.Source.Store)
            m_Fetcher.Reuse(std::move(page->Bytes));
        stream.Loaded.Erase(candidate.Address);
        SetCpuEntry(stream, candidate.Address, CpuPageEntry::Missing);
        m_CpuBytes -= stream.PageBytes;
    }
}

void PageResidencyManager::Update(uint64 frameIndex, float32 deltaSeconds, bool teleport)
{
    const std::chrono::steady_clock::time_point start = m_Clock();
    const uint64 lastFrame = m_FrameIndex;
    m_FrameIndex = frameIndex;
    m_LoadsStarted = 0;
    m_Visits = 0;
    RetryAbsentPages();
    DropUploadedPlaneBytes();
    DrainCompletions(frameIndex);
    FetchCpuNow(frameIndex, start);
    RebuildCandidates(lastFrame);
    StartCpuLoads(frameIndex, false);
    StartLoads(frameIndex);
    StartCpuLoads(frameIndex, true);
    AssignLoaded(frameIndex, deltaSeconds, teleport);
    ListUploadedPlanePages();
    TrimCpuPages(frameIndex);
    for (auto& [id, stream] : m_Streams)
    {
        // Each frame states its windows; the ones this Update kept stay readable until the next.
        stream.KeptCpuWindow.swap(stream.CpuWindow);
        stream.CpuWindow.clear();
    }
    for (const auto& [field, cache] : m_Fields)
        m_Visits += cache.PageVisitsThisFrame();
}

} // namespace GameEngine::PageStreaming
