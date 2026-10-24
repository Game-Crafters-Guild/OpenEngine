#pragma once

#include "PageStreaming/HeightPageOverlay.h"
#include "PageStreaming/PageResidency.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/PagedHeightSampler.h"
#include "TerrainECS/SurfaceHeightQuery.h"
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

// The height pages of every paged terrain of the process, on the main thread: the one residency
// manager (design D-c: one per process, owned by TerrainService), each terrain's stream, level rule
// and page table, and what the GPU side takes each frame (the slots to upload, the page-table words,
// the terrain-UV rects whose height changed source); and the CPU windows the CPU readers keep
// (design D3) in the same residency, so a page is fetched once for both, read through
// SurfaceHeightQuery with the terrain's overlay.
namespace GameEngine::TerrainECS
{

struct TerrainHandle;

/// What a platform spends on height pages (design D5): the height cache's slots, the largest
/// page table one GPU page-table ring slot may hold, the largest modifier overlay
/// (PageStreaming::HeightPageOverlay, in system memory) a terrain may hold, and the CPU height
/// pages' bytes (the residency's one CPU budget, design D5) and synchronous window fetches a frame
/// (design D3, PageResidencyBudget). The CBT sizes its ring
/// slots to the bound terrain's table, up to the table cap (CBTResources::ProvisionPageTableWords),
/// so the cap costs memory only where a terrain needs it; the overlay is built only for a terrain
/// that pages, over the rect its height modifiers reach. GE_TERRAIN_PAGE_SLOTS overrides the slots
/// for soaks.
struct HeightPageProfile
{
    uint32 Slots = 0;
    uint32 TableWordCap = 0;
    uint64 OverlayByteCap = 0;
    uint64 CpuBytes = 0;
    uint32 CpuSyncFetchPages = 0;
};

/// Desktop: 512 slots (33 MiB); tables up to 2^21 words (8 MiB a ring slot), so the 60 x 15 km
/// terrain at 0.25 m (1,174,877 words) pages; an overlay up to 512 MiB, about 6 km^2 of
/// modifiers on a 0.25 m terrain (about 5.3 B per level-0 sample), a few percent of a 16 GiB
/// machine's memory: a 1 km^2 stamp at 0.25 m holds about 82 MiB. Decoded pages on the CPU: 64 MiB
/// (about 990 pages, the uploads waiting and the CPU readers' pages together; the 512 drawn pages
/// fit with room for the windows) and up to 4 synchronous window fetches a frame inside the 0.5 ms
/// deadline: a store page from the file cache takes about 8 us, so 4; a generated page about
/// 0.36 ms, so 1 (Release, measured in slice 1c-iii); an SSD read that misses the file cache is not
/// measured (0.1 to 0.3 ms assumed).
inline constexpr HeightPageProfile kDesktopHeightPages{512u, 1u << 21, 512ull << 20, 64ull << 20, 4u};
/// Steam Deck: half the desktop slots (256, 16.5 MiB) and a quarter of its table cap (2^19 words,
/// 2 MiB a ring slot): about 24 x 6 km at 0.5 m, or 60 x 15 km at 1 m; a quarter of the desktop
/// overlay (128 MiB of its 16 GiB shared with the GPU), which still holds 1 km^2 at 0.25 m. CPU height
/// pages: 32 MiB and 1 synchronous window fetch a frame (a microSD's random read takes milliseconds).
inline constexpr HeightPageProfile kSteamDeckHeightPages{256u, 1u << 19, 128ull << 20, 32ull << 20, 1u};

/// The profile of the platform this build targets.
HeightPageProfile PlatformHeightPageProfile();

/// The field id of the height pages in the residency manager.
inline constexpr uint32 kHeightPageField = 0u;

/// The words of a field's GPU page table (CBTLayout.h binding-22 layout) for a field of
/// samplesX x samplesZ level-0 samples and a cache of slotCount slots.
uint64 PageTableWordCount(uint32 samplesX, uint32 samplesZ, uint32 slotCount);

/// Why a terrain of samplesX x samplesZ level-0 samples does not page under `profile` (its page
/// table is over the profile's cap), worded with the fix, naming the terrain `name`; empty when
/// it fits.
std::string PageTableRefusal(std::string_view name, uint32 samplesX, uint32 samplesZ, const HeightPageProfile& profile);

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

/// Packs `table` into the GPU page-table words (CBTLayout.h binding-22 layout): the field's shape,
/// its levels, every slot's arrival fade and the entries.
void PackPageTableWords(const PageTable& table, const PageCacheGeometry& geometry, std::vector<uint32>& out);

/// One page's samples for its cache slot, uploaded by the GPU side this frame.
struct HeightPageUpload
{
    uint32 Slot = 0;
    std::vector<float32> Samples; ///< kPageSampleCount, row-major, apron included
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
    /// pages only (the narrow CBT arm, pending modifiers, a page table over the GPU cap, no view): no
    /// GPU page, page table or dirty rect.
    bool GpuPages = true;
    PageStreaming::PageSource Source;
    PageRequestTerrain Shape;   ///< its pyramid in world meters; CacheSlots is set by TerrainHeightPages
    float32 OriginY = 0.0f;     ///< world height of a normalized sample of 0
    float32 HeightScale = 0.0f; ///< world meters per normalized sample unit
    /// What the terrain's modifiers add to its source's pages (null: nothing), applied to every page
    /// as it is uploaded. A new version rewrites the resident pages that read OverlayChanged when
    /// the pages hold version OverlayChangedSince, else every page either overlay reaches.
    std::shared_ptr<const PageStreaming::HeightPageOverlay> Overlay;
    uint64 OverlayVersion = 0;
    PageStreaming::PageSampleRect OverlayChanged; ///< level-0 samples the version changed from OverlayChangedSince
    uint64 OverlayChangedSince = 0;
};

class TerrainHeightPages
{
public:
    TerrainHeightPages(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io, const HeightPageProfile& profile);

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
    /// first, as the last Update kept them. Empty when that terrain is not paged. Valid until the
    /// next Update.
    std::span<const PageStreaming::PageAddress> CpuWindowPages(const TerrainHandle& terrain) const;

