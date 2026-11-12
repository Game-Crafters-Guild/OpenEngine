#pragma once

#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageResidency.h"
#include "TerrainECS/PackedPageTable.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/PageTable.h"
#include "TerrainECS/PagedHeightSampler.h"
#include "TerrainECS/Planet/PlanetChart.h"
#include "TerrainECS/Planet/PlanetPageRequester.h"
#include "TerrainECS/TerrainSurface/TerrainSurfacePages.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "CBTTerrain/CBTSphereFaceMap.h"
#include "Mathematics/Vector3.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{
class AssetIOService;
}

namespace GameEngine::ECS
{
class World;
}

// The pages of every paged terrain of the process, on the main thread: the one residency manager
// (design D-c), each terrain's height stream with its page table, GPU uploads, dirty rects and CPU
// windows (design D3), and each terrain's surface stream (TerrainSurfacePages).
namespace GameEngine::TerrainECS
{

struct TerrainHandle;
class TerrainService;
class TerrainRenderFeature;
class TerrainPageFeature;
struct TerrainHeightPagesTestAccess;

/// What a platform spends on height pages (design D5): the height cache's slots; the page table
/// (PageTable, design page-streaming-planets 3.4): the largest table kept dense, one tier, and the
/// blocks of the tiered table a larger terrain takes; the largest modifier overlay
/// (PageStreaming::HeightPageLayer, in system memory) a terrain may hold; and the CPU height
/// pages' bytes (the residency's one CPU budget, design D5) and synchronous window fetches a frame
/// (design D3, PageResidencyBudget). The page-table ring sizes its slots to the bound terrain's
/// tables (TerrainPageTableRing), so a large table costs memory only where a terrain
/// needs it; the overlay is built only for a terrain that pages, over the rect its height modifiers
/// reach. GE_TERRAIN_PAGE_SLOTS overrides the slots for soaks.
struct HeightPageProfile
{
    uint32 Slots = 0;
    uint32 DenseTableWords = 0;
    uint64 OverlayByteCap = 0;
    uint64 CpuBytes = 0;
    uint32 CpuSyncFetchPages = 0;
    uint32 TableMidBlocks = 0;
    uint32 TableLeafBlocks = 0;
};

/// Desktop: 512 slots (33 MiB); a page table is one dense tier while it fits 2^21 words (8 MiB a
/// ring slot: a square terrain of up to about 160,000 samples a side). A larger terrain is tiered
/// with 10 mid and 160 leaf blocks, two working sets at a pose (design page-streaming-planets 3.4):
/// a flat terrain of 2^22 samples a side takes 1,488,355 words (5.7 MiB). An overlay of up to 512 MiB
/// of pages (67.6 KB each, about 5.5 B per level-0 sample under a modifier), about 6 km^2 of modifiers
/// on a 0.25 m terrain wherever they lie, a few percent of a 16 GiB machine's memory: a 1 km^2 stamp
/// at 0.25 m holds about 95 MiB. Decoded pages on the CPU:
/// 64 MiB (about 990 pages, the uploads waiting and the CPU readers' pages together; the 512 drawn
/// pages fit with room for the windows) and up to 4 synchronous window fetches a frame inside the
/// 0.5 ms deadline: a store page from the file cache takes about 8 us, so 4; a generated page about
/// 0.36 ms, so 1 (Release, measured in slice 1c-iii); an SSD read that misses the file cache is not
/// measured (0.1 to 0.3 ms assumed).
inline constexpr HeightPageProfile kDesktopHeightPages{512u, 1u << 21, 512ull << 20, 64ull << 20, 4u, 10u, 160u};
/// Steam Deck: half the desktop slots (256, 16.5 MiB), a quarter of its dense table (2^19 words, 2 MiB
/// a ring slot: a square terrain of up to about 80,000 samples a side) and half its blocks; a quarter
/// of the desktop overlay (128 MiB of its 16 GiB shared with the GPU), which still holds 1 km^2 at
/// 0.25 m. CPU height pages: 32 MiB and 1 synchronous window fetch a frame (a microSD's random read takes milliseconds).
inline constexpr HeightPageProfile kSteamDeckHeightPages{256u, 1u << 19, 128ull << 20, 32ull << 20, 1u, 5u, 80u};

/// The profile of the platform this build targets.
HeightPageProfile PlatformHeightPageProfile();

/// The field id of the height pages in the residency manager.
inline constexpr uint32 kHeightPageField = 0u;

/// Why a terrain whose modifier overlay needs `overlayBytes` does not page under `profile` (over
/// the profile's overlay cap), worded with the fix, naming the terrain `name`; empty when it fits.
std::string OverlayRefusal(std::string_view name, uint64 overlayBytes, const HeightPageProfile& profile);

/// The modifier overlay a terrain may hold, set on its TiledTerrainData by the height page driver
/// each frame (TerrainHeightPageDriver).
struct HeightPageOverlayAllowance
{
    uint64 ByteCap = 0;  ///< the profile's overlay cap when the terrain has CPU pages, else 0
    bool OffGrid = false; ///< its pages are not on the tile lattice the overlay is baked on
};

/// The overlay rule: a terrain with height pages (`wouldPage`: TerrainHeightHasCpuPages) may hold an
/// overlay up to the profile's cap. The overlay lies on the tile lattice (latticeX x latticeZ:
/// tiles x (resolution - 1) + 1 per axis); a cooked store on another grid (sourceX x sourceZ, every
/// power-of-two raw heightmap) cannot take it, so it is OffGrid: it pages while no height modifier
/// reaches it, and keeps its height texture (OverlayGridRefusal) once one does.
HeightPageOverlayAllowance PageOverlayAllowance(bool wouldPage, uint32 sourceX, uint32 sourceZ, uint32 latticeX,
                                                uint32 latticeZ, const HeightPageProfile& profile);

/// Why a terrain whose cooked store (storeX x storeZ samples) is off its tile lattice (latticeX x
/// latticeZ) does not page while a height modifier reaches it, worded with the fix, naming the
/// terrain `name`.
std::string OverlayGridRefusal(std::string_view name, uint32 storeX, uint32 storeZ, uint32 latticeX, uint32 latticeZ);

/// Packs `table` into the GPU page-table words (CBTLayout.h binding-22 layout): the field's shape and
/// its tiers, its levels, every slot's arrival fade and the table's words.
void PackPageTableWords(const PageTable& table, const PageCacheGeometry& geometry, std::vector<uint32>& out);

/// One page's samples for its cache slot, uploaded by the GPU side this frame.
struct HeightPageUpload
{
    uint32 Slot = 0;
    /// The page's bytes: kPageSampleCount float32 heights (PageStreaming::HeightSamples), row-major,
    /// apron included.
    std::vector<uint8> Bytes;
};

/// A terrain-UV rect whose height changed source this frame: the CBT re-evaluates the bisectors
/// over it.
struct HeightPageDirtyRect
{
    uint32 Terrain = 0;
    uint32 TerrainGeneration = 0;
    float32 MinU = 0.0f;
    float32 MinV = 0.0f;
    float32 MaxU = 0.0f;
    float32 MaxV = 0.0f;
};

/// What a paged terrain asks for this frame.
struct PagedTerrainRequest
{
    uint32 Terrain = 0;           ///< the terrain's render handle (TerrainHandle::Index, ::Generation)
    uint32 TerrainGeneration = 0;
    uint64 SourceIdentity = 0;  ///< changes when the source or the pyramid changes (restarts the stream)
    std::string_view Name;      ///< the terrain entity's name, for the log lines (empty: its render index)
    /// The GPU draws the terrain's pages (TerrainHeightIsPaged, with a view). False streams its CPU
    /// pages only (the narrow CBT arm, pending modifiers, no view): no new GPU page, and no page table
    /// or dirty rect reaches the renderer. The pages it holds keep their slots and their entries current.
    bool GpuPages = true;
    PageStreaming::PageSource Source;
    PageRequestTerrain Shape;   ///< its pyramid in world meters; CacheSlots is set by TerrainPages
    float32 OriginY = 0.0f;     ///< world height of a normalized sample of 0
    float32 HeightScale = 0.0f; ///< world meters per normalized sample unit
    /// What the terrain's modifiers add to its source's pages (null: nothing), applied to every page
    /// as it is uploaded. A new version rewrites the resident pages that read OverlayChanged when
    /// the pages hold version OverlayChangedSince, else every page either overlay reaches.
    std::shared_ptr<const PageStreaming::HeightPageLayer> Overlay;
    uint64 OverlayVersion = 0;
    PageStreaming::PageSampleRect OverlayChanged; ///< level-0 samples the version changed from OverlayChangedSince
    uint64 OverlayChangedSince = 0;
};

/// The render handle index of a planet that has no render terrain yet (PlanetPagedRequest).
inline constexpr uint32 kNoRenderTerrain = 0xFFFFFFFFu;

/// A planet's height pages this frame (design page-streaming-planets 3.5): one stream on the planet
/// provider, requested by its sphere requester. The CBT draws them (VertexEval) on the wide arm. The
/// page colliders read the level-0 pages of its CPU windows by address (PlanetPageHeights); the other
/// CPU readers walk its sparse CPU directory (PlanetSurfaceQuery).
struct PlanetPagedRequest
{
    uint32 Planet = 0;          ///< the planet's key: its terrain entity's id
    /// The planet's center in world space: the dynamic bodies' windows (GatherBodyWindows) are placed
    /// relative to it.
    Mathematics::Vector3 Center;
    /// The render handle index of the terrain that draws the planet (TerrainHandle::Index), under
    /// which its page-table words are published; kNoRenderTerrain while it has none.
    uint32 RenderTerrain = kNoRenderTerrain;
    /// The GPU draws the planet's pages (the wide arm): its slots upload, its table is published and
    /// its page changes reach the CBT. False on the narrow arm, which keeps the closed-form relief.
    bool GpuPages = true;
    uint64 SourceIdentity = 0;  ///< changes when the relief or the pyramid changes (restarts the stream)
    std::string_view Name;      ///< the terrain entity's name, for the log lines
    PageStreaming::PageSource Source; ///< read on the first request and when the identity changes
    PlanetPageRequestTerrain Shape;   ///< CacheSlots and Heights are set by TerrainPages
    /// The narrow arm: a request past the cache's slots is made a level coarser until it fits.
    bool FitToSlots = false;
    /// What the planet's layers add to its source's pages (null: nothing), Modifiers first and Overlay
    /// (the sculpt) on top, with the version rule of PagedTerrainRequest::Overlay for both;
    /// OverlayChanged holds each face's level-0 samples the versions after OverlayChangedSince changed.
    std::shared_ptr<const PageStreaming::HeightPageLayer> Overlay;
    std::shared_ptr<const PageStreaming::HeightPageLayer> Modifiers;
    uint64 OverlayVersion = 0;
    PageStreaming::PageFaceRects OverlayChanged;
    uint64 OverlayChangedSince = 0;
};

/// A streamed planet's CPU windows as the last RequestPlanet left them, for its page colliders
/// (design page-streaming-planets-r4-colliders). Valid until the next RequestPlanet or Update.
struct PlanetCpuView
{
    uint64 Epoch = 0; ///< the stream's source identity: a restart changes it
    PlanetChartLayout Layout;
    float64 ReferenceRadius = 0.0;
    /// The level-0 pages that get a body: the core of each camera's and each dynamic body's window,
    /// each once. The window also keeps the pages these bodies read (AppendPlanetPageBodyReads).
    std::span<const PageStreaming::PageAddress> BodyPages;
    uint64 LayerVersion = 0; ///< the version of the planet's page layer its pages carry
};

/// One level of a page stream: the pages its requester wants, and how many of them are resident.
struct PageLevelCounts
{
    uint32 Wanted = 0;
    uint32 Resident = 0;
};

/// One cube face of a planet's page table, as the GPU reads it (get_terrain_stats).
struct HeightPageFaceTable
{
    std::vector<uint32> EntriesPerLevel; ///< per level, the pages that have a table entry
    /// Per page of the finest pinned level, row-major: the finest level with an entry at the page's center,
    /// the level a lambda-0 sample there resolves to; the level count when no level has one.
    std::vector<uint8> ResolvedLevel;
};

/// A page stream's counts per level (get_terrain_stats).
struct PageStreamCounts
{
    std::string Label;
    bool Planet = false;
    bool Surface = false; ///< a terrain's surface stream (its plane stores), not its height stream
    /// A planet stream: the times its request was made one level coarser to fit the field's slots.
    uint32 FitPasses = 0;
    /// A planet surface stream: the pages its request wants before the fit, at the field's pinned levels.
    uint32 FullDetailPages = 0;
    /// The pages the stream's loads made since it was configured (PageResidencyManager::PagesProduced).
    uint64 PagesProduced = 0;
    std::vector<PageLevelCounts> Levels;
    /// A planet's table: whether the GPU draws it, its finest pinned level and that level's pages a face
    /// side, and each face.
    bool Published = false;
    uint32 PinnedLevel = 0;
    uint32 PinnedPagesX = 0;
    uint32 PinnedPagesZ = 0;
    std::vector<HeightPageFaceTable> Faces;
};

/// Sets `out.Levels` to `wanted`'s pages per level of `levels` and how many of them `cache` holds for `stream`.
void CountWantedPages(const PageStreaming::PageCache& cache, uint32 stream,
                      std::span<const PageStreaming::PageWant> wanted, uint32 levels, PageStreamCounts& out);

/// GE_CBT_VALIDATE: logs the wanted and resident pages per level of `label`'s stream (`stream` follows the label:
/// empty for its height) when the line differs from `logged`, then keeps it there.
void LogPageResidencyChange(std::string_view label, std::string_view stream, const PageTable& table,
                            std::span<const PageStreaming::PageWant> wanted, std::string& logged);

class TerrainPages
{
public:
    TerrainPages(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io, const HeightPageProfile& profile,
                 const SurfacePageProfile& surfaceProfile);

