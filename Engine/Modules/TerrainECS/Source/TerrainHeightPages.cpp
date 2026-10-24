#include "TerrainECS/TerrainHeightPages.h"

#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainService.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "CBTTerrain/CBTLayout.h"
#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageTableEntry.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

namespace GameEngine::TerrainECS
{

namespace
{

// The last level-0 sample a page of `level` at index `page` reads along an axis of `samples`,
// over (samples - 1): the page's footprint in terrain UV, one level-`level` sample of margin on
// each side for the bilinear tap that straddles its edge.
void PageAxisUV(uint32 page, uint32 level, uint32 samples, float32& outMin, float32& outMax)
{
    const float32 span = static_cast<float32>(PageStreaming::kPageOwnedSamples << level);
    const float32 margin = static_cast<float32>(1u << level);
    const float32 inv = samples > 1u ? 1.0f / static_cast<float32>(samples - 1u) : 0.0f;
    outMin = std::max(0.0f, (static_cast<float32>(page) * span - margin) * inv);
    outMax = std::min(1.0f, (static_cast<float32>(page + 1u) * span + margin) * inv);
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
// Deck 32 MiB: 31); the rest of the budget stays with the camera windows and the pinned levels.
constexpr uint64 kBodyWindowBudgetDivisor = 4;

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

// True when the requester of a terrain configured with `a` asks for the same pages as one
// configured with `b`: the same pyramid placed at the same world position and height range.
bool SamePlacement(const PageRequestTerrain& a, const PageRequestTerrain& b)
{
    return a.OriginX == b.OriginX && a.OriginZ == b.OriginZ && a.Level0TexelX == b.Level0TexelX &&
           a.Level0TexelZ == b.Level0TexelZ && a.SamplesX == b.SamplesX && a.SamplesZ == b.SamplesZ &&
           a.MinHeight == b.MinHeight && a.MaxHeight == b.MaxHeight;
}

// GE_CBT_VALIDATE: log each paged terrain's wanted and resident pages per level whenever they change.
bool LogResidency()
{
    static const bool kLogResidency = std::getenv("GE_CBT_VALIDATE") != nullptr;
    return kLogResidency;
}

// The pages of the residency line's address lists, at most; the rest are counted.
constexpr std::size_t kLoggedAddressesPerList = 32;

// The level, page X and page Z of table entry index `entry`.
void EntryAddress(const PageTable& table, uint32 entry, uint32& level, uint32& x, uint32& z)
{
    level = 0;
    while (level + 1u < table.Levels.size() && entry >= table.Levels[level + 1u].FirstEntry)
        ++level;
    const uint32 local = entry - table.Levels[level].FirstEntry;
    x = local % table.Levels[level].PagesX;
    z = local / table.Levels[level].PagesX;
}

// " L<level>(x,z)" for each of `entries`, the first `limit`, then the count left.
std::string AddressList(const PageTable& table, const std::vector<uint32>& entries, std::size_t limit)
{
    std::string out;
    for (std::size_t i = 0; i < entries.size() && i < limit; ++i)
    {
        uint32 level = 0, x = 0, z = 0;
        EntryAddress(table, entries[i], level, x, z);
        out += std::format(" L{}({},{})", level, x, z);
    }
    if (entries.size() > limit)
        out += std::format(" +{} more", entries.size() - limit);
    return out.empty() ? std::string(" none") : out;
}

} // namespace

uint64 PageTableWordCount(uint32 samplesX, uint32 samplesZ, uint32 slotCount)
{
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(samplesX, samplesZ);
    if (levels.empty() || levels.size() > CBTTerrain::kCBTPageMaxLevels)
        return ~0ull;
    const uint64 entries = static_cast<uint64>(levels.back().FirstEntry) + levels.back().PagesX * levels.back().PagesZ;
    return CBTTerrain::kCBTPageFadeOffsetWords + static_cast<uint64>(slotCount) + entries;
}

HeightPageProfile PlatformHeightPageProfile()
{
#if defined(PLATFORM_STEAMDECK)
    return kSteamDeckHeightPages;
#else
    return kDesktopHeightPages;
#endif
}

std::string PageTableRefusal(std::string_view name, uint32 samplesX, uint32 samplesZ, const HeightPageProfile& profile)
{
    const uint64 words = PageTableWordCount(samplesX, samplesZ, profile.Slots);
    if (words <= profile.TableWordCap)
        return {};
    constexpr float64 kWordsPerMiB = 1024.0 * 1024.0 / sizeof(uint32);
    return std::format("{} ({} x {} samples) does not stream its height on this platform: its page table needs {} "
                       "words ({:.1f} MiB), over the platform's cap of {} words ({:.1f} MiB). It draws from its "
                       "height texture. Lower its samples per meter or its size to stream it.",
                       name, samplesX, samplesZ, words, static_cast<float64>(words) / kWordsPerMiB,
                       profile.TableWordCap, static_cast<float64>(profile.TableWordCap) / kWordsPerMiB);
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
    out.assign(CBTTerrain::kCBTPageFadeOffsetWords + table.SlotFade.size() + table.Entries.size(), 0u);
    if (table.Levels.empty())
        return;
    out[0] = table.Levels.front().SamplesX;
    out[1] = table.Levels.front().SamplesZ;
    out[2] = static_cast<uint32>(table.Levels.size());
    out[3] = geometry.SlotsPerRow;
    out[4] = static_cast<uint32>(table.SlotFade.size());
    for (std::size_t level = 0; level < table.Levels.size(); ++level)
    {
        uint32* shape = out.data() + CBTTerrain::kCBTPageHeaderWords + 4u * level;
        shape[0] = table.Levels[level].FirstEntry;
        shape[1] = table.Levels[level].PagesX;
        shape[2] = table.Levels[level].PagesZ;
    }
    uint32* fades = out.data() + CBTTerrain::kCBTPageFadeOffsetWords;
    for (std::size_t slot = 0; slot < table.SlotFade.size(); ++slot)
        fades[slot] = std::bit_cast<uint32>(table.SlotFade[slot]);
    std::memcpy(fades + table.SlotFade.size(), table.Entries.data(), table.Entries.size() * sizeof(uint32));
}

TerrainHeightPages::TerrainHeightPages(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io,
                                       const HeightPageProfile& profile)
    : m_Residency(pool, io, ResidencyBudget(profile))
    , m_Profile(profile)
    , m_SlotCount(profile.Slots)
    , m_Geometry(MakePageCacheGeometry(profile.Slots))
{
}

void TerrainHeightPages::ReportRefusal(uint32 terrain, const std::string& refusal)
{
    std::string& logged = m_Refusals.GetOrInsert(terrain);
    if (logged == refusal)
        return;
    logged = refusal;
    Logger::Log::Warning("Terrain pages: {}", refusal);
}

void TerrainHeightPages::Configure(PagedTerrain& terrain, const PagedTerrainRequest& request)
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
    terrain.Requester.Configure(terrain.Shape);
    const PageRequestTerrain& shape = terrain.Shape;
    terrain.Table.Reset(shape.SamplesX, shape.SamplesZ, m_SlotCount);
    PackPageTableWords(terrain.Table, m_Geometry, terrain.Words);
    terrain.Version = ++m_TableVersion;
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
                      terrain.Table.Levels.size(), request.GpuPages ? "" : " for the CPU only");
    // The terrain's height changes source (from its texture, or from its previous pages).
    if (terrain.GpuPages)
        AddWholeTerrainDirty(request.Terrain, terrain);
}

