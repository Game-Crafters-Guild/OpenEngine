#include "TerrainECS/TerrainPages.h"

#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainService.h"
#include "Components/Transform.h"
#include "Core/CpuProfiler.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "CBTTerrain/CBTLayout.h"
#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageTableEntry.h"
#include "Terrain/TerrainTypes.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iterator>

namespace GameEngine::TerrainECS
{

namespace
{

// The terrain UV along an axis of `samples` over which the surface reads a level-`level` sample of
// [first, last]: the bilinear tap between two samples of that level reads both, so one sample of
// the level on each side.
void ReachAxisUV(uint32 first, uint32 last, uint32 level, uint32 samples, float32& outMin, float32& outMax)
{
    const float32 step = static_cast<float32>(1u << level);
    const float32 inv = samples > 1u ? 1.0f / static_cast<float32>(samples - 1u) : 0.0f;
    outMin = std::max(0.0f, (static_cast<float32>(first) - 1.0f) * step * inv);
    outMax = std::min(1.0f, (static_cast<float32>(last) + 1.0f) * step * inv);
}

// The CBT's face UV (CBT_FaceUVFromCube: (a + 1) / 2 of the gnomonic coordinate a) of equal-angle
// face UV t (the page store's: (s + 1) / 2 of the equal-angle coordinate s). Both increase with t,
// so a rect maps to the rect of its corners (planets 3.2).
float32 CubeFaceUV(float32 t)
{
    const float64 gnomonic = PageStreaming::GnomonicFaceCoordinate(2.0 * static_cast<float64>(t) - 1.0);
    return static_cast<float32>(0.5 * (gnomonic + 1.0));
}

// Whether a page's footprint reaches a side of its face, and its reach along that side (equal-angle
// face UV).
struct FaceSideReach
{
    bool Touches = false;
    PageStreaming::PageFaceSide Side = PageStreaming::PageFaceSide::MinZ;
    float32 AlongMin = 0.0f;
    float32 AlongMax = 0.0f;
};

// Grows `rect` (or starts it, when `any` is false) to hold terrain-UV rect [minU, maxU] x [minV, maxV].
void UniteDirty(uint32 terrainKey, uint32 generation, float32 minU, float32 minV, float32 maxU, float32 maxV,
                HeightPageDirtyRect& rect, bool& any)
{
    if (!any)
    {
        rect = HeightPageDirtyRect{terrainKey, generation, minU, minV, maxU, maxV};
        any = true;
        return;
    }
    rect.MinU = std::min(rect.MinU, minU);
    rect.MinV = std::min(rect.MinV, minV);
    rect.MaxU = std::max(rect.MaxU, maxU);
    rect.MaxV = std::max(rect.MaxV, maxV);
}

// The page table's budget under `profile`.
PageTableBudget TableBudget(const HeightPageProfile& profile)
{
    return PageTableBudget{profile.DenseTableWords, profile.TableMidBlocks, profile.TableLeafBlocks};
}

// The residency's budget under `profile`: the one CPU budget of decoded pages (design D5's CPU
// cache) and the synchronous CPU window fetches a frame (design D3).
PageStreaming::PageResidencyBudget ResidencyBudget(const HeightPageProfile& profile)
{
    PageStreaming::PageResidencyBudget budget;
    budget.CpuBytes = profile.CpuBytes;
    budget.CpuSyncFetchPages = profile.CpuSyncFetchPages;
    return budget;
}

// The level-0 pages of one CPU window.
constexpr uint64 kPagesPerWindow = 4;
// Body windows may hold 1 / this of the CPU height pages' budget (desktop 64 MiB: 62 bodies; Steam
// Deck 32 MiB: 31), and so may a whole level 0; the rest stays with the camera windows and the
// pinned levels.
constexpr uint64 kBodyWindowBudgetDivisor = 4;
// A terrain whose level 0 has at most one tile's pages (kMaxTileResolution samples a side: 9 x 9)
// keeps its whole level 0 in its CPU window, so every point of it has a page collider, while the
// level fits 1 / kBodyWindowBudgetDivisor of the CPU budget: 81 float pages are 5.2 MiB, inside the
// 16 MiB of desktop and the 8 MiB of Steam Deck.
constexpr uint32 kTilePagesPerAxis =
    (Terrain::kMaxTileResolution + PageStreaming::kPageOwnedSamples - 1u) / PageStreaming::kPageOwnedSamples;
constexpr uint64 kWholeLevel0WindowPages = static_cast<uint64>(kTilePagesPerAxis) * kTilePagesPerAxis;

// The page along one axis under page coordinate `c` (level-0 pages from the footprint's corner) and
// its neighbor on the nearer side, both inside [0, pages).
void WindowAxis(float32 c, uint32 pages, uint32& own, uint32& neighbor)
{
    const int64 last = static_cast<int64>(pages) - 1;
    const int64 under = std::clamp(static_cast<int64>(std::floor(c)), int64{0}, last);
    const int64 nearer = c - static_cast<float32>(under) < 0.5f ? under - 1 : under + 1;
    own = static_cast<uint32>(under);
    neighbor = static_cast<uint32>(std::clamp(nearer, int64{0}, last));
}

// The level-0 pages of the CPU window around `center` (design D3: two pages wide): on each axis the
// page under the center and its neighbor on the nearer side, so the center has at least half a page
// of level-0 samples on every side; the page under the center first. Nothing for a center more than
// a page off the footprint.
void AppendWindowPages(const PageRequestTerrain& shape, const PageStreaming::PageStoreLevel& level0,
                       const Mathematics::Vector3& center, std::vector<PageStreaming::PageAddress>& out)
{
    const float32 pageMetersX = shape.Level0TexelX * static_cast<float32>(PageStreaming::kPageOwnedSamples);
    const float32 pageMetersZ = shape.Level0TexelZ * static_cast<float32>(PageStreaming::kPageOwnedSamples);
    const float32 x = (center.x - shape.OriginX) / pageMetersX;
    const float32 z = (center.z - shape.OriginZ) / pageMetersZ;
    const float32 pagesX = static_cast<float32>(level0.PagesX);
    const float32 pagesZ = static_cast<float32>(level0.PagesZ);
    if (!(x >= -1.0f && x <= pagesX + 1.0f && z >= -1.0f && z <= pagesZ + 1.0f))
        return; // off the footprint, or a degenerate placement
    uint32 ownX = 0, nearX = 0, ownZ = 0, nearZ = 0;
    WindowAxis(x, level0.PagesX, ownX, nearX);
    WindowAxis(z, level0.PagesZ, ownZ, nearZ);
    out.push_back(PageStreaming::PageAddress{0, 0, ownX, ownZ});
    out.push_back(PageStreaming::PageAddress{0, 0, nearX, ownZ});
    out.push_back(PageStreaming::PageAddress{0, 0, ownX, nearZ});
    out.push_back(PageStreaming::PageAddress{0, 0, nearX, nearZ});
}

// Every level-0 page, when the level has at most kWholeLevel0WindowPages pages and its height pages
// fit `cpuBytes`.
void AppendWholeLevel0(const PageStreaming::PageStoreLevel& level0, uint64 cpuBytes,
                       std::vector<PageStreaming::PageAddress>& out)
{
    const uint64 pages = static_cast<uint64>(level0.PagesX) * level0.PagesZ;
    const uint64 pageBytes = static_cast<uint64>(PageStreaming::kPageSampleCount) * sizeof(float32);
    if (pages > kWholeLevel0WindowPages || pages * pageBytes > cpuBytes)
        return;
    for (uint32 z = 0; z < level0.PagesZ; ++z)
        for (uint32 x = 0; x < level0.PagesX; ++x)
            out.push_back(PageStreaming::PageAddress{0, 0, x, z});
}

// Whether `a` and `b` hold the same points bit for bit.
bool SameBits(std::span<const Mathematics::Vector3> a, std::span<const Mathematics::Vector3> b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size_bytes()) == 0);
}