    /// The pages `request`'s terrain needs for `cameras` this frame: its GPU pages, and its CPU
    /// window pages around `cameras` and the bodies GatherBodyWindows read. A terrain not requested
    /// between two Updates is forgotten at the second.
    void Request(const PagedTerrainRequest& request, std::span<const Mathematics::Vector3> cameras,
                 const PageLevelView& view);

    /// Reads the world positions of `world`'s dynamic physics bodies, the centers of this frame's CPU
    /// body windows (design D3: independent of the view, so a body far from every camera keeps its
    /// finest pages). Called before the frame's Requests.
    void GatherBodyWindows(ECS::World& world);

    /// Keeps a CPU window of finest pages around `center` at the next Update, as a camera's window
    /// does, for every paged terrain: a reader that needs the finest height away from every camera
    /// and body (a navigation bake waiting on a page) asks on each frame it waits.
    void RequestReaderWindow(const Mathematics::Vector3& center);

    /// Fetches the level-0 page under `worldPoint` of the paged terrain drawn with render handle
    /// `terrain` now, on the main thread (PageResidencyManager::FetchCpuPage): a pick reads the
    /// finest height wherever it lands. Returns whether that page is then on the CPU.
    bool FetchFinestPage(const TerrainHandle& terrain, const Mathematics::Vector3& worldPoint);

