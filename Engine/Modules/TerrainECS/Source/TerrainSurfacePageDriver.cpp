#include "TerrainECS/TerrainSurfacePageDriver.h"

#include "TerrainECS/HeightPageStoreLoader.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/TerrainSurfaceBakeInputs.h"
#include "TerrainECS/TerrainSurfacePageBaker.h"
#include "TerrainECS/TerrainSurfaceStore.h"
#include "TerrainECS/TerrainColorMapBaker.h"
#include "TerrainECS/TerrainHeightPageDriver.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "Components/SceneEntityTag.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "JobSystem/TaskHandle.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStore/PageStoreAccess.h"
#include "PageStreaming/PageStore/PageStoreReader.h"
#include "Types/FlatMap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
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
// overlay adds nothing, so a base page plus it is the base page plus HeightPageOverlay::ApplyToPage
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
std::shared_ptr<const OverlayPages> MakeOverlayPages(const PageStreaming::HeightPageOverlay* overlay,
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
            std::vector<uint8> stored;
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

// The first pinned level of `tiled`'s height pyramid (FirstPinnedPageLevel), at least level 1.
uint32 FirstPinnedSurfaceLevel(const TiledTerrainData& tiled, std::span<const PageStreaming::PageStoreLevel> levels,
                               uint32 cacheSlots)
{
    const float32 texel = tiled.Config.TileWorldSize / static_cast<float32>(tiled.Config.TileConfig.HeightmapWidth - 1u);
    return std::max(1u, FirstPinnedPageLevel(levels, 1u, texel, cacheSlots));
}

float64 MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<float64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

struct TerrainSurfacePageDriver::PlaneStore
{
    PageFieldKind Plane = PageFieldKind::Splat;
    uint64 Key = 0;
    std::filesystem::path File;
    // The plane's store for its streams: the same object while the plane has a store, over every run.
    std::shared_ptr<PageStreaming::PageStoreAccess> Access;
    std::unique_ptr<TerrainSurfacePageBaker> Baker;
    bool Failed = false; // the open job could not make the store
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
    std::vector<PageAddress> Pages; // the level-0 pages it bakes
    // Publishes the stores after its pages: the last job of a run.
    bool Finish = false;
    // The stores of the planes the terrain had before, published first.
    std::vector<std::shared_ptr<PlaneStore>> Retired;

    // Results. Missing marks the level-0 pages the open stores lack (one byte a page, row by row).
    std::vector<uint8> Missing;
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
    uint64 SeenVersion = 0;
    std::vector<std::shared_ptr<PlaneStore>> Planes;
    PageStreaming::PageStoreLevel Level0;
    // The inputs of the newest open or settle, read by every job started after it.
    std::shared_ptr<const TerrainSurfaceBakeInputs> Inputs;
    SurfaceHeightSource Source;
    std::vector<PageAddress> Pending; // the level-0 pages every plane still bakes, the next at the back
    // The job in flight and its handle; null when none.
    std::shared_ptr<BakeJob> Job;
    std::unique_ptr<JobSystem::TaskHandle> Task;
    // The edits since the last settle: the level-0 rect they changed, or the whole surface.
    std::optional<PageStreaming::PageSampleRect> Unsettled;
    bool UnsettledWhole = false;
    uint32 QuietFrames = 0;
    // The bake run since Pending was last empty, for its log line.
    std::size_t RunPages = 0;
    std::chrono::steady_clock::time_point RunStart;
    float64 RunLongestJobMs = 0.0;
    bool RunIsColdOpen = false;
    // A job left the stores open: a later job patches them, and the last one publishes them.
    bool StoresOpen = false;
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
};

namespace
{

// Marks in `needed` (one byte a level-0 page, row by row) every level-0 page that the store at
// `file` does not hold.
void MarkMissingPages(const std::filesystem::path& file, PageFieldKind plane, const PageStreaming::PageStoreLevel& level0,
                      std::vector<uint8>& needed)
{
    PageStreaming::PageStoreReader reader;
    const bool open = reader.Open(file, plane).empty();
    for (uint32 pz = 0; pz < level0.PagesZ; ++pz)
        for (uint32 px = 0; px < level0.PagesX; ++px)
            if (!open || !reader.Layout().FindPresent(PageAddress{0, 0, px, pz}))
                needed[static_cast<std::size_t>(pz) * level0.PagesX + px] = 1u;
}

// The level-0 pages marked in `needed`, the first Morton code at the back: each block of pages
// taken from the back is a square of pages that share most of the height pages they read.
std::vector<PageAddress> PendingInMortonOrder(const PageStreaming::PageStoreLevel& level0,
                                              const std::vector<uint8>& needed)
{
    std::vector<PageAddress> pages;
    for (uint32 pz = 0; pz < level0.PagesZ; ++pz)
        for (uint32 px = 0; px < level0.PagesX; ++px)
            if (needed[static_cast<std::size_t>(pz) * level0.PagesX + px] != 0u)
                pages.push_back(PageAddress{0, 0, px, pz});
    std::sort(pages.begin(), pages.end(), [](const PageAddress& a, const PageAddress& b) {
        return PageStreaming::PageMortonCode(a.X, a.Z) > PageStreaming::PageMortonCode(b.X, b.Z);
    });
    return pages;
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

void TerrainSurfacePageDriver::OpenJobStores(BakeJob& job, const PageStreaming::PageStoreLevel& level0)
{
    // A retired access that no plane keeps reads no page from now on: it lets its file go before the
    // file is deleted, so that no stream reads a deleted store.
    for (const std::shared_ptr<PlaneStore>& retired : job.Retired)
        if (std::none_of(job.Planes.begin(), job.Planes.end(),
                         [&retired](const std::shared_ptr<PlaneStore>& store) { return store->Access == retired->Access; }))
            retired->Access->Close();
    for (const PageFieldKind plane : job.Removed)
        RemoveSurfaceStores(job.Directory, job.Scene, job.Tag, plane);
    job.Missing.assign(static_cast<std::size_t>(level0.PagesX) * level0.PagesZ, 0u);
    for (const std::shared_ptr<PlaneStore>& store : job.Planes)
    {
        if (!job.Rebuild && HoldsCurrentStore(*store))
        {
            if (std::string error = store->Access->Publish(store->File); !error.empty())
                job.Warnings.push_back(std::string("the ") + SurfacePlaneName(store->Plane) +
                                       " store is not readable: " + error);
            MarkMissingPages(store->File, store->Plane, level0, job.Missing);
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
        std::fill(job.Missing.begin(), job.Missing.end(), uint8{1});
    }
}

void TerrainSurfacePageDriver::RunJob(BakeJob& job, const PageStreaming::PageStoreLevel& level0)
{
    const auto start = std::chrono::steady_clock::now();
    for (const std::shared_ptr<PlaneStore>& store : job.Retired)
        if (std::string error = store->Baker->Finish(); !error.empty())
            job.Warnings.push_back(std::string("the previous ") + SurfacePlaneName(store->Plane) +
                                   " store was not published: " + error);
    if (job.Open)
        OpenJobStores(job, level0);
    for (const std::shared_ptr<PlaneStore>& store : job.Planes)
    {
        if (store->Failed)
            continue;
        if (!job.Pages.empty())
            if (std::string error = store->Baker->BakeLevel0Pages(store->File, job.Pages); !error.empty())
                job.Warnings.push_back(std::string("the ") + SurfacePlaneName(store->Plane) +
                                       " store missed level-0 pages: " + error);
        if (job.Finish)
            if (std::string error = store->Baker->Finish(); !error.empty())
                job.Warnings.push_back(std::string("the ") + SurfacePlaneName(store->Plane) +
                                       " store was not published: " + error);
    }
    job.Milliseconds = MillisecondsSince(start);
    job.Ran.store(true, std::memory_order_release);
}

TerrainSurfacePageDriver::TerrainSurfacePageDriver() = default;
TerrainSurfacePageDriver::~TerrainSurfacePageDriver() = default;

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
    return stores->Pending.size() + (stores->Job ? stores->Job->Pages.size() : 0u);
}

void TerrainSurfacePageDriver::Start(TerrainStores& stores, std::shared_ptr<BakeJob> job,
                                     JobSystem::WorkStealingThreadPool* pool)
{
    // The bakers take the job's inputs here, on the main thread: the job only reads them.
    const SurfaceHeightPageFetch fetch = MakeHeightFetch(job->Source, job->Overlay);
    for (const std::shared_ptr<PlaneStore>& store : job->Planes)
        store->Baker->SetInputs(job->Inputs, fetch);
    stores.Job = job;
    const PageStreaming::PageStoreLevel level0 = stores.Level0;
    if (pool)
    {
        // Background: the frame's own work goes first, and a shutdown cancels a job not yet started.
        stores.Task = std::make_unique<JobSystem::TaskHandle>(
            pool->Submit([job, level0] { RunJob(*job, level0); }, JobSystem::JobPriority::Background));
        if (stores.Task->IsValid())
            return;
        stores.Task.reset();
    }
    RunJob(*job, level0);
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
        // The pool cancelled the job before it ran: its pages bake again.
        stores.Pending.insert(stores.Pending.end(), job->Pages.begin(), job->Pages.end());
        return true;
    }
    for (const std::string& warning : job->Warnings)
        Logger::Log::Warning("Surface pages: terrain '{}': {}", stores.Tag, warning);
    stores.RunLongestJobMs = std::max(stores.RunLongestJobMs, job->Milliseconds);
    if (job->Open)
    {
        std::erase_if(stores.Planes, [](const std::shared_ptr<PlaneStore>& store) { return store->Failed; });
        // The pages an edit queued while the stores opened stay next.
        std::vector<PageAddress> edited = std::move(stores.Pending);
        stores.Pending = stores.Planes.empty() ? std::vector<PageAddress>{}
                                               : PendingInMortonOrder(stores.Level0, job->Missing);
        std::erase_if(stores.Pending, [&edited](const PageAddress& page) {
            return std::find(edited.begin(), edited.end(), page) != edited.end();
        });
        stores.Pending.insert(stores.Pending.end(), edited.begin(), edited.end());
        stores.RunPages = stores.Pending.size();
        Logger::Log::Info("Surface pages: terrain '{}' opened {} stores in {:.1f} ms, {} level-0 pages to bake",
                          stores.Tag, stores.Planes.size(), MillisecondsSince(stores.RunStart), stores.Pending.size());
    }
    if (!stores.Pending.empty() || job->Pages.empty())
        return true;
    constexpr std::string_view kRunDone =
        "Surface pages: terrain '{}' baked {} level-0 pages over {} planes in {:.1f} ms, at most {:.1f} ms a job";
    if (stores.RunIsColdOpen)
        Logger::Log::Info(kRunDone, stores.Tag, stores.RunPages, stores.Planes.size(), MillisecondsSince(stores.RunStart),
                          stores.RunLongestJobMs);
    else
        Logger::Log::Debug(kRunDone, stores.Tag, stores.RunPages, stores.Planes.size(),
                           MillisecondsSince(stores.RunStart), stores.RunLongestJobMs);
    return true;
}

std::unique_ptr<TerrainSurfacePageDriver::TerrainStores> TerrainSurfacePageDriver::OpenStores(
    const TerrainFrame& frame, bool rebuild, std::vector<std::shared_ptr<PlaneStore>> retired)
{
    auto stores = std::make_unique<TerrainStores>();
    stores->RunStart = std::chrono::steady_clock::now();
    stores->RunIsColdOpen = true;
    stores->Handle = frame.Handle;
    stores->Scene = frame.Scene;
    stores->Tag = std::string(frame.Tag);
    stores->Source = ResolveHeightSource(frame.Service, frame.Tiled, frame.Tag, true);
    stores->HeightKey = stores->Source.Key;
    stores->Write = frame.Write;
    stores->SeenVersion = frame.Tiled.SurfaceVersion;
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
    stores->Level0 = levels.front();
    job->PinnedLevel = FirstPinnedSurfaceLevel(frame.Tiled, levels, frame.Service.GetHeightPages().Profile().Slots);
    std::vector<PageAddress> read;
    for (const PageFieldKind plane : kSurfacePlanes)
    {
        const PageStreaming::PageStoreHeader planeShape = MakeSurfaceStoreHeader(frame.Tiled.Config, plane, 0u);
        const uint64 key = frame.System.SurfaceStoreKey(frame.Tiled, plane, stores->HeightKey, frame.OriginY, frame.Write);
        if (key == 0)
        {
            job->Removed.push_back(plane);
            continue;
        }
        auto store = std::make_shared<PlaneStore>();
        store->Plane = plane;
        store->Key = key;
        store->File = SurfaceStoreFile(frame.Directory, frame.Scene, frame.Tag, plane, key);
        store->Access = KeptAccess(job->Retired, plane, planeShape);
        if (!store->Access)
            store->Access = std::make_shared<PageStreaming::PageStoreAccess>(plane, planeShape);
        store->Baker = std::make_unique<TerrainSurfacePageBaker>(stores->Inputs, plane, store->Access,
                                                                 SurfaceHeightPageFetch{}, frame.Pool);
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

bool TerrainSurfacePageDriver::NoteEdits(TerrainStores& stores, const TiledTerrainData& tiled,
                                         const TerrainColorGroundWrite& write)
{
    bool edited = false;
    if (tiled.SurfaceVersion != stores.SeenVersion)
    {
        stores.Unsettled = stores.Unsettled ? PageStreaming::Union(*stores.Unsettled, tiled.SurfaceChanged)
                                            : tiled.SurfaceChanged;
        stores.UnsettledWhole = stores.UnsettledWhole || CoversTheWholeSurface(tiled, tiled.SurfaceChanged);
        stores.SeenVersion = tiled.SurfaceVersion;
        edited = true;
    }
    // The ground write is in the Color store's key and reaches every page.
    if (!(write == stores.Write))
    {
        const uint32 interior = tiled.Config.TileConfig.HeightmapWidth - 1u;
        stores.Unsettled = PageStreaming::PageSampleRect{0u, 0u, tiled.Config.TilesPerAxisX * interior,
                                                         tiled.Config.TilesPerAxisZ * interior};
        stores.UnsettledWhole = true;
        stores.Write = write;
        edited = true;
    }
    return edited;
}

void TerrainSurfacePageDriver::QueueEdit(TerrainStores& stores, const PageStreaming::PageSampleRect& changed)
{
    std::vector<PageAddress> pages;
    for (const std::shared_ptr<PlaneStore>& store : stores.Planes)
        for (const PageAddress& page : store->Baker->Level0PagesReading(changed))
            if (std::find(pages.begin(), pages.end(), page) == pages.end())
                pages.push_back(page);
    if (stores.Pending.empty())
    {
        stores.RunPages = 0;
        stores.RunStart = std::chrono::steady_clock::now();
        stores.RunLongestJobMs = 0.0;
        stores.RunIsColdOpen = false;
    }
    const std::size_t queued = stores.Pending.size();
    std::erase_if(stores.Pending, [&pages](const PageAddress& page) {
        return std::find(pages.begin(), pages.end(), page) != pages.end();
    });
    stores.RunPages += pages.size() - (queued - stores.Pending.size());
    stores.Pending.insert(stores.Pending.end(), pages.begin(), pages.end());
    Logger::Log::Debug("Surface pages: terrain '{}' queued {} level-0 pages for an edit", stores.Tag, pages.size());
}

void TerrainSurfacePageDriver::BakePending(TerrainStores& stores, const TerrainFrame& frame)
{
    if (stores.Job || stores.Pending.empty() || stores.Planes.empty())
        return;
    const std::size_t count = std::min(kLevel0PagesAJob, stores.Pending.size());
    auto job = std::make_shared<BakeJob>();
    job->Pages.assign(stores.Pending.end() - static_cast<std::ptrdiff_t>(count), stores.Pending.end());
    stores.Pending.resize(stores.Pending.size() - count);
    // The stores stay open while the run lasts: a job opens and publishes no file.
    job->Finish = stores.Pending.empty() && !stores.Unsettled;
    stores.StoresOpen = !job->Finish;
    job->Inputs = stores.Inputs;
    job->Source = stores.Source;
    job->Planes = stores.Planes;
    std::vector<PageAddress> read;
    for (const std::shared_ptr<PlaneStore>& store : stores.Planes)
        if (read.empty())
            read = store->Baker->HeightPagesReadByBake(job->Pages);
    job->Overlay = MakeOverlayPages(frame.Tiled.PageOverlay.get(), read);
    Start(stores, std::move(job), frame.Pool);
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
    const std::string_view tag = sceneTag ? sceneTag->View() : std::string_view{};
    if (!tiled || terrain.BakeOriginScene.IsNull() || tag.empty())
        return;
    const auto sameTerrain = [&](const std::unique_ptr<TerrainStores>& stores) {
        return stores->Handle == handle;
    };
    auto found = std::find_if(m_Terrains.begin(), m_Terrains.end(), sameTerrain);
    if (found != m_Terrains.end())
        (*found)->Seen = true;
    // The surface bakes over the modifiers' heights and the committed splat range.
    if (!tiled->PageOverlayCurrent || !tiled->SplatBakeRangeValid)
        return;
    const SurfaceHeightSource source = ResolveHeightSource(service, *tiled, tag, false);
    if (source.Key == 0)
        return;
    const TerrainFrame frame{*tiled, service, system, pool, directory, handle, terrain.BakeOriginScene, tag, originY,
                             TerrainColorMapBaker::ResolveGroundWrite(world, entity)};

    // The stores of a terrain are made again only between its jobs.
    const bool idle = found == m_Terrains.end() || Poll(**found);
    if (idle && found != m_Terrains.end() &&
        ((*found)->HeightKey != source.Key || (*found)->Scene != terrain.BakeOriginScene || (*found)->Tag != tag))
    {
        *found = OpenStores(frame, false, std::move((*found)->Planes));
        return;
    }
    if (found == m_Terrains.end())
    {
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
        stores.Inputs = system.SnapshotSurfaceBake(*tiled, frame.Write);
        QueueEdit(stores, *stores.Unsettled);
        stores.Unsettled.reset();
    }
    if (idle)
        BakePending(stores, frame);
}

void TerrainSurfacePageDriver::Update(ECS::World& world, TerrainService& service, TerrainModifierSystem& system,
                                      JobSystem::WorkStealingThreadPool* pool)
{
    const std::filesystem::path& directory = service.GetHeightPageStores().Location().CacheDirectory;
    if (directory.empty())
    {
        m_Terrains.clear();
        return;
    }
    for (std::unique_ptr<TerrainStores>& stores : m_Terrains)
        stores->Seen = false;

    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle entity, const Components::Terrain& terrain, const Components::WorldTransform& transform)
        { UpdateTerrain(world, entity, terrain, transform.matrix[13], service, system, pool, directory); });
    // A forgotten terrain stays until a job of its own publishes its open stores, so that no later
    // job writes its files beside it.
    for (const std::unique_ptr<TerrainStores>& stores : m_Terrains)
        if (!stores->Seen && Poll(*stores) && stores->StoresOpen)
            Close(*stores, pool);
    std::erase_if(m_Terrains, [](const std::unique_ptr<TerrainStores>& stores) {
        return !stores->Seen && Poll(*stores) && !stores->StoresOpen;
    });
}

} // namespace GameEngine::TerrainECS