// GE_CBT_VALIDATE: log each page stream's wanted and resident pages per level whenever they change.
bool LogResidency()
{
    static const bool kLogResidency = std::getenv("GE_CBT_VALIDATE") != nullptr;
    return kLogResidency;
}

// The pages of the residency line's address lists, at most; the rest are counted.
constexpr std::size_t kLoggedAddressesPerList = 32;

// The order the residency log lists pages in: level-major, row-major.
bool LogOrder(const PageStreaming::PageAddress& a, const PageStreaming::PageAddress& b)
{
    if (a.Level != b.Level)
        return a.Level < b.Level;
    return a.Z != b.Z ? a.Z < b.Z : a.X < b.X;
}

// " L<level>(x,z)" for each of `pages`, the first `limit`, then the count left.
std::string AddressList(const std::vector<PageStreaming::PageAddress>& pages, std::size_t limit)
{
    std::string out;
    for (std::size_t i = 0; i < pages.size() && i < limit; ++i)
        out += std::format(" L{}({},{})", pages[i].Level, pages[i].X, pages[i].Z);
    if (pages.size() > limit)
        out += std::format(" +{} more", pages.size() - limit);
    return out.empty() ? std::string(" none") : out;
}

// True when every page of the levels from `firstPinned` up, on every face, has its entry.
bool PinnedPagesHaveEntries(const PageTable& table, uint32 firstPinned)
{
    const std::span<const PageStreaming::PageStoreLevel> levels = table.Levels();
    for (uint32 level = firstPinned; level < levels.size(); ++level)
        for (uint32 face = 0; face < table.Faces(); ++face)
            for (uint32 z = 0; z < levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < levels[level].PagesX; ++x)
                {
                    const PageStreaming::PageAddress page{static_cast<uint8>(face), static_cast<uint8>(level), x, z};
                    if (table.Entry(page) == PageStreaming::kNoPage)
                        return false;
                }
    return true;
}

} // namespace

HeightPageProfile PlatformHeightPageProfile()
{
#if defined(PLATFORM_STEAMDECK)
    return kSteamDeckHeightPages;
#else
    return kDesktopHeightPages;
#endif
}

std::string OverlayRefusal(std::string_view name, uint64 overlayBytes, const HeightPageProfile& profile)
{
    if (overlayBytes <= profile.OverlayByteCap)
        return {};
    constexpr float64 kBytesPerMiB = 1024.0 * 1024.0;
    return std::format("{} does not stream its height on this platform: its height modifiers reach so much of it "
                       "that their page overlay needs {} bytes ({:.1f} MiB), over the platform's cap of {} bytes "
                       "({:.1f} MiB). It draws from its height texture, and its CPU height (picks, placement, "
                       "navigation) reads it without those modifiers. Shrink the modifiers' bounds (a "
                       "terrain-wide modifier reaches every sample) or lower the terrain's samples per meter to "
                       "stream it.",
                       name, overlayBytes, static_cast<float64>(overlayBytes) / kBytesPerMiB, profile.OverlayByteCap,
                       static_cast<float64>(profile.OverlayByteCap) / kBytesPerMiB);
}

HeightPageOverlayAllowance PageOverlayAllowance(bool wouldPage, uint32 sourceX, uint32 sourceZ, uint32 latticeX,
                                                uint32 latticeZ, const HeightPageProfile& profile)
{
    if (!wouldPage)
        return {};
    return {profile.OverlayByteCap, sourceX != latticeX || sourceZ != latticeZ};
}

std::string OverlayGridRefusal(std::string_view name, uint32 storeX, uint32 storeZ, uint32 latticeX, uint32 latticeZ)
{
    return std::format("{} does not stream its height while height modifiers reach it: its heightmap's grid ({} x {} "
                       "samples) is not the terrain's tile lattice ({} x {} samples), on which the modifiers are "
                       "baked. It draws from its height texture, and its CPU height (picks, placement, navigation) "
                       "reads it without those modifiers. Import the heightmap at {} x {} samples to stream "
                       "it with its modifiers.",
                       name, storeX, storeZ, latticeX, latticeZ, latticeX, latticeZ);
}

void PackPageTableWords(const PageTable& table, const PageCacheGeometry& geometry, std::vector<uint32>& out)
{
    const std::span<const float32> fades = table.SlotFades();
    const std::span<const uint32> words = table.Words();
    out.assign(CBTTerrain::kCBTPageFadeOffsetWords + fades.size() + words.size(), 0u);
    const std::span<const PageStreaming::PageStoreLevel> levels = table.Levels();
    if (levels.empty())
        return;
    out[0] = levels.front().SamplesX;
    out[1] = levels.front().SamplesZ;
    out[2] = static_cast<uint32>(levels.size());
    out[3] = geometry.SlotsPerRow;
    out[4] = static_cast<uint32>(fades.size());
    out[5] = table.FloorLevel();
    out[6] = table.TierCount();
    out[7] = table.DirectoryOffset();
    for (std::size_t level = 0; level < levels.size(); ++level)
    {
        uint32* shape = out.data() + CBTTerrain::kCBTPageHeaderWords + 4u * level;
        shape[0] = table.LevelOffsets()[level];
        shape[1] = levels[level].PagesX;
        shape[2] = levels[level].PagesZ;
    }
    uint32* fadeWords = out.data() + CBTTerrain::kCBTPageFadeOffsetWords;
    for (std::size_t slot = 0; slot < fades.size(); ++slot)
        fadeWords[slot] = std::bit_cast<uint32>(fades[slot]);
    std::memcpy(fadeWords + fades.size(), words.data(), words.size() * sizeof(uint32));
}

TerrainPages::TerrainPages(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io,
                           const HeightPageProfile& profile, const SurfacePageProfile& surfaceProfile)
    : m_Residency(pool, io, ResidencyBudget(profile))
    , m_Profile(profile)
    , m_SlotCount(profile.Slots)
    , m_Geometry(MakePageCacheGeometry(profile.Slots))
    , m_Surface(m_Residency, surfaceProfile, TableBudget(profile), m_NextStream)
{
}

void TerrainPages::ReportRefusal(uint32 terrain, const std::string& refusal)
{
    std::string& logged = m_Refusals.GetOrInsert(terrain);
    if (logged == refusal)
        return;
    logged = refusal;
    Logger::Log::Warning("Terrain pages: {}", refusal);
}

void TerrainPages::Configure(PagedTerrain& terrain, const PagedTerrainRequest& request)
{
    const bool sameSource = terrain.Stream != 0 && terrain.SourceIdentity == request.SourceIdentity &&
                            terrain.Shape.SamplesX == request.Shape.SamplesX &&
                            terrain.Shape.SamplesZ == request.Shape.SamplesZ;
    if (terrain.Stream == 0)
        terrain.Stream = m_NextStream++;
    terrain.Identity = request.SourceIdentity;
    terrain.Label = request.Name.empty() ? std::format("terrain {}", request.Terrain) : std::string(request.Name);
    terrain.Shape = request.Shape;
    terrain.Shape.CacheSlots = m_SlotCount;
    terrain.Requester.Configure(terrain.Shape, PageRequestArea::Disc);
    const PageRequestTerrain& shape = terrain.Shape;
    terrain.Table.Reset(shape.SamplesX, shape.SamplesZ, 1u, m_SlotCount, TableBudget(m_Profile));
    terrain.Packed.Update(terrain.Table, m_Geometry, ++m_TableVersion);
    // The stream hangs on the source alone, not on the GPU cache: a GPU restart (frames in flight
    // changed) keeps its loaded pages, so the CPU readers keep theirs.
    if (!sameSource)
    {
        m_Residency.Configure(terrain.Stream, kHeightPageField, request.Source);
        terrain.SourceIdentity = request.SourceIdentity;
    }
    terrain.Overlay = request.Overlay;
    terrain.OverlayVersion = request.OverlayVersion;
    terrain.OverlayChanged = request.OverlayChanged;
    terrain.OverlayChangedSince = request.OverlayChangedSince;
    terrain.GpuPages = request.GpuPages;
    // The intent: the CBT logs the frame from which it draws the pages (CBTRenderFeature).
    Logger::Log::Info("Terrain pages: {} (terrain {} generation {}) streams its height pages ({} x {} samples, {} "
                      "levels){}",
                      terrain.Label, request.Terrain, request.TerrainGeneration, shape.SamplesX, shape.SamplesZ,
                      terrain.Table.Levels().size(), request.GpuPages ? "" : " for the CPU only");
    // The terrain's height changes source (from its texture, or from its previous pages).
    if (terrain.GpuPages)
        AddWholeTerrainDirty(request.Terrain, terrain);
}