    /// The CPU height of the paged terrain drawn with render handle `terrain`, valid until the next
    /// Update; an invalid query when that terrain is not paged (TerrainService::SurfaceQuery resolves
    /// it from the terrain's component).
    SurfaceHeightQuery SurfaceQuery(const TerrainHandle& terrain) const;

    /// The level-0 pages of every CPU window of the paged terrain drawn with render handle `terrain`,
    /// each once: the cameras' windows, the readers', then the dynamic bodies' nearest a camera
    /// first, then every other level-0 page when level 0 has at most one tile's pages (9 x 9) and
    /// they fit a quarter of the CPU budget, as the last Update kept them. Empty when that terrain is
    /// not paged. Valid until the next Update.
    std::span<const PageStreaming::PageAddress> CpuWindowPages(const TerrainHandle& terrain) const;

    /// One frame: forgets the terrains not requested since the last Update, runs the residency,
    /// with the CPU windows' deadline-bounded fetch, brings each terrain's page table up to date and
    /// gathers the uploads and the dirty rects.
    void Update(uint64 frameIndex, float32 deltaSeconds, bool teleport, uint32 framesInFlight);

    /// True when `terrain` is not paged yet or its source identity differs: its next Request must
    /// carry the source (a terrain's source is built only then).
    bool NeedsSource(uint32 terrain, uint64 sourceIdentity) const;

