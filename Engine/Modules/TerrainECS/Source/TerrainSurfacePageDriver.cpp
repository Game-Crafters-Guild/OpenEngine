#include "TerrainECS/TerrainSurfacePageDriver.h"

#include "TerrainECS/HeightPageStoreLoader.h"
#include "TerrainECS/PageLevelRule.h"
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
#include "Logger/Logger.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStoreReader.h"

#include <algorithm>
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

// The height pages a surface page reads: the terrain's generated lattice or its cooked store, with
// the terrain's modifier overlay.
struct SurfaceHeightSource
{
    std::shared_ptr<const PageStreaming::GeneratedHeightPageProvider> Generated;
    std::shared_ptr<const PageStreaming::PageStoreReader> Store;
    uint64 Key = 0;
};

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

SurfaceHeightPageFetch MakeHeightFetch(const SurfaceHeightSource& source, const TiledTerrainData& tiled)
{
    return [source, &tiled](const PageAddress& address, std::span<float32> samples) {
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
        if (tiled.PageOverlay)
            tiled.PageOverlay->ApplyToPage(address, samples);
        return true;
    };
}

// The first pinned level of `tiled`'s height pyramid (FirstPinnedPageLevel), at least level 1.
uint32 FirstPinnedSurfaceLevel(const TiledTerrainData& tiled, std::span<const PageStreaming::PageStoreLevel> levels,
                               uint32 cacheSlots)
{
    PageRequestTerrain shape;
    shape.OriginX = tiled.WorldOriginX;
    shape.OriginZ = tiled.WorldOriginZ;
    shape.Level0TexelX = tiled.Config.TileWorldSize / static_cast<float32>(tiled.Config.TileConfig.HeightmapWidth - 1u);
    shape.Level0TexelZ = shape.Level0TexelX;
    shape.SamplesX = levels.front().SamplesX;
    shape.SamplesZ = levels.front().SamplesZ;
    shape.CacheSlots = cacheSlots;
    return std::max(1u, FirstPinnedPageLevel(shape, levels));
}

float64 MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<float64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

struct TerrainSurfacePageDriver::PlaneStore
{
    PageFieldKind Plane = PageFieldKind::Splat;
    std::filesystem::path File;
    std::unique_ptr<TerrainSurfacePageBaker> Baker;
};