void TerrainPages::AddWholeTerrainDirty(uint32 terrainKey, const PagedTerrain& terrain)
{
    m_DirtyRects.push_back(HeightPageDirtyRect{terrainKey, terrain.Generation, 0.0f, 0.0f, 1.0f, 1.0f});
    m_ChangeRects.push_back(PageChangeRect{terrainKey});
}

bool TerrainPages::NeedsSource(uint32 terrain, uint64 sourceIdentity) const
{
    const PagedTerrain* paged = m_Terrains.Find(terrain);
    return !paged || paged->Identity != sourceIdentity;
}

void TerrainPages::Request(const PagedTerrainRequest& request, std::span<const Mathematics::Vector3> cameras,
                           const PageLevelView& view)
{
    if (m_FramesInFlight == 0)
        return; // the field's cache is configured by the first Update
    const bool inserted = !m_Terrains.Contains(request.Terrain);
    PagedTerrain& terrain = m_Terrains.GetOrInsert(request.Terrain);
    terrain.Generation = request.TerrainGeneration;
    if (inserted || terrain.Identity != request.SourceIdentity || request.Shape.SamplesX != terrain.Shape.SamplesX ||
        request.Shape.SamplesZ != terrain.Shape.SamplesZ)
    {
        Configure(terrain, request);
    }
    else if (terrain.GpuPages != request.GpuPages)
    {
        // The GPU starts or stops drawing the pages (its texture meanwhile); the CPU pages stay. The table
        // stays too: Update keeps it and the slots current while the GPU draws the texture.
        terrain.GpuPages = request.GpuPages;
        terrain.Requester.Configure(terrain.Shape, PageRequestArea::Disc); // the next Build requests anew
        terrain.Packed.Update(terrain.Table, m_Geometry, ++m_TableVersion);
        AddWholeTerrainDirty(request.Terrain, terrain);
        Logger::Log::Info("Terrain pages: {} (terrain {} generation {}) {}", terrain.Label, request.Terrain,
                          request.TerrainGeneration,
                          terrain.GpuPages ? "draws its height pages again"
                                           : "streams its height pages for the CPU only and draws its texture");
    }
    if (!SamePagePlacement(request.Shape, terrain.Shape))
    {
        // Moved or rescaled: the resident pages are keyed by terrain UV and stay valid, but the
        // requester asks for the pages under the cameras at the new placement, and every bisector's
        // world corners move with it.
        terrain.Shape = request.Shape;
        terrain.Shape.CacheSlots = m_SlotCount;
        terrain.Requester.Configure(terrain.Shape, PageRequestArea::Disc);
        AddWholeTerrainDirty(request.Terrain, terrain);
    }
    if (request.OverlayVersion != terrain.OverlayVersion)
    {
        PageStreaming::PageFaceRects changed;
        changed[0] = request.OverlayChanged;
        const PageStreaming::HeightPageLayer* held = terrain.Overlay.get();
        const PageStreaming::HeightPageLayer* requested = request.Overlay.get();
        PageStreaming::RefreshLayeredPages(m_Residency, terrain.Stream, std::span(&held, 1), terrain.OverlayVersion,
                                           std::span(&requested, 1), changed, request.OverlayChangedSince,
                                           PageStreaming::kPageApronSamples, m_FaceRects);
        terrain.Overlay = request.Overlay;
        terrain.OverlayVersion = request.OverlayVersion;
        terrain.OverlayChanged = request.OverlayChanged;
        terrain.OverlayChangedSince = request.OverlayChangedSince;
    }
    if (terrain.GpuPages)
        m_Refusals.Erase(request.Terrain); // a refused terrain streams its CPU pages only
    terrain.Requested = true;
    terrain.OriginY = request.OriginY;
    terrain.HeightScale = request.HeightScale;
    RequestCpuWindows(terrain, cameras);
    m_Wants.clear();
    if (terrain.GpuPages)
    {
        SetDiscCameras(cameras);
        if (!terrain.Requester.Build(m_Cameras, view, m_Wants))
            return; // a parked camera: the last request stands
    }
    m_Residency.Request(terrain.Stream, m_Wants);
    terrain.WantedPages = m_Wants;
}

void TerrainPages::RequestPlanet(const PlanetPagedRequest& request, std::span<const Mathematics::Vector3> cameras,
                                 const PageLevelView& view)
{
    if (m_FramesInFlight == 0)
        return; // the field's cache is configured by the first Update
    PagedPlanet& planet = m_Planets.GetOrInsert(request.Planet);
    if (planet.Stream == 0 || planet.Identity != request.SourceIdentity)
    {
        assert(request.Source.Generated && "a planet's first request, or a new relief, carries its source");
        if (!request.Source.Generated)
            return;
        if (planet.Stream == 0)
            planet.Stream = m_NextStream++;
        planet.Identity = request.SourceIdentity;
        planet.Label = request.Name.empty() ? std::format("planet {}", request.Planet) : std::string(request.Name);
        m_Residency.Configure(planet.Stream, kHeightPageField, request.Source);
        PlanetPageRequestTerrain shape = request.Shape;
        shape.CacheSlots = m_SlotCount;
        shape.Heights = request.Source.Generated;
        planet.Requester.Configure(shape, PageRequestArea::Disc);
        planet.Shape = shape;
        planet.Layout = ComputePlanetChartLayout(shape.ReferenceRadius, shape.FaceSamples);
        planet.ReferenceRadius = shape.ReferenceRadius;
        planet.CpuWindowOf.clear();
        planet.BodyPagesBuilt = false;
        planet.GpuPages = request.GpuPages;
        // A restarted stream loads every page with the layers.
        planet.Overlay = FittingOverlay(planet, request.Overlay);
        planet.Modifiers = FittingOverlay(planet, request.Modifiers);
        planet.OverlayVersion = request.OverlayVersion;
        ResetPlanetTable(planet);
        Logger::Log::Info("Terrain pages: {} streams its height pages on six faces ({} samples a side, {} levels, "
                          "levels {} and coarser pinned){}",
                          planet.Label, shape.FaceSamples, planet.Requester.Levels().size(),
                          planet.Requester.FirstPinnedLevel(), planet.GpuPages ? "" : "; nothing draws them");
    }
    if (request.OverlayVersion != planet.OverlayVersion)
    {
        std::shared_ptr<const PageStreaming::HeightPageLayer> overlay = FittingOverlay(planet, request.Overlay);
        std::shared_ptr<const PageStreaming::HeightPageLayer> modifiers = FittingOverlay(planet, request.Modifiers);
        const std::array<const PageStreaming::HeightPageLayer*, 2> held = {planet.Modifiers.get(), planet.Overlay.get()};
        const std::array<const PageStreaming::HeightPageLayer*, 2> requested = {modifiers.get(), overlay.get()};
        PageStreaming::RefreshLayeredPages(m_Residency, planet.Stream, held, planet.OverlayVersion, requested,
                                           request.OverlayChanged, request.OverlayChangedSince,
                                           PageStreaming::kPageApronSamples, m_FaceRects);
        planet.Overlay = std::move(overlay);
        planet.Modifiers = std::move(modifiers);
        planet.OverlayVersion = request.OverlayVersion;
    }
    planet.RenderTerrain = request.RenderTerrain;
    planet.Center = request.Center;
    planet.Requested = true;
    SetDiscCameras(cameras);
    planet.FitPasses = 0;
    if (request.FitToSlots)
        planet.FitPasses = planet.Requester.BuildWithinSlots(m_Cameras, view, m_SlotCount, m_Wants).Passes;
    else
        planet.Requester.Build(m_Cameras, view, m_Wants);
    m_Residency.Request(planet.Stream, m_Wants);
    planet.WantedPages = m_Wants;
    RequestPlanetCpuWindows(planet, request.Center, cameras);
}