    /// Requests planet `request.Planet`'s height pages for `cameras` (relative to the planet's
    /// center) this frame; a planet not requested in a frame is forgotten at the next Update.
    void RequestPlanet(const PlanetPagedRequest& request, std::span<const Mathematics::Vector3> cameras,
                       const PageLevelView& view);

    /// The surface pages planet `request.Planet` needs for `cameras` (relative to its center) this frame:
    /// call after its RequestPlanet. Nothing while the planet is not requested this frame, its GPU does
    /// not draw its pages or it has no render terrain.
    void RequestPlanetSurface(const PlanetSurfacePagedRequest& request, std::span<const PageRequestCamera> cameras,
                              const PageLevelView& view);

    /// True when planet `planet` has no stream on the source of `sourceIdentity`: its next
    /// RequestPlanet carries the source.
    bool NeedsPlanetSource(uint32 planet, uint64 sourceIdentity) const;
    /// The version of planet `planet`'s layer its pages hold (zero: none, or no such planet).
    uint64 PlanetOverlayVersion(uint32 planet) const;

    /// Planet `planet`'s CPU windows, or false when it is not streamed.
    bool FindPlanetCpuView(uint32 planet, PlanetCpuView& out) const;

    /// Copies level-0 page `address` of planet `planet` into `heights` (kPageSampleCount meters above
    /// the reference radius, apron included) with the planet's page layer applied, as the GPU draws
    /// it. False when the page is not on the CPU.
    bool PlanetPageHeights(uint32 planet, const PageStreaming::PageAddress& address,
                           std::vector<float32>& heights) const;