struct TerrainSurfacePageDriver::TerrainStores
{
    TiledTerrainHandle Handle;
    GUID Scene;
    std::string Tag;
    uint64 HeightKey = 0;
    TerrainColorGroundWrite Write;
    uint64 SeenVersion = 0;
    std::vector<PlaneStore> Planes;
    std::vector<PageAddress> Pending; // the level-0 pages every plane still bakes, the next at the back
    // The edits since the last settle: the level-0 rect they changed, or the whole surface.
    std::optional<PageStreaming::PageSampleRect> Unsettled;
    bool UnsettledWhole = false;
    uint32 QuietFrames = 0;
    // The bake run since Pending was last empty, for its log line.
    std::size_t RunPages = 0;
    std::chrono::steady_clock::time_point RunStart;
    float64 RunLongestFrameMs = 0.0;
    bool RunIsColdOpen = false;
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

// The level-0 pages marked in `needed`, the first Morton code at the back: each four pages taken
// from the back are a block of 2 x 2 pages that share most of the height pages they read.
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

TerrainSurfacePageDriver::TerrainSurfacePageDriver() = default;
TerrainSurfacePageDriver::~TerrainSurfacePageDriver() = default;

const TerrainSurfacePageDriver::TerrainStores* TerrainSurfacePageDriver::Find(const TiledTerrainHandle& terrain) const
{
    for (const std::unique_ptr<TerrainStores>& stores : m_Terrains)
        if (stores->Handle == terrain)
            return stores.get();
    return nullptr;
}

std::size_t TerrainSurfacePageDriver::PendingPages(const TiledTerrainHandle& terrain) const
{
    const TerrainStores* stores = Find(terrain);
    return stores ? stores->Pending.size() : 0u;
}

std::unique_ptr<TerrainSurfacePageDriver::TerrainStores> TerrainSurfacePageDriver::OpenStores(const TerrainFrame& frame,
                                                                                              bool rebuild)
{
    const auto start = std::chrono::steady_clock::now();
    const SurfaceHeightSource source = ResolveHeightSource(frame.Service, frame.Tiled, frame.Tag, true);
    auto stores = std::make_unique<TerrainStores>();
    stores->Handle = frame.Handle;
    stores->Scene = frame.Scene;
    stores->Tag = std::string(frame.Tag);
    stores->HeightKey = source.Key;
    stores->Write = frame.Write;
    stores->SeenVersion = frame.Tiled.SurfaceVersion;
    stores->Seen = true;
    PageStreaming::PageStoreLevel level0;
    std::vector<uint8> needed;
    for (const PageFieldKind plane : kSurfacePlanes)
    {
        const uint64 key = frame.System.SurfaceStoreKey(frame.Tiled, plane, source.Key, frame.OriginY, frame.Write);
        if (key == 0)
        {
            RemoveSurfaceStores(frame.Directory, frame.Scene, frame.Tag, plane);
            continue;
        }
        PlaneStore store;
        store.Plane = plane;
        store.File = SurfaceStoreFile(frame.Directory, frame.Scene, frame.Tag, plane, key);
        store.Baker = std::make_unique<TerrainSurfacePageBaker>(frame.System, frame.Tiled, plane,
                                                                MakeHeightFetch(source, frame.Tiled), frame.Pool);
        store.Baker->SetGroundWrite(frame.Write);
        const PageStreaming::PageStoreHeader header = MakeSurfaceStoreHeader(frame.Tiled, plane, key);
        const std::vector<PageStreaming::PageStoreLevel> levels =
            PageStreaming::BuildPageStoreLevels(header.SamplesX, header.SamplesZ);
        if (needed.empty())
        {
            level0 = levels.front();
            needed.assign(static_cast<std::size_t>(level0.PagesX) * level0.PagesZ, 0u);
        }
        PageStreaming::PageStoreReader existing;
        std::error_code missingCache;
        const bool current = !rebuild && existing.Open(store.File, plane).empty() &&
                             existing.Layout().Header.Key == key &&
                             (plane != PageFieldKind::Normal ||
                              std::filesystem::exists(SurfaceSamplesCachePath(store.File), missingCache));
        if (current)
            MarkMissingPages(store.File, plane, level0, needed);
        else
        {
            RemoveSurfaceStores(frame.Directory, frame.Scene, frame.Tag, plane);
            const uint32 pinned =
                FirstPinnedSurfaceLevel(frame.Tiled, levels, frame.Service.GetHeightPages().Profile().Slots);
            if (std::string error = store.Baker->CreateStore(store.File, key, pinned); !error.empty())
            {
                Logger::Log::Warning("Surface pages: the {} store of terrain '{}' was not made: {}",
                                     SurfacePlaneName(plane), frame.Tag, error);
                continue;
            }
            std::fill(needed.begin(), needed.end(), uint8{1});
        }
        stores->Planes.push_back(std::move(store));
    }
    if (!stores->Planes.empty())
        stores->Pending = PendingInMortonOrder(level0, needed);
    stores->RunPages = stores->Pending.size();
    stores->RunStart = start;
    stores->RunIsColdOpen = true;
    Logger::Log::Info("Surface pages: terrain '{}' opened {} stores in {:.1f} ms, {} level-0 pages to bake",
                      frame.Tag, stores->Planes.size(), MillisecondsSince(start), stores->Pending.size());
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
    for (const PlaneStore& store : stores.Planes)
        for (const PageAddress& page : store.Baker->Level0PagesReading(changed))
            if (std::find(pages.begin(), pages.end(), page) == pages.end())
                pages.push_back(page);
    if (stores.Pending.empty())
    {
        stores.RunPages = 0;
        stores.RunStart = std::chrono::steady_clock::now();
        stores.RunLongestFrameMs = 0.0;
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

void TerrainSurfacePageDriver::BakePending(TerrainStores& stores)
{
    if (stores.Pending.empty())
        return;
    const auto start = std::chrono::steady_clock::now();
    const std::size_t count = std::min(kLevel0PagesAFrame, stores.Pending.size());
    const std::vector<PageAddress> pages(stores.Pending.end() - static_cast<std::ptrdiff_t>(count),
                                         stores.Pending.end());
    stores.Pending.resize(stores.Pending.size() - count);
    for (PlaneStore& store : stores.Planes)
        if (std::string error = store.Baker->BakeLevel0Pages(store.File, pages); !error.empty())
            Logger::Log::Warning("Surface pages: the {} store of terrain '{}' missed level-0 pages: {}",
                                 SurfacePlaneName(store.Plane), stores.Tag, error);
    stores.RunLongestFrameMs = std::max(stores.RunLongestFrameMs, MillisecondsSince(start));
    if (!stores.Pending.empty())
        return;
    constexpr std::string_view kRunDone =
        "Surface pages: terrain '{}' baked {} level-0 pages over {} planes in {:.1f} ms, at most {:.1f} ms a frame";
    if (stores.RunIsColdOpen)
        Logger::Log::Info(kRunDone, stores.Tag, stores.RunPages, stores.Planes.size(), MillisecondsSince(stores.RunStart),
                          stores.RunLongestFrameMs);
    else
        Logger::Log::Debug(kRunDone, stores.Tag, stores.RunPages, stores.Planes.size(),
                           MillisecondsSince(stores.RunStart), stores.RunLongestFrameMs);
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

    if (found != m_Terrains.end() && ((*found)->HeightKey != source.Key || (*found)->Scene != terrain.BakeOriginScene ||
                                      (*found)->Tag != tag))
    {
        m_Terrains.erase(found);
        found = m_Terrains.end();
    }
    if (found == m_Terrains.end())
    {
        m_Terrains.push_back(OpenStores(frame, false));
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
    // their key, and a rect bakes again the pages that read it.
    if (stores.Unsettled)
    {
        if (++stores.QuietFrames < kSurfaceSettleFrames)
            return;
        if (stores.UnsettledWhole)
        {
            *found = OpenStores(frame, true);
            return;
        }
        QueueEdit(stores, *stores.Unsettled);
        stores.Unsettled.reset();
    }
    BakePending(stores);
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
    std::erase_if(m_Terrains, [](const std::unique_ptr<TerrainStores>& stores) { return !stores->Seen; });
}

} // namespace GameEngine::TerrainECS