void TerrainPages::RequestPlanetSurface(const PlanetSurfacePagedRequest& request,
                                        std::span<const PageRequestCamera> cameras, const PageLevelView& view)
{
    const PagedPlanet* planet = m_Planets.Find(request.Planet);
    if (!planet || !planet->Requested || !planet->GpuPages || planet->RenderTerrain == kNoRenderTerrain)
        return;
    PlanetSurfaceHeights heights;
    heights.RenderTerrain = planet->RenderTerrain;
    heights.Identity = planet->Identity;
    heights.Shape = planet->Shape;
    heights.Layers = {planet->Modifiers, planet->Overlay};
    heights.LayerVersion = planet->OverlayVersion;
    m_Surface.RequestPlanet(request, heights, cameras, view);
}

// The geometry's cameras: each looks every way, so the requester refines the whole disc.
void TerrainPages::SetDiscCameras(std::span<const Mathematics::Vector3> cameras)
{
    m_Cameras.clear();
    for (const Mathematics::Vector3& camera : cameras)
        m_Cameras.push_back(PageRequestCamera{camera, {}, 0.0f, false});
}

void TerrainPages::RequestPlanetCpuWindows(PagedPlanet& planet, const Mathematics::Vector3& center,
                                           std::span<const Mathematics::Vector3> cameras)
{
    GE_CPU_PROFILE_SCOPE("Terrain.PlanetCpuWindows");
    // The window pages are a function of these points: while none moves, the pages stay and the
    // residency gets the same window again.
    if (!planet.BodyPagesBuilt || !SameBits(planet.BodyPagesCameras, cameras) ||
        !SameBits(planet.BodyPagesBodies, m_BodyWindowCenters) ||
        !SameBits(std::span(&planet.BodyPagesCenter, 1), std::span(&center, 1)))
        BuildPlanetCpuWindow(planet, center, cameras);
    m_Residency.RequestCpu(planet.Stream, static_cast<uint32>(planet.Requester.Levels().size()), planet.CpuWindow);
}

void TerrainPages::BuildPlanetCpuWindow(PagedPlanet& planet, const Mathematics::Vector3& center,
                                        std::span<const Mathematics::Vector3> cameras)
{
    GE_CPU_PROFILE_SCOPE("Terrain.PlanetCpuWindowPages");
    planet.BodyPages.clear();
    for (const Mathematics::Vector3& camera : cameras)
        AppendPlanetWindowPages(planet.Layout, camera, planet.BodyPages);
    OrderBodyWindows(cameras, center);
    for (const auto& [squaredDistance, body] : m_BodyOrder)
        AppendPlanetWindowPages(planet.Layout, m_BodyWindowCenters[body] - center, planet.BodyPages);
    // The pages the bodies read change only when the body pages do; RequestCpu keeps a repeated page
    // at its first place.
    if (planet.BodyPages != planet.CpuWindowOf)
    {
        planet.CpuWindow = planet.BodyPages;
        for (const PageStreaming::PageAddress& page : planet.BodyPages)
        {
            m_WindowPages.clear();
            AppendPlanetPageBodyReads(planet.Layout, planet.ReferenceRadius, page.Face, page.X, page.Z, m_WindowPages);
            planet.CpuWindow.insert(planet.CpuWindow.end(), m_WindowPages.begin(), m_WindowPages.end());
        }
        planet.CpuWindowOf = planet.BodyPages;
    }
    planet.BodyPagesCameras.assign(cameras.begin(), cameras.end());
    planet.BodyPagesBodies.assign(m_BodyWindowCenters.begin(), m_BodyWindowCenters.end());
    planet.BodyPagesCenter = center;
    planet.BodyPagesBuilt = true;
}

bool TerrainPages::FindPlanetCpuView(uint32 planet, PlanetCpuView& out) const
{
    const PagedPlanet* paged = m_Planets.Find(planet);
    if (!paged || paged->Stream == 0)
        return false;
    out.Epoch = paged->Identity;
    out.Layout = paged->Layout;
    out.ReferenceRadius = paged->ReferenceRadius;
    out.BodyPages = paged->BodyPages;
    out.LayerVersion = paged->OverlayVersion;
    return true;
}

bool TerrainPages::PlanetPageHeights(uint32 planet, const PageStreaming::PageAddress& address,
                                     std::vector<float32>& heights) const
{
    const PagedPlanet* paged = m_Planets.Find(planet);
    if (!paged || paged->Stream == 0)
        return false;
    const std::vector<uint8>* bytes = m_Residency.Bytes(paged->Stream, address);
    if (bytes == nullptr || bytes->size() != PageStreaming::kPageSampleCount * sizeof(float32))
        return false;
    heights.resize(PageStreaming::kPageSampleCount);
    std::memcpy(heights.data(), bytes->data(), bytes->size());
    ApplyPlanetLayers(*paged, address, heights);
    return true;
}

SurfaceHeightQuery TerrainPages::PlanetSurfaceQuery(uint32 planet) const
{
    const PagedPlanet* paged = m_Planets.Find(planet);
    if (!paged || paged->Stream == 0)
        return {};
    return SurfaceHeightQuery(m_Residency.CpuPages(paged->Stream), paged->Requester.Levels(),
                              SurfaceHeightSphere{paged->Center, paged->ReferenceRadius, paged->Layout.FaceSamples,
                                                  paged->Modifiers.get(), paged->Overlay.get()});
}

void TerrainPages::ApplyPlanetLayers(const PagedPlanet& planet, const PageStreaming::PageAddress& address,
                                     std::span<float32> heights)
{
    if (planet.Modifiers)
        planet.Modifiers->ApplyToPage(address, heights);
    if (planet.Overlay)
        planet.Overlay->ApplyToPage(address, heights);
}

std::shared_ptr<const PageStreaming::HeightPageLayer>
TerrainPages::FittingOverlay(const PagedPlanet& planet,
                             const std::shared_ptr<const PageStreaming::HeightPageLayer>& layer) const
{
    if (!layer || layer->FitsLevels(planet.Requester.Levels()))
        return layer;
    // Its samples are another lattice's: they would land at other places and miss levels.
    Logger::Log::Warning("Terrain pages: {} ignores its height page layer: it was made for another lattice "
                         "(the planet's radius or Samples Per Meter changed); set them back to show it",
                         planet.Label);
    return nullptr;
}

uint64 TerrainPages::PlanetOverlayVersion(uint32 planet) const
{
    const PagedPlanet* paged = m_Planets.Find(planet);
    return paged ? paged->OverlayVersion : 0u;
}

bool TerrainPages::NeedsPlanetSource(uint32 planet, uint64 sourceIdentity) const
{
    const PagedPlanet* paged = m_Planets.Find(planet);
    return !paged || paged->Identity != sourceIdentity;
}