    /// The CPU height of planet `planet` (SurfaceHeightQuery's spherical arm) through its sparse CPU
    /// directory, valid until the next RequestPlanet or Update; neither valid nor spherical when it is
    /// not streamed.
    SurfaceHeightQuery PlanetSurfaceQuery(uint32 planet) const;

    /// Every page stream's wanted and resident pages per level, as of the last Update.
    void StreamCounts(std::vector<PageStreamCounts>& out) const;

    /// When `deviceEpoch` differs from the last one seen (the GPU cache was lost with its device),
    /// empties the cache and every page table, so every page is assigned and uploaded again.
    void RestartOnDeviceEpoch(uint64 deviceEpoch);

    /// Logs `refusal` (OverlayRefusal, OverlayGridRefusal) once for `terrain`
    /// until the terrain pages or its refusal changes: the terrain keeps its height texture meanwhile.
    void ReportRefusal(uint32 terrain, const std::string& refusal);

    uint32 SlotCount() const { return m_SlotCount; }
    const HeightPageProfile& Profile() const { return m_Profile; }
    const PageCacheGeometry& Geometry() const { return m_Geometry; }
    /// This frame's slot uploads. The GPU side takes them (moves them out) after every Update, the
    /// frame its page table is published: the next Update clears what was not taken.
    std::vector<HeightPageUpload>& Uploads() { return m_Uploads; }
    /// The rects whose height changed source since the caller last drained them (cleared by the
    /// caller): page assigns, releases, rewrites and arrival-fade steps, and whole terrains switching
    /// between their pages and their texture.
    std::vector<HeightPageDirtyRect>& DirtyRects() { return m_DirtyRects; }
    /// The pages whose GPU table entry, fade or slot texels changed since the caller last drained
    /// them (TerrainPageFeature::Publish moves them out), one rect a page; a reset or a planet's
    /// change covers its whole terrain.
    std::vector<PageChangeRect>& ChangeRects() { return m_ChangeRects; }
    /// The packed page table of a terrain or planet the GPU draws from its pages, by render handle
    /// index (its version bumped on every change), or null. A planet is listed here and in
    /// PagedTerrains only while every page of its pinned levels has its entry.
    const PackedPageTable* PackedTable(uint32 terrain) const;
    /// PackedPageTable::HashWords of a whole pack of the page table of PackedTable(terrain) as it is now: the
    /// words that a whole write gives. 0 when PackedTable is null. It packs the table on each call: call it
    /// on request only, never every frame.
    uint64 WholeTableHash(uint32 terrain) const;
    /// Every terrain and planet the GPU draws from its pages (render handle indices), in no order.
    void PagedTerrains(std::vector<uint32>& out) const;
    const PageStreaming::PageResidencyManager& Residency() const { return m_Residency; }
    /// The surface streams, in the same residency.
    TerrainSurfacePages& Surface() { return m_Surface; }
    const TerrainSurfacePages& Surface() const { return m_Surface; }

private:
    // UpdateHeightPages drains the planets' dirty regions into the sphere dirty union.
    friend void UpdateHeightPages(TerrainService& service, TerrainRenderFeature& terrainFeature,
                                  TerrainPageFeature& pages, uint64 frameIndex, float32 deltaSeconds,
                                  uint32 framesInFlight);
    friend struct TerrainHeightPagesTestAccess;