void TerrainHeightPages::AddWholeTerrainDirty(uint32 terrainKey, const PagedTerrain& terrain)
{
    m_DirtyRects.push_back(HeightPageDirtyRect{terrainKey, terrain.Generation, 0.0f, 0.0f, 1.0f, 1.0f});
}

bool TerrainHeightPages::NeedsSource(uint32 terrain, uint64 sourceIdentity) const
{
    const PagedTerrain* paged = m_Terrains.Find(terrain);
    return !paged || paged->Identity != sourceIdentity;
}

void TerrainHeightPages::Request(const PagedTerrainRequest& request, std::span<const Mathematics::Vector3> cameras,
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
        // The GPU starts or stops drawing the pages (its texture meanwhile); the CPU pages stay.
        terrain.GpuPages = request.GpuPages;
        terrain.Table.Reset(terrain.Shape.SamplesX, terrain.Shape.SamplesZ, m_SlotCount);
        PackPageTableWords(terrain.Table, m_Geometry, terrain.Words);
        terrain.Version = ++m_TableVersion;
        AddWholeTerrainDirty(request.Terrain, terrain);
    }
    if (!SamePlacement(request.Shape, terrain.Shape))
    {
        // Moved or rescaled: the resident pages are keyed by terrain UV and stay valid, but the
        // requester asks for the pages under the cameras at the new placement, and every bisector's
        // world corners move with it.
        terrain.Shape = request.Shape;
        terrain.Shape.CacheSlots = m_SlotCount;
        terrain.Requester.Configure(terrain.Shape);
        AddWholeTerrainDirty(request.Terrain, terrain);
    }
    if (request.OverlayVersion != terrain.OverlayVersion)
    {
        // A modifier edit: the pages over the changed rect are rewritten with the new overlay as
        // they reload, keeping their old content until then. An overlay not derived from the one
        // the pages hold (the terrain's modifier data was rebuilt, as a scene load does) rewrites
        // every page either overlay reaches.
        if (request.OverlayChangedSince == terrain.OverlayVersion)
        {
            m_Residency.Refresh(terrain.Stream, request.OverlayChanged);
        }
        else
        {
            if (terrain.Overlay)
                m_Residency.Refresh(terrain.Stream, terrain.Overlay->Level0Rect());
            if (request.Overlay)
                m_Residency.Refresh(terrain.Stream, request.Overlay->Level0Rect());
        }
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
    m_Wants.clear();
    if (terrain.GpuPages)
        terrain.Requester.Build(cameras, view, m_Wants);
    m_Residency.Request(terrain.Stream, m_Wants);
    RequestCpuWindows(terrain, cameras);
    if (LogResidency())
    {
        terrain.WantedEntries.clear();
        for (const PageStreaming::PageWant& want : m_Wants)
        {
            const PageStreaming::PageStoreLevel& level = terrain.Table.Levels[want.Address.Level];
            terrain.WantedEntries.push_back(level.FirstEntry + want.Address.Z * level.PagesX + want.Address.X);
        }
        std::sort(terrain.WantedEntries.begin(), terrain.WantedEntries.end());
    }
}