void CountWantedPages(const PageStreaming::PageCache& cache, uint32 stream,
                      std::span<const PageStreaming::PageWant> wanted, uint32 levels, PageStreamCounts& out)
{
    out.Levels.assign(levels, PageLevelCounts{});
    for (const PageStreaming::PageWant& want : wanted)
    {
        const PageStreaming::PageAddress& address = want.Address;
        if (address.Level >= levels)
            continue;
        PageLevelCounts& level = out.Levels[address.Level];
        ++level.Wanted;
        if (cache.SlotOf(PageStreaming::CachedPage{stream, address}) != PageStreaming::kNoResidentSlot)
            ++level.Resident;
    }
}

void TerrainPages::StreamCounts(std::vector<PageStreamCounts>& out) const
{
    out.clear();
    const PageStreaming::PageCache& cache = m_Residency.FieldCache(kHeightPageField);
    for (const auto& [key, terrain] : m_Terrains)
    {
        PageStreamCounts& counts = out.emplace_back();
        counts.Label = terrain.Label;
        counts.PagesProduced = m_Residency.PagesProduced(terrain.Stream);
        CountWantedPages(cache, terrain.Stream, terrain.WantedPages,
                         static_cast<uint32>(terrain.Table.Levels().size()), counts);
    }
    for (const auto& [key, planet] : m_Planets)
    {
        PageStreamCounts& counts = out.emplace_back();
        counts.Label = planet.Label;
        counts.Planet = true;
        counts.FitPasses = planet.FitPasses;
        counts.PagesProduced = m_Residency.PagesProduced(planet.Stream);
        CountWantedPages(cache, planet.Stream, planet.WantedPages,
                         static_cast<uint32>(planet.Requester.Levels().size()), counts);
        AppendPlanetTable(planet, counts);
    }
    m_Surface.StreamCounts(out);
}

void TerrainPages::AppendPlanetTable(const PagedPlanet& planet, PageStreamCounts& out) const
{
    const std::span<const PageStreaming::PageStoreLevel> levels = planet.Table.Levels();
    const uint32 faces = planet.Table.Faces();
    if (levels.empty())
        return;
    out.Published = planet.Published;
    out.PinnedLevel = std::min(planet.Requester.FirstPinnedLevel(), static_cast<uint32>(levels.size()) - 1u);
    out.PinnedPagesX = levels[out.PinnedLevel].PagesX;
    out.PinnedPagesZ = levels[out.PinnedLevel].PagesZ;
    out.Faces.assign(faces, HeightPageFaceTable{});
    for (HeightPageFaceTable& face : out.Faces)
        face.EntriesPerLevel.assign(levels.size(), 0u);
    std::vector<PageStreaming::PageAddress> resident;
    planet.Table.AppendResident(resident);
    for (const PageStreaming::PageAddress& address : resident)
        if (address.Face < faces && address.Level < levels.size())
            ++out.Faces[address.Face].EntriesPerLevel[address.Level];
    // The level-0 texel at a pinned page's center, then the page holding it on each level from level 0.
    const uint32 span = PageStreaming::kPageOwnedSamples << out.PinnedLevel;
    for (uint32 face = 0; face < faces; ++face)
    {
        std::vector<uint8>& resolved = out.Faces[face].ResolvedLevel;
        resolved.assign(static_cast<std::size_t>(out.PinnedPagesX) * out.PinnedPagesZ,
                        static_cast<uint8>(levels.size()));
        for (uint32 z = 0; z < out.PinnedPagesZ; ++z)
            for (uint32 x = 0; x < out.PinnedPagesX; ++x)
            {
                const uint64 texelX = std::min<uint64>(uint64(x) * span + span / 2u, levels[0].SamplesX - 1u);
                const uint64 texelZ = std::min<uint64>(uint64(z) * span + span / 2u, levels[0].SamplesZ - 1u);
                for (uint32 level = 0; level < levels.size(); ++level)
                {
                    const uint64 pageSpan = uint64(PageStreaming::kPageOwnedSamples) << level;
                    const PageStreaming::PageAddress page{
                        static_cast<uint8>(face), static_cast<uint8>(level),
                        static_cast<uint32>(std::min<uint64>(texelX / pageSpan, levels[level].PagesX - 1u)),
                        static_cast<uint32>(std::min<uint64>(texelZ / pageSpan, levels[level].PagesZ - 1u))};
                    if (planet.Table.Entry(page) != PageStreaming::kNoPage)
                    {
                        resolved[static_cast<std::size_t>(z) * out.PinnedPagesX + x] = static_cast<uint8>(level);
                        break;
                    }
                }
            }
    }
}

void TerrainPages::RequestCpuWindows(const PagedTerrain& terrain, std::span<const Mathematics::Vector3> cameras)
{
    m_WindowPages.clear();
    const PageStreaming::PageStoreLevel& level0 = terrain.Table.Levels().front();
    for (const Mathematics::Vector3& camera : cameras)
        AppendWindowPages(terrain.Shape, level0, camera, m_WindowPages);
    for (const Mathematics::Vector3& center : m_ReaderWindowCenters)
        AppendWindowPages(terrain.Shape, level0, center, m_WindowPages);
    AppendBodyWindows(terrain, cameras);
    // After the point windows: a synchronous fetch serves the pages near a camera or a body first.
    AppendWholeLevel0(level0, m_Profile.CpuBytes / kBodyWindowBudgetDivisor, m_WindowPages);
    m_Residency.RequestCpu(terrain.Stream, terrain.Requester.FirstPinnedLevel(), m_WindowPages);
}

void TerrainPages::AppendBodyWindows(const PagedTerrain& terrain, std::span<const Mathematics::Vector3> cameras)
{
    OrderBodyWindows(cameras, Mathematics::Vector3());
    const PageStreaming::PageStoreLevel& level0 = terrain.Table.Levels().front();
    for (const auto& [squaredDistance, body] : m_BodyOrder)
        AppendWindowPages(terrain.Shape, level0, m_BodyWindowCenters[body], m_WindowPages);
}

void TerrainPages::OrderBodyWindows(std::span<const Mathematics::Vector3> cameras, const Mathematics::Vector3& origin)
{
    // The bodies nearest a camera first, as many as a quarter of the CPU budget holds windows for. The
    // cameras are relative to `origin`, the bodies in world space.
    const uint64 bodyBytes = kPagesPerWindow * static_cast<uint64>(PageStreaming::kPageSampleCount) * sizeof(float32);
    const std::size_t bound = static_cast<std::size_t>(m_Profile.CpuBytes / kBodyWindowBudgetDivisor / bodyBytes);
    m_BodyOrder.clear();
    for (uint32 body = 0; body < m_BodyWindowCenters.size(); ++body)
    {
        float32 nearest = 0.0f;
        for (std::size_t camera = 0; camera < cameras.size(); ++camera)
        {
            const Mathematics::Vector3 offset = m_BodyWindowCenters[body] - origin - cameras[camera];
            const float32 squared = Mathematics::Vector3::Dot(offset, offset);
            nearest = camera == 0 ? squared : std::min(nearest, squared);
        }
        m_BodyOrder.emplace_back(nearest, body);
    }
    const bool overBound = m_BodyOrder.size() > bound;
    if (overBound)
    {
        std::nth_element(m_BodyOrder.begin(), m_BodyOrder.begin() + static_cast<std::ptrdiff_t>(bound), m_BodyOrder.end());
        m_BodyOrder.resize(bound);
        if (!m_BodiesOverBound)
            Logger::Log::Warning("Terrain CPU height pages: {} dynamic bodies; the {} nearest a camera keep their finest "
                                 "pages, the others read the pinned levels until fewer bodies move.",
                                 m_BodyWindowCenters.size(), bound);
    }
    m_BodiesOverBound = overBound;
    std::sort(m_BodyOrder.begin(), m_BodyOrder.end());
}

void TerrainPages::GatherBodyWindows(ECS::World& world)
{
    m_BodyWindowCenters.clear();
    world.Query<ECS::Read<Components::PhysicsBody>, ECS::Read<Components::WorldTransform>>().Each(
        [this](const Components::PhysicsBody& body, const Components::WorldTransform& transform) {
            if (body.motionType == Physics::MotionType::Dynamic)
                m_BodyWindowCenters.push_back(transform.GetPosition() - Mathematics::WorldPosition{});
        });
}