    /// The planets' face rects whose height changed source since the caller last drained them, one union
    /// per face in CBT face UV: page assigns, releases, rewrites, arrival-fade steps, and the neighbor face's edge
    /// band whose corners resolve on the page's face (CBT_PlanetPagePoint). The caller clears them.
    CBTTerrain::SphereEditRegions& PlanetDirtyRegions() { return m_PlanetDirty; }

    struct PagedTerrain
    {
        uint32 Stream = 0;
        uint32 Generation = 0;
        uint64 Identity = 0;
        uint64 SourceIdentity = 0; ///< the source its stream holds: a GPU cache restart keeps its pages
        float32 OriginY = 0.0f;
        float32 HeightScale = 0.0f;
        std::string Label;        ///< how the log lines name it: the entity's name, or its render index
        PageRequestTerrain Shape; ///< the placement its requester is configured with
        PageRequester Requester;
        PageTable Table;
        std::shared_ptr<const PageStreaming::HeightPageLayer> Overlay;
        uint64 OverlayVersion = 0;
        PageStreaming::PageSampleRect OverlayChanged; ///< what OverlayVersion changed from OverlayChangedSince
        uint64 OverlayChangedSince = 0;
        PackedPageTable Packed;
        bool Requested = false;
        bool GpuPages = false;
        std::vector<PageStreaming::PageWant> WantedPages; ///< this frame's requests
        std::string LoggedResidency;       ///< GE_CBT_VALIDATE: the residency line last logged
    };

    struct PagedPlanet
    {
        uint32 Stream = 0;
        uint64 Identity = 0;
        std::string Label;
        PlanetPageRequestTerrain Shape; ///< the pyramid and generator its requester is configured with
        PlanetPageRequester Requester;
        PageTable Table; ///< six faces of FaceSamples a side
        PackedPageTable Packed;
        uint32 RenderTerrain = kNoRenderTerrain;
        bool GpuPages = false;
        /// Every pinned page has its entry, so every sample resolves: the GPU may draw the planet's pages.
        bool Published = false;
        uint32 FitPasses = 0; ///< this frame's passes a level coarser (BuildWithinSlots)
        std::vector<PageStreaming::PageWant> WantedPages; ///< this frame's requests
        bool Requested = false;
        std::shared_ptr<const PageStreaming::HeightPageLayer> Overlay;
        std::shared_ptr<const PageStreaming::HeightPageLayer> Modifiers;
        uint64 OverlayVersion = 0;
        PlanetChartLayout Layout;
        float64 ReferenceRadius = 0.0;
        Mathematics::Vector3 Center; ///< the last request's center in world space
        std::vector<PageStreaming::PageAddress> BodyPages; ///< PlanetCpuView::BodyPages
        /// The CPU window: BodyPages and the pages they read, for the BodyPages it was built from.
        std::vector<PageStreaming::PageAddress> CpuWindow;
        std::vector<PageStreaming::PageAddress> CpuWindowOf;
        /// The points BodyPages was built from: the cameras (planet-local), the dynamic bodies (world)
        /// and the planet's center. False until the first build on the current stream.
        bool BodyPagesBuilt = false;
        std::vector<Mathematics::Vector3> BodyPagesCameras;
        std::vector<Mathematics::Vector3> BodyPagesBodies;
        Mathematics::Vector3 BodyPagesCenter;
    };