void TerrainHeightPages::RequestCpuWindows(const PagedTerrain& terrain, std::span<const Mathematics::Vector3> cameras)
{
    m_WindowPages.clear();
    const PageStreaming::PageStoreLevel& level0 = terrain.Table.Levels.front();
    for (const Mathematics::Vector3& camera : cameras)
        AppendWindowPages(terrain.Shape, level0, camera, m_WindowPages);
    for (const Mathematics::Vector3& center : m_ReaderWindowCenters)
        AppendWindowPages(terrain.Shape, level0, center, m_WindowPages);
    AppendBodyWindows(terrain, cameras);
    m_Residency.RequestCpu(terrain.Stream, terrain.Requester.FirstPinnedLevel(), m_WindowPages);
}

void TerrainHeightPages::AppendBodyWindows(const PagedTerrain& terrain, std::span<const Mathematics::Vector3> cameras)
{
    // The bodies nearest a camera first, as many as a quarter of the CPU budget holds windows for.
    const uint64 bodyBytes = kPagesPerWindow * static_cast<uint64>(PageStreaming::kPageSampleCount) * sizeof(float32);
    const std::size_t bound = static_cast<std::size_t>(m_Profile.CpuBytes / kBodyWindowBudgetDivisor / bodyBytes);
    m_BodyOrder.clear();
    for (uint32 body = 0; body < m_BodyWindowCenters.size(); ++body)
    {
        float32 nearest = 0.0f;
        for (std::size_t camera = 0; camera < cameras.size(); ++camera)
        {
            const Mathematics::Vector3 offset = m_BodyWindowCenters[body] - cameras[camera];
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
    const PageStreaming::PageStoreLevel& level0 = terrain.Table.Levels.front();
    for (const auto& [squaredDistance, body] : m_BodyOrder)
        AppendWindowPages(terrain.Shape, level0, m_BodyWindowCenters[body], m_WindowPages);
}

void TerrainHeightPages::GatherBodyWindows(ECS::World& world)
{
    m_BodyWindowCenters.clear();
    world.Query<ECS::Read<Components::PhysicsBody>, ECS::Read<Components::WorldTransform>>().Each(
        [this](const Components::PhysicsBody& body, const Components::WorldTransform& transform) {
            if (body.motionType == Physics::MotionType::Dynamic)
                m_BodyWindowCenters.push_back(transform.GetPosition());
        });
}

void TerrainHeightPages::RequestReaderWindow(const Mathematics::Vector3& center)
{
    m_ReaderWindowCenters.push_back(center);
}

bool TerrainHeightPages::FetchFinestPage(const TerrainHandle& terrain, const Mathematics::Vector3& worldPoint)
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

SurfaceHeightQuery TerrainHeightPages::SurfaceQuery(const TerrainHandle& terrain) const
{
    const PagedTerrain* paged = m_Terrains.Find(terrain.Index);
    if (!paged || paged->Generation != terrain.Generation)
        return {};
    const SurfaceHeightEdits edits{paged->SourceIdentity, paged->OverlayVersion, paged->OverlayChanged,
                                   paged->OverlayChangedSince};
    return SurfaceHeightQuery(m_Residency.CpuPages(paged->Stream), paged->Table.Levels, paged->Shape, paged->OriginY,
                              paged->HeightScale, paged->Overlay.get(), edits);
}

std::span<const PageStreaming::PageAddress> TerrainHeightPages::CpuWindowPages(const TerrainHandle& terrain) const
{
    const PagedTerrain* paged = m_Terrains.Find(terrain.Index);
    if (!paged || paged->Generation != terrain.Generation)
        return {};
    return m_Residency.CpuWindow(paged->Stream);
}

void TerrainHeightPages::LogResidencyChange(PagedTerrain& terrain)
{
    const PageTable& table = terrain.Table;
    std::vector<uint32> missing; // wanted, not resident
    std::vector<uint32> extra;   // resident, not wanted
    std::vector<uint32> resident;
    std::string counts;
    for (std::size_t level = 0; level < table.Levels.size(); ++level)
    {
        const uint32 first = table.Levels[level].FirstEntry;
        const uint32 end = first + table.Levels[level].PagesX * table.Levels[level].PagesZ;
        uint32 wanted = 0, residentCount = 0, both = 0;
        for (uint32 entry = first; entry < end; ++entry)
        {
            const bool isWanted = std::binary_search(terrain.WantedEntries.begin(), terrain.WantedEntries.end(), entry);
            const bool isResident = table.Entries[entry] != PageStreaming::kNoPage;
            wanted += isWanted ? 1u : 0u;
            residentCount += isResident ? 1u : 0u;
            both += isWanted && isResident ? 1u : 0u;
            if (isWanted && !isResident)
                missing.push_back(entry);
            if (isResident && !isWanted)
                extra.push_back(entry);
            if (isResident)
                resident.push_back(entry);
        }
        counts += std::format(" L{} {}/{}/{}", level, wanted, residentCount, both);
    }
    std::string line = std::format("wanted/resident/both per level:{}; wanted not resident{}; resident not wanted{}",
                                   counts, AddressList(table, missing, kLoggedAddressesPerList),
                                   AddressList(table, extra, kLoggedAddressesPerList));
    if (line == terrain.LoggedResidency)
        return;
    Logger::Log::Info("Terrain pages: {} {}", terrain.Label, line);
    // Once the residency matches the request, the whole resident set, so a settled pose's level map can be
    // rebuilt from the log.
    if (missing.empty() && extra.empty())
        Logger::Log::Info("Terrain pages: {} resident:{}", terrain.Label, AddressList(table, resident, resident.size()));
    terrain.LoggedResidency = std::move(line);
}

void TerrainHeightPages::Update(uint64 frameIndex, float32 deltaSeconds, bool teleport, uint32 framesInFlight)
{
    m_Uploads.clear();
    if (framesInFlight != m_FramesInFlight)
    {
        // The first frame, or the renderer's frames in flight changed: (re)start the field's cache.
        m_FramesInFlight = framesInFlight;
        m_Residency.ConfigureField(kHeightPageField, m_SlotCount, framesInFlight, kAtlasUpgradeFadeSeconds);
        for (auto& [key, terrain] : m_Terrains)
            terrain.Identity = ~terrain.Identity; // reconfigured by its next Request
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

    m_Residency.Update(frameIndex, deltaSeconds, teleport);
    m_ReaderWindowCenters.clear(); // a reader still waiting asks again
    for (auto& [key, terrain] : m_Terrains)
    {
        if (!terrain.GpuPages)
            continue; // the CPU reads its pages, the GPU draws its texture
        CollectStreamChanges(key, terrain);
        if (LogResidency())
            LogResidencyChange(terrain);
    }
}

void TerrainHeightPages::AddDirtyPage(uint32 terrainKey, const PagedTerrain& terrain,
                                      const PageStreaming::PageAddress& address, HeightPageDirtyRect& rect,
                                      bool& any) const
{
    const PageStreaming::PageStoreLevel& level0 = terrain.Table.Levels.front();
    float32 minU = 0.0f, maxU = 0.0f, minV = 0.0f, maxV = 0.0f;
    PageAxisUV(address.X, address.Level, level0.SamplesX, minU, maxU);
    PageAxisUV(address.Z, address.Level, level0.SamplesZ, minV, maxV);
    if (!any)
    {
        rect = HeightPageDirtyRect{terrainKey, terrain.Generation, minU, minV, maxU, maxV};
        any = true;
        return;
    }
    rect.MinU = std::min(rect.MinU, minU);
    rect.MinV = std::min(rect.MinV, minV);
    rect.MaxU = std::max(rect.MaxU, maxU);
    rect.MaxV = std::max(rect.MaxV, maxV);
}

void TerrainHeightPages::CollectStreamChanges(uint32 terrainKey, PagedTerrain& terrain)
{
    const PageStreaming::PageCache& cache = m_Residency.FieldCache(kHeightPageField);
    if (UpdatePageTable(cache, terrain.Stream, terrain.Table) != 0u)
    {
        PackPageTableWords(terrain.Table, m_Geometry, terrain.Words);
        terrain.Version = ++m_TableVersion;
    }

    HeightPageDirtyRect rect;
    bool any = false;
    for (const PageStreaming::CachedPage& page : cache.Transitions())
        if (page.Stream == terrain.Stream)
            AddDirtyPage(terrainKey, terrain, page.Address, rect, any);
    for (const PageStreaming::CachedPage& page : cache.FadeChanges())
        if (page.Stream == terrain.Stream)
            AddDirtyPage(terrainKey, terrain, page.Address, rect, any);
    for (const PageStreaming::CachedPage& page : cache.Rewrites())
        if (page.Stream == terrain.Stream)
            AddDirtyPage(terrainKey, terrain, page.Address, rect, any);
    if (any)
        m_DirtyRects.push_back(rect);

    for (const PageStreaming::PageUploadRequest& upload : cache.Uploads())
    {
        if (upload.Page.Stream != terrain.Stream)
            continue;
        const std::vector<float32>* samples = m_Residency.Samples(terrain.Stream, upload.Page.Address);
        if (samples == nullptr)
            continue;
        HeightPageUpload& out = m_Uploads.emplace_back(HeightPageUpload{upload.Slot, *samples});
        if (terrain.Overlay)
            terrain.Overlay->ApplyToPage(upload.Page.Address, out.Samples);
    }
}

void TerrainHeightPages::RestartOnDeviceEpoch(uint64 deviceEpoch)
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
        const PageStreaming::PageStoreLevel& level0 = terrain.Table.Levels.front();
        terrain.Table.Reset(level0.SamplesX, level0.SamplesZ, m_SlotCount);
        PackPageTableWords(terrain.Table, m_Geometry, terrain.Words);
        terrain.Version = ++m_TableVersion;
        if (terrain.GpuPages)
            AddWholeTerrainDirty(key, terrain);
    }
}

void TerrainHeightPages::PagedTerrains(std::vector<uint32>& out) const
{
    out.clear();
    for (const auto& [key, terrain] : m_Terrains)
        if (terrain.GpuPages)
            out.push_back(key);
}

const std::vector<uint32>* TerrainHeightPages::TableWords(uint32 terrain, uint64& version) const
{
    const PagedTerrain* paged = m_Terrains.Find(terrain);
    if (!paged || !paged->GpuPages)
        return nullptr;
    version = paged->Version;
    return &paged->Words;
}

} // namespace GameEngine::TerrainECS