void TerrainPages::RequestReaderWindow(const Mathematics::Vector3& center)
{
    m_ReaderWindowCenters.push_back(center);
}

bool TerrainPages::FetchFinestPage(const TerrainHandle& terrain, const Mathematics::Vector3& worldPoint)
{
    const PagedTerrain* paged = m_Terrains.Find(terrain.Index);
    if (!paged || paged->Generation != terrain.Generation)
        return false;
    uint32 pageX = 0;
    uint32 pageZ = 0;
    if (!SurfaceQuery(terrain).FinestPageAt(worldPoint.x, worldPoint.z, pageX, pageZ))
        return false;
    return m_Residency.FetchCpuPage(paged->Stream, PageStreaming::PageAddress{0, 0, pageX, pageZ});
}

SurfaceHeightQuery TerrainPages::SurfaceQuery(const TerrainHandle& terrain) const
{
    const PagedTerrain* paged = m_Terrains.Find(terrain.Index);
    if (!paged || paged->Generation != terrain.Generation)
        return {};
    const SurfaceHeightEdits edits{paged->SourceIdentity, paged->OverlayVersion, paged->OverlayChanged,
                                   paged->OverlayChangedSince};
    return SurfaceHeightQuery(m_Residency.CpuPages(paged->Stream), paged->Table.Levels(), paged->Shape, paged->OriginY,
                              paged->HeightScale, paged->Overlay.get(), edits);
}

std::span<const PageStreaming::PageAddress> TerrainPages::CpuWindowPages(const TerrainHandle& terrain) const
{
    const PagedTerrain* paged = m_Terrains.Find(terrain.Index);
    if (!paged || paged->Generation != terrain.Generation)
        return {};
    return m_Residency.CpuWindow(paged->Stream);
}

void LogPageResidencyChange(std::string_view label, std::string_view stream, const PageTable& table,
                            std::span<const PageStreaming::PageWant> requests, std::string& logged)
{
    if (!LogResidency())
        return;
    std::vector<PageStreaming::PageAddress> wanted;
    wanted.reserve(requests.size());
    for (const PageStreaming::PageWant& want : requests)
        wanted.push_back(want.Address);
    std::vector<PageStreaming::PageAddress> resident;
    table.AppendResident(resident);
    std::sort(resident.begin(), resident.end(), LogOrder);
    std::sort(wanted.begin(), wanted.end(), LogOrder);
    std::vector<PageStreaming::PageAddress> missing; // wanted, not resident
    std::vector<PageStreaming::PageAddress> extra;   // resident, not wanted
    std::set_difference(wanted.begin(), wanted.end(), resident.begin(), resident.end(),
                        std::back_inserter(missing), LogOrder);
    std::set_difference(resident.begin(), resident.end(), wanted.begin(), wanted.end(),
                        std::back_inserter(extra), LogOrder);
    std::string counts;
    for (std::size_t level = 0; level < table.Levels().size(); ++level)
    {
        const auto onLevel = [level](const PageStreaming::PageAddress& page) { return page.Level == level; };
        const std::size_t wantedCount = std::ranges::count_if(wanted, onLevel);
        const std::size_t residentCount = std::ranges::count_if(resident, onLevel);
        const std::size_t notResident = std::ranges::count_if(missing, onLevel);
        counts += std::format(" L{} {}/{}/{}", level, wantedCount, residentCount, wantedCount - notResident);
    }
    std::string line = std::format("wanted/resident/both per level:{}; wanted not resident{}; resident not wanted{}",
                                   counts, AddressList(missing, kLoggedAddressesPerList),
                                   AddressList(extra, kLoggedAddressesPerList));
    if (line == logged)
        return;
    Logger::Log::Info("Terrain pages: {}{} {}", label, stream, line);
    // Once the residency matches the request, the whole resident set, so a settled pose's level map can be
    // rebuilt from the log.
    if (missing.empty() && extra.empty())
        Logger::Log::Info("Terrain pages: {}{} resident:{}", label, stream, AddressList(resident, resident.size()));
    logged = std::move(line);
}

void TerrainPages::Update(uint64 frameIndex, float32 deltaSeconds, bool teleport, uint32 framesInFlight)
{
    m_Uploads.clear();
    if (framesInFlight != m_FramesInFlight)
    {
        // The first frame, or the renderer's frames in flight changed: (re)start the field's cache.
        m_FramesInFlight = framesInFlight;
        m_Residency.ConfigureField(kHeightPageField, m_SlotCount, framesInFlight, kAtlasUpgradeFadeSeconds);
        for (auto& [key, terrain] : m_Terrains)
            terrain.Identity = ~terrain.Identity; // reconfigured by its next Request
        for (auto& [key, planet] : m_Planets)
            ResetPlanetTable(planet); // its stream keeps its pages; their slots are gone
        m_Surface.ConfigureField(framesInFlight);
    }

    m_Forgotten.clear();
    for (auto& [key, terrain] : m_Terrains)
    {
        if (!terrain.Requested)
            m_Forgotten.push_back(key);
        terrain.Requested = false;
    }
    for (uint32 key : m_Forgotten)
    {
        PagedTerrain& forgotten = *m_Terrains.Find(key);
        if (forgotten.GpuPages)
            AddWholeTerrainDirty(key, forgotten); // back to its texture
        Logger::Log::Info("Terrain pages: {} (terrain {} generation {}) stops streaming its height pages",
                          forgotten.Label, key, forgotten.Generation);
        m_Residency.Forget(forgotten.Stream);
        m_Terrains.Erase(key);
    }

    m_Forgotten.clear();
    for (auto& [key, planet] : m_Planets)
    {
        if (!planet.Requested)
            m_Forgotten.push_back(key);
        planet.Requested = false;
    }
    for (uint32 key : m_Forgotten)
    {
        PagedPlanet& forgotten = *m_Planets.Find(key);
        Logger::Log::Info("Terrain pages: {} stops streaming its height pages", forgotten.Label);
        m_Residency.Forget(forgotten.Stream);
        m_Planets.Erase(key);
    }

    m_Surface.ForgetUnrequested();

    {
        GE_CPU_PROFILE_SCOPE("Terrain.HeightPages.Residency");
        m_Residency.Update(frameIndex, deltaSeconds, teleport);
    }
    m_ReaderWindowCenters.clear(); // a reader still waiting asks again
    for (auto& [key, terrain] : m_Terrains)
    {
        // Also while the GPU draws the terrain's texture: its pages keep their slots, and an edit must
        // reach those slots and the table before the GPU draws the pages again.
        CollectStreamChanges(key, terrain);
        if (terrain.GpuPages)
            LogPageResidencyChange(terrain.Label, {}, terrain.Table, terrain.WantedPages, terrain.LoggedResidency);
    }
    for (auto& [key, planet] : m_Planets)
        if (planet.GpuPages)
            CollectPlanetChanges(planet);
    m_Surface.Collect();
}

void TerrainPages::ResetPlanetTable(PagedPlanet& planet)
{
    const std::span<const PageStreaming::PageStoreLevel> levels = planet.Requester.Levels();
    if (levels.empty())
        return;
    planet.Table.Reset(levels.front().SamplesX, levels.front().SamplesZ, PageStreaming::kPageStoreMaxFaces,
                       m_SlotCount, TableBudget(m_Profile));
    planet.Packed.Update(planet.Table, m_Geometry, ++m_TableVersion);
    // An empty table resolves no sample: the planet draws its relief until its pinned levels return.
    if (planet.Published)
        AddPlanetWholeDirty();
    planet.Published = false;
}