    void Configure(PagedTerrain& terrain, const PagedTerrainRequest& request);
    std::shared_ptr<const PageStreaming::HeightPageLayer> FittingOverlay(
        const PagedPlanet& planet, const std::shared_ptr<const PageStreaming::HeightPageLayer>& layer) const;
    static void ApplyPlanetLayers(const PagedPlanet& planet, const PageStreaming::PageAddress& address,
                                  std::span<float32> heights);
    void AppendPlanetTable(const PagedPlanet& planet, PageStreamCounts& out) const;
    void SetDiscCameras(std::span<const Mathematics::Vector3> cameras);
    void RequestCpuWindows(const PagedTerrain& terrain, std::span<const Mathematics::Vector3> cameras);
    void AppendBodyWindows(const PagedTerrain& terrain, std::span<const Mathematics::Vector3> cameras);
    void OrderBodyWindows(std::span<const Mathematics::Vector3> cameras, const Mathematics::Vector3& origin);
    void RequestPlanetCpuWindows(PagedPlanet& planet, const Mathematics::Vector3& center,
                                 std::span<const Mathematics::Vector3> cameras);
    void BuildPlanetCpuWindow(PagedPlanet& planet, const Mathematics::Vector3& center,
                              std::span<const Mathematics::Vector3> cameras);
    void CollectStreamChanges(uint32 terrainKey, PagedTerrain& terrain);
    void ResetPlanetTable(PagedPlanet& planet);
    void CollectPlanetChanges(PagedPlanet& planet);
    void AddPlanetWholeDirty();
    void AddPlanetDirtyPage(const PagedPlanet& planet, const PageStreaming::PageAddress& address);
    void AddPlanetDirtyRewrite(const PagedPlanet& planet, const PageStreaming::PageRewrite& rewrite);
    void AddPlanetDirtyFaceRect(uint8 face, uint32 level, uint32 samples, float32 minU, float32 minV, float32 maxU,
                                float32 maxV);
    void AddPlanetDirtyRect(uint32 face, float32 minU, float32 minV, float32 maxU, float32 maxV);
    void AddWholeTerrainDirty(uint32 terrainKey, const PagedTerrain& terrain);
    void AddDirtyPage(uint32 terrainKey, const PagedTerrain& terrain, const PageStreaming::PageAddress& address,
                      HeightPageDirtyRect& rect, bool& any) const;
    void AddDirtyRewrite(uint32 terrainKey, const PagedTerrain& terrain, const PageStreaming::PageRewrite& rewrite,
                         HeightPageDirtyRect& rect, bool& any) const;

    PageStreaming::PageResidencyManager m_Residency;
    HeightPageProfile m_Profile;
    uint32 m_SlotCount = 0;
    PageCacheGeometry m_Geometry;
    uint32 m_FramesInFlight = 0;
    uint64 m_DeviceEpoch = 0;
    uint32 m_NextStream = 1;
    // Every table version comes from this one counter, so a terrain paged again at a render index
    // never repeats a version the CBT's per-ring-slot upload check has seen at that index.
    uint64 m_TableVersion = 0;
    FlatMap<uint32, PagedTerrain> m_Terrains; // by render index: a few terrains
    FlatMap<uint32, PagedPlanet> m_Planets;   // by terrain entity id: a few planets
    std::vector<PageStreaming::PageWant> m_Wants; // reused per request
    std::vector<PageRequestCamera> m_Cameras;     // reused per request: the geometry's cameras, every way
    std::vector<Mathematics::Vector3> m_BodyWindowCenters;
    std::vector<Mathematics::Vector3> m_ReaderWindowCenters; // RequestReaderWindow's, until the next Update
    std::vector<PageStreaming::PageAddress> m_WindowPages; // reused per request
    std::vector<std::pair<float32, uint32>> m_BodyOrder;   // reused per request: (squared distance, body)
    bool m_BodiesOverBound = false;                       // the bound was logged and still holds
    std::vector<HeightPageUpload> m_Uploads;
    std::vector<HeightPageDirtyRect> m_DirtyRects;
    std::vector<PageChangeRect> m_ChangeRects;
    CBTTerrain::SphereEditRegions m_PlanetDirty;
    std::vector<uint32> m_Forgotten; // reused per Update
    std::vector<PageStreaming::PageAddress> m_LateEntries; // reused per Update
    std::vector<PageStreaming::PageFaceRect> m_FaceRects;   // reused by RefreshLayeredPages
    FlatMap<uint32, std::string> m_Refusals; // per terrain, the refusal last logged
    TerrainSurfacePages m_Surface; // after m_Residency and m_NextStream, which it holds
};

} // namespace GameEngine::TerrainECS