    /// One frame: forgets the terrains not requested since the last Update, runs the residency,
    /// with the CPU windows' deadline-bounded fetch, brings each terrain's page table up to date and
    /// gathers the uploads and the dirty rects.
    void Update(uint64 frameIndex, float32 deltaSeconds, bool teleport, uint32 framesInFlight);

    /// True when `terrain` is not paged yet or its source identity differs: its next Request must
    /// carry the source (a terrain's source is built only then).
    bool NeedsSource(uint32 terrain, uint64 sourceIdentity) const;

    /// When `deviceEpoch` differs from the last one seen (the GPU cache was lost with its device),
    /// empties the cache and every page table, so every page is assigned and uploaded again.
    void RestartOnDeviceEpoch(uint64 deviceEpoch);

    /// Logs `refusal` (PageTableRefusal) once for `terrain` until the terrain pages or its refusal
    /// changes: the terrain keeps its height texture meanwhile.
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
    /// The page-table words of a terrain the GPU draws from its pages and their version (bumped on
    /// every change), or null.
    const std::vector<uint32>* TableWords(uint32 terrain, uint64& version) const;
    /// Every terrain the GPU draws from its pages (render handle indices), in no order.
    void PagedTerrains(std::vector<uint32>& out) const;
    const PageStreaming::PageResidencyManager& Residency() const { return m_Residency; }

private:
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
        HeightPageRequester Requester;
        PageTable Table;
        std::shared_ptr<const PageStreaming::HeightPageOverlay> Overlay;
        uint64 OverlayVersion = 0;
        PageStreaming::PageSampleRect OverlayChanged; ///< what OverlayVersion changed from OverlayChangedSince
        uint64 OverlayChangedSince = 0;
        std::vector<uint32> Words;
        uint64 Version = 0;
        bool Requested = false;
        bool GpuPages = false;
        std::vector<uint32> WantedEntries; ///< GE_CBT_VALIDATE: this frame's wanted pages, as table entry indices
        std::string LoggedResidency;       ///< GE_CBT_VALIDATE: the residency line last logged
    };

    void Configure(PagedTerrain& terrain, const PagedTerrainRequest& request);
    void RequestCpuWindows(const PagedTerrain& terrain, std::span<const Mathematics::Vector3> cameras);
    void AppendBodyWindows(const PagedTerrain& terrain, std::span<const Mathematics::Vector3> cameras);
    void CollectStreamChanges(uint32 terrainKey, PagedTerrain& terrain);
    void LogResidencyChange(PagedTerrain& terrain);
    void AddWholeTerrainDirty(uint32 terrainKey, const PagedTerrain& terrain);
    void AddDirtyPage(uint32 terrainKey, const PagedTerrain& terrain, const PageStreaming::PageAddress& address,
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
    std::vector<PageStreaming::PageWant> m_Wants; // reused per request
    std::vector<Mathematics::Vector3> m_BodyWindowCenters;
    std::vector<Mathematics::Vector3> m_ReaderWindowCenters; // RequestReaderWindow's, until the next Update
    std::vector<PageStreaming::PageAddress> m_WindowPages; // reused per request
    std::vector<std::pair<float32, uint32>> m_BodyOrder;   // reused per request: (squared distance, body)
    bool m_BodiesOverBound = false;                       // the bound was logged and still holds
    std::vector<HeightPageUpload> m_Uploads;
    std::vector<HeightPageDirtyRect> m_DirtyRects;
    std::vector<uint32> m_Forgotten; // reused per Update
    FlatMap<uint32, std::string> m_Refusals; // per terrain, the refusal last logged
};

} // namespace GameEngine::TerrainECS