void TerrainPages::AddPlanetWholeDirty()
{
    for (uint32 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
        AddPlanetDirtyRect(face, 0.0f, 0.0f, 1.0f, 1.0f);
}

void TerrainPages::AddPlanetDirtyRect(uint32 face, float32 minU, float32 minV, float32 maxU, float32 maxV)
{
    for (uint32 i = 0; i < m_PlanetDirty.Count; ++i)
    {
        CBTTerrain::SphereFaceUVRect& rect = m_PlanetDirty.Rects[i];
        if (rect.Face != face)
            continue;
        rect.MinU = std::min(rect.MinU, minU);
        rect.MinV = std::min(rect.MinV, minV);
        rect.MaxU = std::max(rect.MaxU, maxU);
        rect.MaxV = std::max(rect.MaxV, maxV);
        return;
    }
    if (m_PlanetDirty.Count < m_PlanetDirty.Rects.size())
        m_PlanetDirty.Rects[m_PlanetDirty.Count++] = CBTTerrain::SphereFaceUVRect{face, minU, minV, maxU, maxV};
}

void TerrainPages::AddPlanetDirtyPage(const PagedPlanet& planet, const PageStreaming::PageAddress& address)
{
    // The page's footprint in equal-angle face UV, one sample of its level wider on each side.
    const uint32 samples = planet.Table.Levels().front().SamplesX;
    float32 minU = 0.0f, maxU = 0.0f, minV = 0.0f, maxV = 0.0f;
    PageAxisUV(address.X, address.Level, samples, minU, maxU);
    PageAxisUV(address.Z, address.Level, samples, minV, maxV);
    AddPlanetDirtyFaceRect(address.Face, address.Level, samples, minU, minV, maxU, maxV);
}

void TerrainPages::AddPlanetDirtyRewrite(const PagedPlanet& planet, const PageStreaming::PageRewrite& rewrite)
{
    // Only the samples of the page's level that the edit reaches changed (AddDirtyRewrite's rule).
    const PageStreaming::PageAddress& address = rewrite.Page.Address;
    const std::span<const PageStreaming::PageStoreLevel> levels = planet.Table.Levels();
    const uint32 samples = levels.front().SamplesX;
    const PageStreaming::PageSampleRect reach =
        PageStreaming::Level0RectReach(levels, rewrite.Level0Rect, address.Level);
    float32 pageMinU = 0.0f, pageMaxU = 0.0f, pageMinV = 0.0f, pageMaxV = 0.0f;
    PageAxisUV(address.X, address.Level, samples, pageMinU, pageMaxU);
    PageAxisUV(address.Z, address.Level, samples, pageMinV, pageMaxV);
    float32 minU = 0.0f, maxU = 0.0f, minV = 0.0f, maxV = 0.0f;
    ReachAxisUV(reach.MinX, reach.MaxX, address.Level, samples, minU, maxU);
    ReachAxisUV(reach.MinZ, reach.MaxZ, address.Level, samples, minV, maxV);
    minU = std::max(minU, pageMinU);
    minV = std::max(minV, pageMinV);
    maxU = std::min(maxU, pageMaxU);
    maxV = std::min(maxV, pageMaxV);
    if (minU > maxU || minV > maxV)
        return; // nothing under the page's footprint changed
    AddPlanetDirtyFaceRect(address.Face, address.Level, samples, minU, minV, maxU, maxV);
}

void TerrainPages::AddPlanetDirtyFaceRect(uint8 face, uint32 level, uint32 samples, float32 minU, float32 minV,
                                          float32 maxU, float32 maxV)
{
    AddPlanetDirtyRect(face, CubeFaceUV(minU), CubeFaceUV(minV), CubeFaceUV(maxU), CubeFaceUV(maxV));

    // A corner on a face side resolves on the lowest face that holds it (CBT_PlanetPagePoint), so a
    // rect on a side changes the corners of the neighbor face along that side: a band one sample of
    // the level deep on the neighbor's side, over the rect's reach along the edge.
    const float32 band = static_cast<float32>(1u << level) / static_cast<float32>(samples - 1u);
    const FaceSideReach sides[] = {
        {minV <= 0.0f, PageStreaming::PageFaceSide::MinZ, minU, maxU},
        {maxV >= 1.0f, PageStreaming::PageFaceSide::MaxZ, minU, maxU},
        {minU <= 0.0f, PageStreaming::PageFaceSide::MinX, minV, maxV},
        {maxU >= 1.0f, PageStreaming::PageFaceSide::MaxX, minV, maxV},
    };
    for (const FaceSideReach& side : sides)
    {
        if (!side.Touches)
            continue;
        const PageStreaming::PageFaceEdge edge = PageStreaming::PageFaceNeighbor(face, side.Side);
        const float32 alongMin = edge.Reversed ? 1.0f - side.AlongMax : side.AlongMin;
        const float32 alongMax = edge.Reversed ? 1.0f - side.AlongMin : side.AlongMax;
        float32 acrossMin = 0.0f;
        float32 acrossMax = band;
        if (edge.Side == PageStreaming::PageFaceSide::MaxZ || edge.Side == PageStreaming::PageFaceSide::MaxX)
        {
            acrossMin = 1.0f - band;
            acrossMax = 1.0f;
        }
        const bool column =
            edge.Side == PageStreaming::PageFaceSide::MinX || edge.Side == PageStreaming::PageFaceSide::MaxX;
        const float32 u0 = column ? acrossMin : alongMin;
        const float32 u1 = column ? acrossMax : alongMax;
        const float32 v0 = column ? alongMin : acrossMin;
        const float32 v1 = column ? alongMax : acrossMax;
        AddPlanetDirtyRect(edge.Face, CubeFaceUV(u0), CubeFaceUV(v0), CubeFaceUV(u1), CubeFaceUV(v1));
    }
}

void TerrainPages::CollectPlanetChanges(PagedPlanet& planet)
{
    const PageStreaming::PageCache& cache = m_Residency.FieldCache(kHeightPageField);
    m_LateEntries.clear();
    const uint32 writes = UpdatePageTable(cache, planet.Stream, planet.Table);
    planet.Table.TakeLateEntries(m_LateEntries);
    if (writes != 0u)
        planet.Packed.Update(planet.Table, m_Geometry, ++m_TableVersion);

    // An assigned or released page changes the height source over its footprint, a fading page
    // blends its footprint with its parent, and a waiting page that takes its entry starts to answer.
    bool changed = writes != 0u || !m_LateEntries.empty();
    for (const PageStreaming::CachedPage& page : cache.Transitions())
        if (page.Stream == planet.Stream)
            AddPlanetDirtyPage(planet, page.Address);
    for (const PageStreaming::CachedPage& page : cache.FadeChanges())
        if (page.Stream == planet.Stream)
            AddPlanetDirtyPage(planet, page.Address);
    for (const PageStreaming::PageAddress& address : m_LateEntries)
        AddPlanetDirtyPage(planet, address);
    for (const PageStreaming::PageRewrite& rewrite : cache.Rewrites())
        if (rewrite.Page.Stream == planet.Stream)
            AddPlanetDirtyRewrite(planet, rewrite);
    if (!planet.Published && PinnedPagesHaveEntries(planet.Table, planet.Requester.FirstPinnedLevel()))
    {
        // Every face's height changes source from the relief to the pages: the CBT re-evaluates the planet.
        planet.Published = true;
        AddPlanetWholeDirty();
        changed = true;
    }

    for (const PageStreaming::PageUploadRequest& upload : cache.Uploads())
    {
        if (upload.Page.Stream != planet.Stream)
            continue;
        const std::vector<uint8>* bytes = m_Residency.Bytes(planet.Stream, upload.Page.Address);
        if (bytes == nullptr)
            continue;
        HeightPageUpload& out = m_Uploads.emplace_back(HeightPageUpload{upload.Slot, *bytes});
        ApplyPlanetLayers(planet, upload.Page.Address, PageStreaming::HeightSamples(out.Bytes));
        changed = true;
    }
    // A planet's pages lie on six faces, not on its terrain UV: a change covers the whole planet.
    if (changed && planet.RenderTerrain != kNoRenderTerrain)
        m_ChangeRects.push_back(PageChangeRect{planet.RenderTerrain});
}

void TerrainPages::AddDirtyPage(uint32 terrainKey, const PagedTerrain& terrain,
                                const PageStreaming::PageAddress& address, HeightPageDirtyRect& rect,
                                bool& any) const
{
    const PageStreaming::PageStoreLevel& level0 = terrain.Table.Levels().front();
    float32 minU = 0.0f, maxU = 0.0f, minV = 0.0f, maxV = 0.0f;
    PageAxisUV(address.X, address.Level, level0.SamplesX, minU, maxU);
    PageAxisUV(address.Z, address.Level, level0.SamplesZ, minV, maxV);
    UniteDirty(terrainKey, terrain.Generation, minU, minV, maxU, maxV, rect, any);
}

void TerrainPages::AddDirtyRewrite(uint32 terrainKey, const PagedTerrain& terrain,
                                   const PageStreaming::PageRewrite& rewrite, HeightPageDirtyRect& rect,
                                   bool& any) const
{
    // Only the samples of the page's level that the edit reaches changed: a coarse page covers a
    // large part of the terrain, the edit's reach at its level a few of its samples.
    const PageStreaming::PageAddress& address = rewrite.Page.Address;
    const PageStreaming::PageStoreLevel& level0 = terrain.Table.Levels().front();
    const PageStreaming::PageSampleRect reach =
        PageStreaming::Level0RectReach(terrain.Table.Levels(), rewrite.Level0Rect, address.Level);
    float32 pageMinU = 0.0f, pageMaxU = 0.0f, pageMinV = 0.0f, pageMaxV = 0.0f;
    PageAxisUV(address.X, address.Level, level0.SamplesX, pageMinU, pageMaxU);
    PageAxisUV(address.Z, address.Level, level0.SamplesZ, pageMinV, pageMaxV);
    float32 minU = 0.0f, maxU = 0.0f, minV = 0.0f, maxV = 0.0f;
    ReachAxisUV(reach.MinX, reach.MaxX, address.Level, level0.SamplesX, minU, maxU);
    ReachAxisUV(reach.MinZ, reach.MaxZ, address.Level, level0.SamplesZ, minV, maxV);
    minU = std::max(minU, pageMinU);
    minV = std::max(minV, pageMinV);
    maxU = std::min(maxU, pageMaxU);
    maxV = std::min(maxV, pageMaxV);
    if (minU > maxU || minV > maxV)
        return; // nothing under the page's footprint changed
    UniteDirty(terrainKey, terrain.Generation, minU, minV, maxU, maxV, rect, any);
}

void TerrainPages::CollectStreamChanges(uint32 terrainKey, PagedTerrain& terrain)
{
    const PageStreaming::PageCache& cache = m_Residency.FieldCache(kHeightPageField);
    m_LateEntries.clear();
    const uint32 writes = UpdatePageTable(cache, terrain.Stream, terrain.Table);
    terrain.Table.TakeLateEntries(m_LateEntries);
    if (writes != 0u)
        terrain.Packed.Update(terrain.Table, m_Geometry, ++m_TableVersion);

    // The dirty and change rects reach the renderer only while the GPU draws the pages: the switch
    // between the pages and the texture dirties the whole terrain.
    if (terrain.GpuPages)
    {
        // An assigned or released page changes the height source over its whole footprint, and a fading
        // page blends its whole footprint with its parent; a rewritten page changed where its edit reaches.
        HeightPageDirtyRect rect;
        bool any = false;
        for (const PageStreaming::CachedPage& page : cache.Transitions())
            if (page.Stream == terrain.Stream)
                AddDirtyPage(terrainKey, terrain, page.Address, rect, any);
        for (const PageStreaming::CachedPage& page : cache.FadeChanges())
            if (page.Stream == terrain.Stream)
                AddDirtyPage(terrainKey, terrain, page.Address, rect, any);
        for (const PageStreaming::PageAddress& address : m_LateEntries)
            AddDirtyPage(terrainKey, terrain, address, rect, any); // a waiting page took its entry
        for (const PageStreaming::PageRewrite& rewrite : cache.Rewrites())
            if (rewrite.Page.Stream == terrain.Stream)
                AddDirtyRewrite(terrainKey, terrain, rewrite, rect, any);
        if (any)
            m_DirtyRects.push_back(rect);
        AppendPageChangeRects(terrainKey, terrain.Stream, terrain.Table.Levels(), cache, m_LateEntries, m_ChangeRects);
    }

    for (const PageStreaming::PageUploadRequest& upload : cache.Uploads())
    {
        if (upload.Page.Stream != terrain.Stream)
            continue;
        const std::vector<uint8>* bytes = m_Residency.Bytes(terrain.Stream, upload.Page.Address);
        if (bytes == nullptr)
            continue;
        HeightPageUpload& out = m_Uploads.emplace_back(HeightPageUpload{upload.Slot, *bytes});
        if (terrain.Overlay)
            terrain.Overlay->ApplyToPage(upload.Page.Address, PageStreaming::HeightSamples(out.Bytes));
    }
}

void TerrainPages::RestartOnDeviceEpoch(uint64 deviceEpoch)
{
    if (deviceEpoch == m_DeviceEpoch)
        return;
    m_DeviceEpoch = deviceEpoch;
    if (m_FramesInFlight == 0)
        return;
    m_Residency.ConfigureField(kHeightPageField, m_SlotCount, m_FramesInFlight, kAtlasUpgradeFadeSeconds);
    for (auto& [key, terrain] : m_Terrains)
    {
        // Its streams' loaded pages stay; their slots are gone: no entry may name one.
        const PageStreaming::PageStoreLevel& level0 = terrain.Table.Levels().front();
        terrain.Table.Reset(level0.SamplesX, level0.SamplesZ, 1u, m_SlotCount, TableBudget(m_Profile));
        terrain.Packed.Update(terrain.Table, m_Geometry, ++m_TableVersion);
        if (terrain.GpuPages)
            AddWholeTerrainDirty(key, terrain);
    }
    for (auto& [key, planet] : m_Planets)
        ResetPlanetTable(planet);
    m_Surface.ConfigureField(m_FramesInFlight);
}

void TerrainPages::PagedTerrains(std::vector<uint32>& out) const
{
    out.clear();
    for (const auto& [key, terrain] : m_Terrains)
        if (terrain.GpuPages)
            out.push_back(key);
    for (const auto& [key, planet] : m_Planets)
        if (planet.Published && planet.RenderTerrain != kNoRenderTerrain)
            out.push_back(planet.RenderTerrain);
}

const PackedPageTable* TerrainPages::PackedTable(uint32 terrain) const
{
    if (const PagedTerrain* paged = m_Terrains.Find(terrain))
        return paged->GpuPages ? &paged->Packed : nullptr;
    for (const auto& [key, planet] : m_Planets)
        if (planet.Published && planet.RenderTerrain == terrain)
            return &planet.Packed;
    return nullptr;
}

uint64 TerrainPages::WholeTableHash(uint32 terrain) const
{
    const PageTable* table = nullptr;
    if (const PagedTerrain* paged = m_Terrains.Find(terrain))
        table = paged->GpuPages ? &paged->Table : nullptr;
    else
        for (const auto& [key, planet] : m_Planets)
            if (table == nullptr && planet.Published && planet.RenderTerrain == terrain)
                table = &planet.Table;
    if (table == nullptr)
        return 0;
    std::vector<uint32> words;
    PackPageTableWords(*table, m_Geometry, words);
    return PackedPageTable::HashWords(words);
}

} // namespace GameEngine::TerrainECS
