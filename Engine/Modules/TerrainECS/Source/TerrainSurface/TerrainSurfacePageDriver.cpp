#include "TerrainECS/TerrainSurface/TerrainSurfacePageDriver.h"

#include "TerrainSurface/TerrainSurfaceBakeQueue.h"

#include "TerrainECS/HeightPageStoreLoader.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/TerrainSurface/TerrainSurfaceBakeInputs.h"
#include "TerrainECS/TerrainSurface/TerrainSurfacePageBaker.h"
#include "TerrainECS/TerrainSurface/TerrainSurfaceStore.h"
#include "TerrainECS/TerrainColorMap/TerrainColorMapBaker.h"
#include "TerrainECS/TerrainHeightPageDriver.h"
#include "TerrainECS/TerrainPages.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainSurface/TerrainSurfaceSessionStore.h"
#include "Components/SceneEntityTag.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "FileSystem/ScopedFileLock.h"
#include "JobSystem/TaskHandle.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStore/PageStoreAccess.h"
#include "PageStreaming/PageStore/PageStoreReader.h"
#include "Types/ByteBuffer.h"
#include "Types/FlatMap.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace GameEngine::TerrainECS
{

namespace
{

using PageStreaming::PageAddress;
using PageStreaming::PageFieldKind;

// The height pages a surface page reads: the terrain's generated lattice or its cooked store.
struct SurfaceHeightSource
{
    std::shared_ptr<const PageStreaming::GeneratedHeightPageProvider> Generated;
    std::shared_ptr<const PageStreaming::PageStoreReader> Store;
    uint64 Key = 0;
};

// What the modifier overlay adds to each height page a job reads: the page's samples, -0.0 where the
// overlay adds nothing, so a base page plus it is the base page plus HeightPageLayer::ApplyToPage
// bit for bit. A page the overlay does not reach has no entry.
using OverlayPages = FlatMap<PageAddress, std::vector<float32>>;

// The height source of `tiled` on its tile lattice, or none (Key 0) while its store cooks or when
// the store is on another lattice. A generated source makes its provider only `withProvider`.
SurfaceHeightSource ResolveHeightSource(TerrainService& service, const TiledTerrainData& tiled,
                                        std::string_view name, bool withProvider)
{
    SurfaceHeightSource source;
    const TiledTerrainBase& base = tiled.Config.Base;
    if (base.Source != Components::TerrainBaseSource::HeightmapAsset)
    {
        const PageStreaming::GeneratedHeightLattice lattice = MakeGeneratedHeightLattice(tiled);
        if (withProvider)
            source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
        source.Key = GeneratedHeightIdentity(lattice);
        return source;
    }
    source.Store = service.GetHeightPageStores().Store(base.HeightmapGuid, base.HeightmapContentVersion, name);
    const uint32 interior = tiled.Config.TileConfig.HeightmapWidth - 1u;
    if (!source.Store || source.Store->Layout().Header.SamplesX != tiled.Config.TilesPerAxisX * interior + 1u ||
        source.Store->Layout().Header.SamplesZ != tiled.Config.TilesPerAxisZ * interior + 1u)
        return {};
    source.Key = source.Store->Layout().Header.Key;
    return source;
}

// The overlay of every page of `read` that `overlay` reaches.
std::shared_ptr<const OverlayPages> MakeOverlayPages(const PageStreaming::HeightPageLayer* overlay,
                                                     std::span<const PageAddress> read)
{
    auto pages = std::make_shared<OverlayPages>();
    if (!overlay)
        return pages;
    std::vector<float32> samples;
    for (const PageAddress& address : read)
    {
        if (pages->Contains(address))
            continue;
        samples.assign(PageStreaming::kPageSampleCount, -0.0f);
        if (overlay->ApplyToPage(address, samples))
            pages->InsertOrAssign(address, std::move(samples));
    }
    return pages;
}

SurfaceHeightPageFetch MakeHeightFetch(const SurfaceHeightSource& source, std::shared_ptr<const OverlayPages> overlay)
{
    return [source, overlay = std::move(overlay)](const PageAddress& address, std::span<float32> samples) {
        if (source.Generated)
        {
            if (!source.Generated->FillPage(address, samples))
                return false;
        }
        else
        {
            ByteBuffer stored;
            std::vector<uint8> payload;
            if (!source.Store->ReadPageBytes(address, stored) ||
                !source.Store->DecodePayload(address, stored, payload))
                return false;
            const std::span<const float32> heights = PageStreaming::HeightSamples(payload);
            if (heights.size() != samples.size())
                return false;
            std::copy(heights.begin(), heights.end(), samples.begin());
        }
        if (const std::vector<float32>* added = overlay->Find(address))
            for (std::size_t sample = 0; sample < samples.size(); ++sample)
                samples[sample] += (*added)[sample];
        return true;
    };
}

// The first pinned level of `tiled`'s surface pyramid: FirstPinnedPageLevel on the surface profile's
// slots, the level the stream pins, at least level 1.
uint32 FirstPinnedSurfaceLevel(const TiledTerrainData& tiled, std::span<const PageStreaming::PageStoreLevel> levels,
                               const SurfacePageProfile& profile)
{
    const float32 texel = tiled.Config.TileWorldSize / static_cast<float32>(tiled.Config.TileConfig.HeightmapWidth - 1u);
    return std::max(1u, FirstPinnedPageLevel(levels, 1u, texel, profile.Slots));
}

// "the grass store" or "the splat, normal and color stores": `noun` for the planes of `planes`
// (SurfacePlaneBit), in kSurfacePlanes order.
std::string NamePlanes(uint8 planes, std::string_view noun)
{
    std::vector<std::string_view> names;
    for (const PageFieldKind plane : kSurfacePlanes)
        if ((planes & SurfacePlaneBit(plane)) != 0u)
            names.push_back(SurfacePlaneName(plane));
    if (names.size() < 2u)
        return std::format("the {} {}", names.empty() ? std::string_view() : names.front(), noun);
    return std::format("the {} and {} {}s", Join(std::span(names).first(names.size() - 1u), ", "), names.back(), noun);
}

// "1 page" or "N pages", for the log lines.
std::string CountPages(std::size_t pages)
{
    return std::format("{} page{}", pages, pages == 1u ? "" : "s");
}

float64 MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<float64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

struct TerrainSurfacePageDriver::PlaneStore
{
    PageFieldKind Plane = PageFieldKind::Splat;
    uint64 Key = 0; // the key in the file name
    // The key of the inputs the next job bakes from. A patch writes it into the store, so an open
    // keeps a patched store only when the key of its file name is the key of the patch.
    uint64 InputsKey = 0;
    std::filesystem::path File;
    // The plane's store for its streams: the same object while the plane has a store, over every run.
    std::shared_ptr<PageStreaming::PageStoreAccess> Access;
    std::unique_ptr<TerrainSurfacePageBaker> Baker;
    bool Failed = false; // the open job could not make the store
    // The streams read the plane. A plane that a grass edit adds shows when its wanted pages are baked.
    bool Shown = true;
    // The store is in the session folder: it is deleted when the terrain leaves the session.
    bool InSession = false;
};

// The stores of the terrains whose scene is not saved: one folder a driver, under the cache, named
// for a GUID that the driver makes. The lock beside the folder tells a sweep that the session lives.
struct TerrainSurfacePageDriver::Session
{
    std::filesystem::path Cache;
    GUID Id;
    std::filesystem::path Directory;
    std::unique_ptr<FileSystem::ScopedFileLock> Lock; // null when the lock was refused
};

// One job of a terrain's stores. It owns or copies all it reads; the main thread reads its results
// only once its handle is done.
struct TerrainSurfacePageDriver::BakeJob
{
    std::shared_ptr<const TerrainSurfaceBakeInputs> Inputs;
    SurfaceHeightSource Source;
    std::shared_ptr<const OverlayPages> Overlay;
    std::vector<std::shared_ptr<PlaneStore>> Planes;
    // The open job: the planes whose stores are deleted, and how the others are kept or made.
    bool Open = false;
    bool Rebuild = false;
    uint32 PinnedLevel = 1;
    std::filesystem::path Directory;
    GUID Scene;
    std::string Tag;
    std::vector<PageFieldKind> Removed;
    std::vector<SurfaceBakePage> Pages; // the pages it bakes, of any level
    // Publishes the stores after its pages: the last job of a run. The open job carries no Finish:
    // CreateStore publishes each store it makes, and a kept store is published as it is.
    bool Finish = false;
    // The stores of the planes the terrain had before, published first.
    std::vector<std::shared_ptr<PlaneStore>> Retired;

    // Results. Present holds the pages that each store of the open holds.
    std::vector<SurfacePlaneLayout> Present;
    std::vector<std::string> Warnings;
    float64 Milliseconds = 0.0;
    std::atomic<bool> Ran{false};
};

struct TerrainSurfacePageDriver::TerrainStores
{
    TiledTerrainHandle Handle;
    GUID Scene;
    std::string Tag;
    uint64 HeightKey = 0;
    TerrainColorGroundWrite Write;
    std::vector<std::shared_ptr<PlaneStore>> Planes;
    // The inputs of the newest open or settle, read by every job started after it.
    std::shared_ptr<const TerrainSurfaceBakeInputs> Inputs;
    SurfaceHeightSource Source;
    TerrainSurfaceBakeQueue Pending; // the pages still to bake
    uint32 PinnedLevel = 1;
    // The job in flight and its handle; null when none.
    std::shared_ptr<BakeJob> Job;
    std::unique_ptr<JobSystem::TaskHandle> Task;
    // The edits since the last settle: the level-0 rect and the planes they changed, or the whole surface.
    std::optional<PageStreaming::PageSampleRect> Unsettled;
    uint8 UnsettledPlanes = 0; // SurfacePlaneBit of each plane
    bool UnsettledWhole = false;
    uint32 QuietFrames = 0;
    // The bake run since Pending was last empty, for its log line.
    std::size_t RunPages = 0;
    uint8 RunPlanes = 0; // bit PageFieldKind
    std::chrono::steady_clock::time_point RunStart;
    float64 RunLongestJobMs = 0.0;
    bool RunQueueLogged = false; // the run logged its queued pages
    // A job left the stores open: a later job patches them, and the last one publishes them.
    bool StoresOpen = false;
    // The open job ended: every plane left holds its pinned levels.
    bool PinnedLevelsBaked = false;
    bool Seen = false;
};

struct TerrainSurfacePageDriver::TerrainFrame
{
    TiledTerrainData& Tiled;
    TerrainService& Service;
    TerrainModifierSystem& System;
    JobSystem::WorkStealingThreadPool* Pool = nullptr;
    const std::filesystem::path& Directory;
    TiledTerrainHandle Handle;
    GUID Scene;
    std::string_view Tag;
    float32 OriginY = 0.0f;
    TerrainColorGroundWrite Write;
    bool InSession = false;
};

namespace
{

// The pages that the store of `plane` at `file` holds; none when it does not open.
SurfacePlaneLayout ReadPresentPages(const std::filesystem::path& file, PageFieldKind plane)
{
    PageStreaming::PageStoreReader reader;
    if (!reader.Open(file, plane).empty())
        return SurfacePlaneLayout{SurfacePlaneBit(plane), {}};
    return SurfacePlaneLayout{SurfacePlaneBit(plane), reader.Layout()};
}

// The addresses of the pages of `pages` that `plane` bakes.
std::vector<PageAddress> PlanePages(std::span<const SurfaceBakePage> pages, PageFieldKind plane)
{
    std::vector<PageAddress> addresses;
    for (const SurfaceBakePage& page : pages)
        if ((page.Planes & SurfacePlaneBit(plane)) != 0u)
            addresses.push_back(page.Address);
    return addresses;
}

// True when `changed` holds every level-0 sample of `tiled`.
bool CoversTheWholeSurface(const TiledTerrainData& tiled, const PageStreaming::PageSampleRect& changed)
{
    const uint32 interior = tiled.Config.TileConfig.HeightmapWidth - 1u;
    return changed.MinX == 0 && changed.MinZ == 0 && changed.Width() >= tiled.Config.TilesPerAxisX * interior + 1u &&
           changed.Height() >= tiled.Config.TilesPerAxisZ * interior + 1u;
}

} // namespace

std::shared_ptr<PageStreaming::PageStoreAccess>
TerrainSurfacePageDriver::KeptAccess(std::span<const std::shared_ptr<PlaneStore>> retired, PageFieldKind plane,
                                     const PageStreaming::PageStoreHeader& shape)
{
    for (const std::shared_ptr<PlaneStore>& store : retired)
        if (store->Plane == plane && store->Access->Format() == shape.Format &&
            store->Access->Levels().front().SamplesX == shape.SamplesX &&
            store->Access->Levels().front().SamplesZ == shape.SamplesZ)
            return store->Access;
    return nullptr;
}

bool TerrainSurfacePageDriver::HoldsCurrentStore(const PlaneStore& store)
{
    PageStreaming::PageStoreReader existing;
    std::error_code missingCache;
    return existing.Open(store.File, store.Plane).empty() && existing.Layout().Header.Key == store.Key &&
           (store.Plane != PageFieldKind::Normal ||
            std::filesystem::exists(SurfaceSamplesCachePath(store.File), missingCache));
}

void TerrainSurfacePageDriver::OpenJobStores(BakeJob& job)
{
    // A retired access that no plane keeps reads no page from now on: it lets its file go before the
    // file is deleted, so that no stream reads a deleted store.
    for (const std::shared_ptr<PlaneStore>& retired : job.Retired)
        if (std::none_of(job.Planes.begin(), job.Planes.end(),
                         [&retired](const std::shared_ptr<PlaneStore>& store) { return store->Access == retired->Access; }))
            retired->Access->Close();
    for (const PageFieldKind plane : job.Removed)
        RemoveSurfaceStores(job.Directory, job.Scene, job.Tag, plane);
    for (const std::shared_ptr<PlaneStore>& store : job.Planes)
    {
        if (!job.Rebuild && HoldsCurrentStore(*store))
        {
            if (std::string error = store->Access->Publish(store->File); !error.empty())
                job.Warnings.push_back(std::string("the ") + SurfacePlaneName(store->Plane) +
                                       " store is not readable: " + error);
            job.Present.push_back(ReadPresentPages(store->File, store->Plane));
            continue;
        }
        RemoveSurfaceStores(job.Directory, job.Scene, job.Tag, store->Plane);
        if (std::string error = store->Baker->CreateStore(store->File, store->Key, job.PinnedLevel); !error.empty())
        {
            job.Warnings.push_back(std::string("the ") + SurfacePlaneName(store->Plane) +
                                   " store was not made: " + error);
            store->Failed = true;
            continue;
        }
        job.Present.push_back(ReadPresentPages(store->File, store->Plane));
    }
    // A terrain that leaves the session (its scene is saved) deletes the session's stores.
    for (const std::shared_ptr<PlaneStore>& retired : job.Retired)
        if (retired->InSession && std::none_of(job.Planes.begin(), job.Planes.end(),
                                               [&retired](const std::shared_ptr<PlaneStore>& store) {
                                                   return store->File == retired->File;
                                               }))
            RemoveSurfaceStore(retired->File);
}

void TerrainSurfacePageDriver::RunJob(BakeJob& job)
{
    const auto start = std::chrono::steady_clock::now();
    for (const std::shared_ptr<PlaneStore>& store : job.Retired)
        if (std::string error = store->Baker->Finish(); !error.empty())
            job.Warnings.push_back(std::string("the previous ") + SurfacePlaneName(store->Plane) +
                                   " store was not published: " + error);
    if (job.Open)
        OpenJobStores(job);
    for (const std::shared_ptr<PlaneStore>& store : job.Planes)
    {
        if (store->Failed)
            continue;
        if (const std::vector<PageAddress> pages = PlanePages(job.Pages, store->Plane); !pages.empty())
            if (std::string error = store->Baker->BakePages(store->File, pages); !error.empty())
                job.Warnings.push_back(std::string("the ") + SurfacePlaneName(store->Plane) +
                                       " store missed pages: " + error);
        if (job.Finish)
            if (std::string error = store->Baker->Finish(); !error.empty())
                job.Warnings.push_back(std::string("the ") + SurfacePlaneName(store->Plane) +
                                       " store was not published: " + error);
    }
    job.Milliseconds = MillisecondsSince(start);
    job.Ran.store(true, std::memory_order_release);
}

TerrainSurfacePageDriver::TerrainSurfacePageDriver() = default;
TerrainSurfacePageDriver::~TerrainSurfacePageDriver()
{
    EndSession();
}

const TerrainSurfacePageDriver::Session* TerrainSurfacePageDriver::OpenSession(const std::filesystem::path& cache)
{
    if (m_Session)
        return m_Session->Lock ? m_Session.get() : nullptr;
    m_Session = std::make_unique<Session>();
    m_Session->Cache = cache;
    m_Session->Id = GUID::Generate();
    m_Session->Directory = SessionStoreDirectory(cache, m_Session->Id);
    // The lock comes before the folder, so that no sweep finds the folder without its lock. The first
    // store that bakes makes the folder.
    std::error_code error;
    std::filesystem::create_directories(SessionStoreRoot(cache), error);
    auto lock = std::make_unique<FileSystem::ScopedFileLock>(SessionStoreLock(cache, m_Session->Id),
                                                             FileSystem::ScopedFileLock::Mode::Exclusive, false);
    if (!lock->IsLocked())
    {
        Logger::Log::Warning("Surface pages: no session folder for the terrains of a scene that is not saved ({}); "
                             "they draw without surface pages until the scene is saved.",
                             lock->FailureReason());
        return nullptr;
    }
    m_Session->Lock = std::move(lock);
    return m_Session.get();
}

void TerrainSurfacePageDriver::EndSession()
{
    if (!m_Session)
        return;
    // A refused session holds no lock and made no folder: it has nothing to delete.
    if (!m_Session->Lock)
    {
        m_Session.reset();
        return;
    }
    // A store that a job still writes can stay on disk: the sweep of the next driver deletes it.
    for (const std::unique_ptr<TerrainStores>& stores : m_Terrains)
        if (stores->Scene == m_Session->Id && !(stores->Task && !stores->Task->IsDone()))
            for (const std::shared_ptr<PlaneStore>& store : stores->Planes)
                store->Access->Close();
    std::error_code error;
    std::filesystem::remove_all(m_Session->Directory, error);
    m_Session->Lock.reset();
    std::filesystem::remove(SessionStoreLock(m_Session->Cache, m_Session->Id), error);
    m_Session.reset();
}

void TerrainSurfacePageDriver::DeleteSessionStores(const TerrainStores& stores) const
{
    if (!m_Session || stores.Scene != m_Session->Id)
        return;
    for (const std::shared_ptr<PlaneStore>& store : stores.Planes)
    {
        store->Access->Close();
        RemoveSurfaceStore(store->File);
    }
}

void TerrainSurfacePageDriver::CloseDroppedStores()
{
    // A stream can still hold a dropped access. Close each access, so that its reader lets the file go and a
    // later open of the same store can patch it. A store that is open for a run stays as it is.
    for (std::size_t index = 0; index < m_Dropped.size();)
    {
        TerrainStores& stores = *m_Dropped[index];
        if (!Poll(stores))
        {
            ++index;
            continue;
        }
        if (!stores.StoresOpen)
            for (const std::shared_ptr<PlaneStore>& store : stores.Planes)
                store->Access->Close();
        m_Dropped.erase(m_Dropped.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

const TerrainSurfacePageDriver::TerrainStores* TerrainSurfacePageDriver::Find(const TiledTerrainHandle& terrain) const
{
    for (const std::unique_ptr<TerrainStores>& stores : m_Terrains)
        if (stores->Handle == terrain)
            return stores.get();
    return nullptr;
}

std::shared_ptr<const PageStreaming::PageStoreAccess>
TerrainSurfacePageDriver::SurfaceStore(const TiledTerrainHandle& terrain, PageFieldKind plane) const
{
    if (const TerrainStores* stores = Find(terrain))
        for (const std::shared_ptr<PlaneStore>& store : stores->Planes)
            if (store->Plane == plane)
                return store->Access;
    return nullptr;
}

std::size_t TerrainSurfacePageDriver::PendingPages(const TiledTerrainHandle& terrain) const
{
    const TerrainStores* stores = Find(terrain);
    if (!stores)
        return 0u;
    return stores->Pending.Size() + (stores->Job ? stores->Job->Pages.size() : 0u);
}

void TerrainSurfacePageDriver::Start(TerrainStores& stores, std::shared_ptr<BakeJob> job,
                                     JobSystem::WorkStealingThreadPool* pool)
{
    // The bakers take the job's inputs here, on the main thread: the job only reads them.
    const SurfaceHeightPageFetch fetch = MakeHeightFetch(job->Source, job->Overlay);
    for (const std::shared_ptr<PlaneStore>& store : job->Planes)
        store->Baker->SetInputs(job->Inputs, store->InputsKey, fetch);
    stores.Job = job;
    if (pool)
    {
        // Background: the frame's own work goes first, and a shutdown cancels a job not yet started.
        stores.Task = std::make_unique<JobSystem::TaskHandle>(
            pool->Submit([job] { RunJob(*job); }, JobSystem::JobPriority::Background));
        if (stores.Task->IsValid())
            return;
        stores.Task.reset();
    }
    RunJob(*job);
}

bool TerrainSurfacePageDriver::Poll(TerrainStores& stores)
{
    if (!stores.Job)
        return true;
    if (stores.Task && !stores.Task->IsDone())
        return false;
    const std::shared_ptr<BakeJob> job = std::move(stores.Job);
    stores.Task.reset();
    if (!job->Ran.load(std::memory_order_acquire))
    {
        // The pool canceled the job before it ran: its pages bake again, and a later job publishes.
        stores.Pending.QueueNext(job->Pages);
        stores.StoresOpen = stores.StoresOpen || job->Finish;
        return true;
    }
    for (const std::string& warning : job->Warnings)
        Logger::Log::Warning("Surface pages: terrain '{}': {}", stores.Tag, warning);
    stores.RunLongestJobMs = std::max(stores.RunLongestJobMs, job->Milliseconds);
    stores.RunPages += job->Pages.size();
    if (!job->Pages.empty() && Logger::Log::ShouldLog(Logger::LogLevel::Debug))
        Logger::Log::Debug("Surface pages: terrain '{}' job baked {} from level {} in {:.1f} ms", stores.Tag,
                           CountPages(job->Pages.size()), job->Pages.front().Address.Level, job->Milliseconds);
    stores.Pending.MarkBaked(job->Pages);
    if (job->Open)
    {
        std::erase_if(stores.Planes, [](const std::shared_ptr<PlaneStore>& store) { return store->Failed; });
        stores.PinnedLevelsBaked = true;
        uint8 opened = 0;
        for (const std::shared_ptr<PlaneStore>& store : job->Planes)
            if (!store->Failed)
                opened |= SurfacePlaneBit(store->Plane);
        for (const PageFieldKind plane : job->Removed)
            stores.Pending.RemovePlane(SurfacePlaneBit(plane));
        // The pages an edit queued while the stores opened stay next.
        for (SurfacePlaneLayout& present : job->Present)
            stores.Pending.SetPresent(std::move(present));
        if (stores.Planes.empty())
            stores.Pending.Clear();
        if (job->Planes.empty())
        {
            uint8 removed = 0;
            for (const PageFieldKind plane : job->Removed)
                removed |= SurfacePlaneBit(plane);
            Logger::Log::Info("Surface pages: terrain '{}' deleted {}", stores.Tag, NamePlanes(removed, "store"));
        }
        else
            Logger::Log::Info("Surface pages: terrain '{}' opened {} in {:.1f} ms", stores.Tag,
                              NamePlanes(opened, "store"), MillisecondsSince(stores.RunStart));
    }
    // Every queued page is baked: each plane shows.
    if (stores.Pending.Empty())
        for (const std::shared_ptr<PlaneStore>& store : stores.Planes)
            store->Shown = true;
    if (!stores.Pending.Empty() || job->Pages.empty())
        return true;
    // One line a run: an open, or the settle of an edit, which bakes once.
    Logger::Log::Info("Surface pages: terrain '{}' baked {} of {} in {:.1f} ms, at most {:.1f} ms a job",
                      stores.Tag, CountPages(stores.RunPages), NamePlanes(stores.RunPlanes, "plane"),
                      MillisecondsSince(stores.RunStart), stores.RunLongestJobMs);
    return true;
}

std::shared_ptr<TerrainSurfacePageDriver::PlaneStore>
TerrainSurfacePageDriver::MakePlaneStore(const TerrainFrame& frame, const TerrainStores& stores, PageFieldKind plane,
                                         uint64 key, std::span<const std::shared_ptr<PlaneStore>> retired)
{
    const PageStreaming::PageStoreHeader shape = MakeSurfaceStoreHeader(frame.Tiled.Config, plane, 0u);
    auto store = std::make_shared<PlaneStore>();
    store->Plane = plane;
    store->Key = key;
    store->InputsKey = key;
    store->File = SurfaceStoreFile(frame.Directory, frame.Scene, frame.Tag, plane, key);
    store->InSession = frame.InSession;
    store->Access = KeptAccess(retired, plane, shape);
    if (!store->Access)
        store->Access = std::make_shared<PageStreaming::PageStoreAccess>(plane, shape);
    store->Baker =
        std::make_unique<TerrainSurfacePageBaker>(stores.Inputs, plane, store->Access, SurfaceHeightPageFetch{}, frame.Pool);
    return store;
}

std::unique_ptr<TerrainSurfacePageDriver::TerrainStores> TerrainSurfacePageDriver::OpenStores(
    const TerrainFrame& frame, bool rebuild, std::vector<std::shared_ptr<PlaneStore>> retired)
{
    auto stores = std::make_unique<TerrainStores>();
    stores->RunStart = std::chrono::steady_clock::now();
    stores->Handle = frame.Handle;
    stores->Scene = frame.Scene;
    stores->Tag = std::string(frame.Tag);
    stores->Source = ResolveHeightSource(frame.Service, frame.Tiled, frame.Tag, true);
    stores->HeightKey = stores->Source.Key;
    stores->Write = frame.Write;
    stores->Seen = true;
    stores->Inputs = frame.System.SnapshotSurfaceBake(frame.Tiled, frame.Write);

    auto job = std::make_shared<BakeJob>();
    job->Open = true;
    job->Rebuild = rebuild;
    job->Retired = std::move(retired);
    job->Directory = frame.Directory;
    job->Scene = frame.Scene;
    job->Tag = stores->Tag;
    job->Inputs = stores->Inputs;
    job->Source = stores->Source;
    const PageStreaming::PageStoreHeader shape = MakeSurfaceStoreHeader(frame.Tiled.Config, PageFieldKind::Splat, 0u);
    const std::vector<PageStreaming::PageStoreLevel> levels =
        PageStreaming::BuildPageStoreLevels(shape.SamplesX, shape.SamplesZ);
    stores->PinnedLevel =
        FirstPinnedSurfaceLevel(frame.Tiled, levels, frame.Service.GetTerrainPages().Surface().Profile());
    stores->Pending.Reset(levels, stores->PinnedLevel);
    // The wanted pages that a store lacks queue when the open job ends, so the pages to bake are known then.
    stores->Pending.Want(frame.Service.GetTerrainPages().Surface().WantedPages(frame.Tiled.GlobalGpuHandleIndex));
    job->PinnedLevel = stores->PinnedLevel;
    // The open bakes the surface of now: it takes the change record.
    frame.Tiled.SurfaceChangedSince = frame.Tiled.SurfaceVersion;
    std::vector<PageAddress> read;
    for (const PageFieldKind plane : kSurfacePlanes)
    {
        const uint64 key = frame.System.SurfaceStoreKey(frame.Tiled, plane, stores->HeightKey, frame.OriginY, frame.Write);
        if (key == 0)
        {
            job->Removed.push_back(plane);
            continue;
        }
        std::shared_ptr<PlaneStore> store = MakePlaneStore(frame, *stores, plane, key, job->Retired);
        if (read.empty())
            read = store->Baker->HeightPagesReadByCreate(job->PinnedLevel);
        stores->Planes.push_back(store);
    }
    job->Planes = stores->Planes;
    job->Overlay = MakeOverlayPages(frame.Tiled.PageOverlay.get(), read);
    Start(*stores, std::move(job), frame.Pool);
    Poll(*stores);
    return stores;
}

bool TerrainSurfacePageDriver::NoteEdits(TerrainStores& stores, TiledTerrainData& tiled,
                                         const TerrainColorGroundWrite& write)
{
    bool edited = false;
    if (tiled.SurfaceChangedSince != tiled.SurfaceVersion)
    {
        stores.Unsettled = stores.Unsettled ? PageStreaming::Union(*stores.Unsettled, tiled.SurfaceChanged)
                                            : tiled.SurfaceChanged;
        stores.UnsettledPlanes |= tiled.SurfaceChangedPlanes;
        // A grass edit of the whole surface bakes its planes' pages again; it makes no store again.
        stores.UnsettledWhole = stores.UnsettledWhole || ((tiled.SurfaceChangedPlanes & ~kGrassSurfacePlanes) != 0u &&
                                                          CoversTheWholeSurface(tiled, tiled.SurfaceChanged));
        tiled.SurfaceChangedSince = tiled.SurfaceVersion;
        edited = true;
    }
    // The ground write is in the Color store's key and reaches every page.
    if (!(write == stores.Write))
    {
        const uint32 interior = tiled.Config.TileConfig.HeightmapWidth - 1u;
        stores.Unsettled = PageStreaming::PageSampleRect{0u, 0u, tiled.Config.TilesPerAxisX * interior,
                                                         tiled.Config.TilesPerAxisZ * interior};
        stores.UnsettledPlanes = kEverySurfacePlane;
        stores.UnsettledWhole = true;
        stores.Write = write;
        edited = true;
    }
    return edited;
}

void TerrainSurfacePageDriver::StartRun(TerrainStores& stores)
{
    stores.RunPages = 0;
    stores.RunPlanes = 0;
    stores.RunStart = std::chrono::steady_clock::now();
    stores.RunLongestJobMs = 0.0;
    stores.RunQueueLogged = false;
}

void TerrainSurfacePageDriver::QueueEdit(TerrainStores& stores, const PageStreaming::PageSampleRect& changed,
                                         uint8 planes)
{
    // A present page of any level that reads the rect bakes again: its direct texels see the edit, and
    // the refilter of the finer pages remakes its filtered texels. An absent page bakes the edit when wanted.
    FlatMap<PageAddress, uint8> edited;
    std::vector<PageAddress> present;
    for (const std::shared_ptr<PlaneStore>& store : stores.Planes)
    {
        const uint8 plane = SurfacePlaneBit(store->Plane);
        if ((planes & plane) == 0u)
            continue;
        const std::vector<PageStreaming::PageSampleRect> reading = store->Baker->PagesReading(changed);
        present.clear();
        for (uint32 level = 0; level < reading.size(); ++level)
            stores.Pending.AppendPresent(plane, level, reading[level], present);
        for (const PageAddress& address : present)
            edited.GetOrInsert(address) |= plane;
    }
    if (edited.Empty())
        return;
    // A job in flight is part of the run: the edit joins it.
    if (stores.Pending.Empty() && !stores.Job)
        StartRun(stores);
    // The queue takes the last page first: the finest pages bake first, so a coarse page filters fresh pages.
    std::vector<SurfaceBakePage> pages;
    pages.reserve(edited.Size());
    for (const auto& [address, pagePlanes] : edited)
        pages.push_back(SurfaceBakePage{address, pagePlanes});
    std::ranges::reverse(pages);
    stores.Pending.QueueNext(pages);
    Logger::Log::Debug("Surface pages: terrain '{}' queued {} for an edit", stores.Tag, CountPages(pages.size()));
}

void TerrainSurfacePageDriver::ChangeGrassStore(TerrainStores& stores, const TerrainFrame& frame, uint64 key)
{
    auto job = std::make_shared<BakeJob>();
    job->Open = true;
    job->PinnedLevel = stores.PinnedLevel;
    job->Directory = frame.Directory;
    job->Scene = stores.Scene;
    job->Tag = stores.Tag;
    job->Inputs = stores.Inputs;
    job->Source = stores.Source;
    job->Overlay = std::make_shared<OverlayPages>();
    const auto grass = std::find_if(stores.Planes.begin(), stores.Planes.end(), [](const auto& store) {
        return store->Plane == PageFieldKind::GrassControl;
    });
    if (grass != stores.Planes.end())
    {
        job->Retired.push_back(*grass);
        job->Removed.push_back(PageFieldKind::GrassControl);
        stores.Planes.erase(grass);
    }
    if (key != 0)
    {
        std::shared_ptr<PlaneStore> store = MakePlaneStore(frame, stores, PageFieldKind::GrassControl, key, {});
        // The other planes stay on screen: the new plane shows when its wanted pages are baked.
        store->Shown = false;
        stores.Planes.push_back(store);
        job->Planes.push_back(std::move(store));
    }
    if (stores.Pending.Empty() && !stores.Job)
        StartRun(stores);
    Start(stores, std::move(job), frame.Pool);
    Poll(stores);
}

void TerrainSurfacePageDriver::BakePending(TerrainStores& stores, const TerrainFrame& frame)
{
    if (stores.Job || stores.Planes.empty())
        return;
    // The pages the surface stream wanted at its last request, the coarsest level first: the view's far field
    // is whole sooner, and the camera's ground follows.
    const bool idle = stores.Pending.Empty();
    stores.Pending.Want(frame.Service.GetTerrainPages().Surface().WantedPages(frame.Tiled.GlobalGpuHandleIndex));
    if (stores.Pending.Empty())
    {
        // A Want that empties the queue after a job that left the stores open: a job of no page
        // publishes them, so every run ends with one Finish.
        if (stores.StoresOpen && !stores.Unsettled)
            StartFinish(stores, frame.Pool);
        return;
    }
    if (idle)
        StartRun(stores);
    if (!stores.RunQueueLogged)
    {
        Logger::Log::Info("Surface pages: terrain '{}' queued {} to bake", stores.Tag,
                          CountPages(stores.Pending.Size()));
        stores.RunQueueLogged = true;
    }
    auto job = std::make_shared<BakeJob>();
    stores.Pending.Take(kPagesAJob, job->Pages);
    for (const SurfaceBakePage& page : job->Pages)
        stores.RunPlanes |= page.Planes;
    // The stores stay open while the run lasts: a job opens and publishes no file.
    job->Finish = stores.Pending.Empty() && !stores.Unsettled;
    stores.StoresOpen = !job->Finish;
    job->Inputs = stores.Inputs;
    job->Source = stores.Source;
    job->Planes = stores.Planes;
    std::vector<PageAddress> read;
    // The planes of a page can differ, so the read is the union of each plane's read.
    for (const std::shared_ptr<PlaneStore>& store : stores.Planes)
        for (const PageAddress& page : store->Baker->HeightPagesReadByBake(PlanePages(job->Pages, store->Plane)))
            if (std::find(read.begin(), read.end(), page) == read.end())
                read.push_back(page);
    job->Overlay = MakeOverlayPages(frame.Tiled.PageOverlay.get(), read);
    Start(stores, std::move(job), frame.Pool);
    Poll(stores);
}

void TerrainSurfacePageDriver::StartFinish(TerrainStores& stores, JobSystem::WorkStealingThreadPool* pool)
{
    auto job = std::make_shared<BakeJob>();
    job->Finish = true;
    job->Inputs = stores.Inputs;
    job->Source = stores.Source;
    job->Planes = stores.Planes;
    job->Overlay = std::make_shared<OverlayPages>();
    stores.StoresOpen = false;
    Start(stores, std::move(job), pool);
    Poll(stores);
}

void TerrainSurfacePageDriver::Close(TerrainStores& stores, JobSystem::WorkStealingThreadPool* pool)
{
    auto job = std::make_shared<BakeJob>();
    job->Retired = stores.Planes;
    job->Inputs = stores.Inputs;
    job->Source = stores.Source;
    job->Overlay = std::make_shared<OverlayPages>();
    stores.StoresOpen = false;
    Start(stores, std::move(job), pool);
    Poll(stores);
}

void TerrainSurfacePageDriver::UpdateTerrain(ECS::World& world, ECS::EntityHandle entity,
                                             const Components::Terrain& terrain, float32 originY,
                                             TerrainService& service, TerrainModifierSystem& system,
                                             JobSystem::WorkStealingThreadPool* pool,
                                             const std::filesystem::path& directory)
{
    const TiledTerrainHandle handle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration};
    if (handle.Index == 0 && handle.Generation == 0)
        return;
    TiledTerrainData* tiled = service.GetTiledTerrainData(handle);
    const auto* sceneTag = world.GetComponent<Components::SceneEntityTag>(entity);
    // A terrain whose scene is not saved bakes into the session stores, named for its tiled terrain.
    // Without its session, it bakes no surface pages.
    const bool inSession = terrain.BakeOriginScene.IsNull();
    const Session* session = inSession ? OpenSession(directory) : nullptr;
    if (inSession && !session)
        return;
    const GUID scene = session ? session->Id : terrain.BakeOriginScene;
    const auto sameTerrain = [&](const std::unique_ptr<TerrainStores>& stores) {
        return stores->Handle == handle;
    };
    auto found = std::find_if(m_Terrains.begin(), m_Terrains.end(), sameTerrain);
    // The session tag is made once, when the terrain joins the session; its stores keep it.
    const bool joined = session && found != m_Terrains.end() && (*found)->Scene == scene;
    const std::string sessionTag =
        inSession && !joined ? std::format("tiled-{}-{}", handle.Index, handle.Generation) : std::string{};
    const std::string_view tag = !inSession ? (sceneTag ? sceneTag->View() : std::string_view{})
                                 : joined   ? std::string_view((*found)->Tag)
                                            : std::string_view(sessionTag);
    if (!tiled || tag.empty())
        return;
    if (found != m_Terrains.end())
        (*found)->Seen = true;
    // The surface bakes over the modifiers' heights once the resident splats are baked.
    if (!tiled->PageOverlayCurrent || !tiled->ResidentSplatsBaked)
        return;
    const SurfaceHeightSource source = ResolveHeightSource(service, *tiled, tag, false);
    if (source.Key == 0)
        return;
    const TerrainFrame frame{*tiled,
                             service,
                             system,
                             pool,
                             session ? session->Directory : directory,
                             handle,
                             scene,
                             tag,
                             originY,
                             TerrainColorMapBaker::ResolveGroundWrite(world, entity),
                             inSession};

    // The stores of a terrain are made again only between its jobs.
    const bool idle = found == m_Terrains.end() || Poll(**found);
    if (idle && found != m_Terrains.end() &&
        ((*found)->HeightKey != source.Key || (*found)->Scene != scene || (*found)->Tag != tag))
    {
        *found = OpenStores(frame, false, std::move((*found)->Planes));
        return;
    }
    if (found == m_Terrains.end())
    {
        if (std::ranges::any_of(m_Dropped, sameTerrain))
            return; // its dropped stores still have a job
        m_Terrains.push_back(OpenStores(frame, false, {}));
        return;
    }

    TerrainStores& stores = **found;
    const bool edited = NoteEdits(stores, *tiled, frame.Write);
    if (edited || service.IsInteractiveModifierEdit())
    {
        stores.QuietFrames = 0;
        return;
    }
    // A stroke or a drag bakes once, at its settle: the whole surface makes the stores again whatever
    // their key, and a rect bakes again the pages that read it, from the inputs of the settle.
    if (stores.Unsettled)
    {
        if (stores.QuietFrames < kSurfaceSettleFrames)
            ++stores.QuietFrames;
        if (stores.QuietFrames < kSurfaceSettleFrames)
            return;
        if (stores.UnsettledWhole)
        {
            if (idle)
                *found = OpenStores(frame, true, std::move(stores.Planes));
            return;
        }
        // An edit bakes again the present pages that read it: they are known when the open job ends.
        if (stores.Job && stores.Job->Open)
            return;
        // A grass edit that makes or deletes the GrassControl store waits for the job in flight.
        const bool grassEdit = (stores.UnsettledPlanes & SurfacePlaneBit(PageFieldKind::GrassControl)) != 0u;
        const uint64 grassKey = grassEdit ? system.SurfaceStoreKey(*tiled, PageFieldKind::GrassControl,
                                                                              stores.HeightKey, originY, frame.Write)
                                                      : 0u;
        const bool hasGrass = std::any_of(stores.Planes.begin(), stores.Planes.end(), [](const auto& store) {
            return store->Plane == PageFieldKind::GrassControl;
        });
        const bool grassStoreChanges = grassEdit && (grassKey != 0u) != hasGrass;
        if (grassStoreChanges && !idle)
            return;
        stores.Inputs = system.SnapshotSurfaceBake(*tiled, frame.Write);
        if (grassStoreChanges)
            ChangeGrassStore(stores, frame, grassKey);
        for (const std::shared_ptr<PlaneStore>& store : stores.Planes)
            store->InputsKey = grassEdit && store->Plane == PageFieldKind::GrassControl
                                   ? grassKey
                                   : system.SurfaceStoreKey(*tiled, store->Plane, stores.HeightKey, originY, frame.Write);
        // A new GrassControl store bakes all its pages; the other planes bake the edited rect.
        const uint8 planes = grassStoreChanges
                                 ? static_cast<uint8>(stores.UnsettledPlanes & ~SurfacePlaneBit(PageFieldKind::GrassControl))
                                 : stores.UnsettledPlanes;
        QueueEdit(stores, *stores.Unsettled, planes);
        stores.Unsettled.reset();
        stores.UnsettledPlanes = 0;
        if (grassStoreChanges)
            return;
    }
    if (idle)
        BakePending(stores, frame);
}

void TerrainSurfacePageDriver::PublishSource(TerrainService& service, const TerrainStores& stores)
{
    TiledTerrainData* tiled = service.GetTiledTerrainData(stores.Handle);
    if (!tiled)
        return;
    TerrainSurfacePageSource& source = tiled->SurfacePages;
    source.Planes.clear();
    source.PlaneMask = 0;
    source.PinnedLevelsBaked = false;
    if (!stores.Seen)
        return;
    constexpr uint8 kFirst = static_cast<uint8>(PageFieldKind::Splat);
    constexpr uint8 kLast = static_cast<uint8>(PageFieldKind::Color);
    for (uint8 kind = kFirst; kind <= kLast; ++kind)
        for (const std::shared_ptr<PlaneStore>& store : stores.Planes)
            if (static_cast<uint8>(store->Plane) == kind && store->Access && store->Shown)
            {
                source.Planes.push_back(store->Access);
                source.PlaneMask |= 1u << (kind - kFirst);
            }
    source.PinnedLevelsBaked = stores.PinnedLevelsBaked && !source.Planes.empty();
}

void TerrainSurfacePageDriver::Update(ECS::World& world, TerrainService& service, TerrainModifierSystem& system,
                                      JobSystem::WorkStealingThreadPool* pool)
{
    const std::filesystem::path& directory = service.GetHeightPageStores().Location().CacheDirectory;
    if (m_Session && m_Session->Cache != directory)
    {
        for (std::unique_ptr<TerrainStores>& stores : m_Terrains)
            if (stores->Scene == m_Session->Id)
            {
                stores->Seen = false;
                PublishSource(service, *stores);
            }
        EndSession();
    }
    if (directory.empty())
    {
        for (std::unique_ptr<TerrainStores>& stores : m_Terrains)
        {
            stores->Seen = false;
            PublishSource(service, *stores);
            m_Dropped.push_back(std::move(stores));
        }
        m_Terrains.clear();
        CloseDroppedStores();
        return;
    }
    CloseDroppedStores();
    // The first frame of a cache deletes the sessions of editors that stopped without deleting them.
    if (directory != m_SweptCache)
    {
        RemoveOrphanSessionStores(directory);
        m_SweptCache = directory;
    }
    for (std::unique_ptr<TerrainStores>& stores : m_Terrains)
        stores->Seen = false;

    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle entity, const Components::Terrain& terrain, const Components::WorldTransform& transform)
        {
            UpdateTerrain(world, entity, terrain, (transform.GetPosition() - Mathematics::WorldPosition{}).y, service,
                          system, pool, directory);
        });
    // A forgotten terrain stays until a job of its own publishes its open stores, so that no later
    // job writes its files beside it.
    for (const std::unique_ptr<TerrainStores>& stores : m_Terrains)
    {
        PublishSource(service, *stores);
        if (!stores->Seen && Poll(*stores) && stores->StoresOpen)
            Close(*stores, pool);
    }
    // A forgotten terrain of the session deletes its stores: no later frame opens them again.
    std::erase_if(m_Terrains, [this](const std::unique_ptr<TerrainStores>& stores) {
        const bool forgotten = !stores->Seen && Poll(*stores) && !stores->StoresOpen;
        if (forgotten)
            DeleteSessionStores(*stores);
        return forgotten;
    });
}

} // namespace GameEngine::TerrainECS
