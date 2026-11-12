#pragma once

#include "TerrainECS/PackedPageTable.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/PageTable.h"
#include "TerrainECS/PagedHeightSampler.h"
#include "TerrainECS/Planet/PlanetPageRequester.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Components
{
struct TerrainSurfaceRulesEffect;
}

namespace GameEngine::PageStreaming
{
class GeneratedHeightPageProvider;
class PageResidencyManager;
class PageStoreAccess;
} // namespace GameEngine::PageStreaming

namespace GameEngine::TerrainECS
{

struct PageStreamCounts;
struct TerrainSurfacePageSource;

/// What a platform spends on surface pages (design D5): the surface field's cache slots. Each slot
/// holds one page of every plane a terrain has. The field holds PlanetSlots while it streams a planet.
struct SurfacePageProfile
{
    uint32 Slots = 0;       ///< a field with planar terrains only
    uint32 PlanetSlots = 0; ///< a field that holds a planet; the larger of the two counts
};

/// Desktop: 2,048 slots for planar terrains (168 MiB for the splat and BC5 normal planes, 366 MiB with
/// grass control and color). With a planet, the slots of an Earth-sized planet's wedge at 1,080 rows in
/// a 16:9 view, one texel a pixel (4,096: 336 and 732 MiB).
SurfacePageProfile DesktopSurfacePageProfile();
/// Steam Deck: 1,024 slots with or without a planet (84 MiB for the splat and normal planes).
inline constexpr SurfacePageProfile kSteamDeckSurfacePages{1024u, 1024u};

/// The profile of the platform this build targets. A build that is not Release reads
/// GE_SURFACE_PAGE_SLOTS, a test override of both counts.
SurfacePageProfile PlatformSurfacePageProfile();

/// The surface pages' level rule target: one texel a pixel (lambda_s, planets design 3.5).
inline constexpr float32 kSurfacePageTargetPixels = 1.0f;

/// The field id of the surface pages in the residency manager.
inline constexpr uint32 kSurfacePageField = 1u;

/// What a terrain whose surface is paged (TerrainSurfaceIsPaged) asks for this frame.
struct SurfacePagedRequest
{
    uint32 Terrain = 0; ///< the terrain's render handle (TerrainHandle::Index, ::Generation)
    uint32 TerrainGeneration = 0;
    std::string_view Name; ///< the terrain entity's name, for the log lines (empty: its render index)
    const TerrainSurfacePageSource* Source = nullptr;
    PageRequestTerrain Shape; ///< its pyramid in world meters; CacheSlots is set by TerrainSurfacePages
};

/// What a planet asks for its generated surface pages this frame (planets note 3.14): the splat and
/// the normal of each page, generated from its height window and the rule rows.
struct PlanetSurfacePagedRequest
{
    uint32 Planet = 0;     ///< the planet's key: its terrain entity's id, as PlanetPagedRequest::Planet
    std::string_view Name; ///< the terrain entity's name, for the log lines
    const Components::TerrainSurfaceRulesEffect* Rules = nullptr; ///< read when RulesIdentity changes
    uint64 RulesIdentity = 0; ///< changes when the rows change: every page is generated again
    /// The level-0 face samples the layer versions after LayerChangedSince changed, as
    /// PlanetPagedRequest::OverlayChanged.
    PageStreaming::PageFaceRects LayerChanged;
    uint64 LayerChangedSince = 0;
};

/// A planet's height inputs to its surface pages, as its height stream holds them (TerrainPages).
struct PlanetSurfaceHeights
{
    uint32 RenderTerrain = 0; ///< the render handle index the surface table is published under
    uint64 Identity = 0;      ///< the height stream's source identity: a change restarts the surface stream
    PlanetPageRequestTerrain Shape; ///< the pyramid and its generator (Shape.Heights)
    /// The layers the height pages add, in order (the modifiers, then the sculpt), and their version.
    std::array<std::shared_ptr<const PageStreaming::HeightPageLayer>, 2> Layers;
    uint64 LayerVersion = 0;
};

/// One page of every plane for a surface cache slot, uploaded by the GPU side this frame.
struct SurfacePageUpload
{
    uint32 Slot = 0;
    uint32 PlaneMask = 0;     ///< the planes in Bytes (TerrainSurfacePageSource::PlaneMask)
    std::vector<uint8> Bytes; ///< each plane's PagePayloadBytes in turn, in PageFieldKind order
};

/// The surface pages of every surface-paged terrain, on the main thread: each terrain's stream over
/// its plane stores, requested over the view wedge at one texel a pixel, its page table, and the slot
/// uploads the GPU side takes each frame. The streams live in the residency of TerrainPages, its owner.
class TerrainSurfacePages
{
public:
    /// Streams in `residency`, under stream ids taken from `nextStream`, the owner's one counter.
    TerrainSurfacePages(PageStreaming::PageResidencyManager& residency, const SurfacePageProfile& profile,
                        const PageTableBudget& tableBudget, uint32& nextStream);

    /// The pages `request`'s terrain needs for `cameras` this frame. A terrain not requested between
    /// two Updates is forgotten at the second.
    void Request(const SurfacePagedRequest& request, std::span<const PageRequestCamera> cameras,
                 const PageLevelView& view);
    /// The surface pages planet `request.Planet` needs for `cameras` (relative to its center) this frame,
    /// a level coarser everywhere while they pass the field's slots. A new layer version or new rows
    /// rewrite the resident pages they change; each keeps its old content until then.
    void RequestPlanet(const PlanetSurfacePagedRequest& request, const PlanetSurfaceHeights& heights,
                       std::span<const PageRequestCamera> cameras, const PageLevelView& view);
    /// The pages the last request of terrain `terrain` (render handle index) wanted, with their distances;
    /// empty when it has none.
    /// TerrainExtraction writes them after TerrainModifiers runs, so TerrainModifiers reads the last frame's pages.
    std::span<const PageStreaming::PageWant> WantedPages(uint32 terrain) const;

    /// (Re)starts the surface field's cache for the renderer's frames in flight, empties every
    /// page table and keeps each stream's loaded pages.
    void ConfigureField(uint32 framesInFlight);
    /// Before the residency's Update: ends the streams of the terrains not requested since the last call.
    void ForgetUnrequested();
    /// After the residency's Update: brings each terrain's page table up to date and gathers the uploads.
    void Collect();

    /// This frame's slot uploads. The GPU side takes them after every Update: the next Update clears
    /// what was not taken.
    std::vector<SurfacePageUpload>& Uploads() { return m_Uploads; }
    /// The pages whose table entry, fade or slot texels changed since the caller last drained them
    /// (TerrainPageFeature::Publish moves them out), one rect a page; a reset covers its whole terrain.
    std::vector<PageChangeRect>& ChangeRects() { return m_ChangeRects; }
    /// The packed surface page table of a terrain by render handle index (its version bumped on every
    /// change) and the planes every resident page holds (TerrainSurfacePageSource::PlaneMask), or null.
    const PackedPageTable* PackedTable(uint32 terrain, uint32& planeMask) const;
    /// The render handle indices of the terrains whose surface is paged.
    void PagedTerrains(std::vector<uint32>& out) const;
    /// Appends each surface stream's wanted and resident pages per level to `out`, as of the last Update.
    void StreamCounts(std::vector<PageStreamCounts>& out) const;
    /// The last surface request of planet `planet` (its terrain entity's id) against the field's slots;
    /// zero passes and pages while the planet streams no surface.
    PageSlotFit PlanetFit(uint32 planet) const;
    /// The field's slots now: SurfacePageProfile::Slots, or PlanetSlots while it holds a planet.
    uint32 Slots() const { return m_Slots; }
    /// The surface field's cache geometry: every plane cache packs its slots in these rows. It changes
    /// when the first planet comes and when the last one goes (SurfacePageProfile::PlanetSlots).
    const PageCacheGeometry& Geometry() const { return m_Geometry; }
    /// The surface field's profile: its slots decide the level every surface stream pins.
    const SurfacePageProfile& Profile() const { return m_Profile; }

private:
    struct PagedSurface
    {
        uint32 Stream = 0;
        uint32 Generation = 0;
        std::string Label;
        std::vector<const PageStreaming::PageStoreAccess*> Planes; ///< the source its stream holds
        uint32 PlaneMask = 0;
        // The planes the shaders read: the planes every resident page holds (old AND new while
        // a plane change rewrites the pages).
        uint32 ShownPlaneMask = 0;
        PageRequestTerrain Shape;
        PageRequester Requester;
        // The planet form: six faces of generated pages (PlanetSurfacePageProvider).
        bool Planet = false;
        PlanetPageRequester PlanetRequester;
        PlanetPageRequestTerrain PlanetShape;
        uint32 PlanetEntity = 0; ///< PlanetSurfacePagedRequest::Planet
        PageSlotFit Fit;         ///< the last request's fit to the field's slots
        uint64 Identity = 0;
        uint64 LayerVersion = 0;
        uint64 RulesIdentity = 0;
        /// The layers of LayerVersion: a rebuilt layer rewrites every page the old one reached.
        std::array<std::shared_ptr<const PageStreaming::HeightPageLayer>, 2> Layers;
        PageTable Table;
        PackedPageTable Packed;
        bool Requested = false;
        std::vector<PageStreaming::PageWant> WantedPages; ///< the last request
        std::string LoggedResidency;                         ///< GE_CBT_VALIDATE: the residency line last logged
    };

    void Configure(PagedSurface& surface, const SurfacePagedRequest& request);
    void ChangePlanes(PagedSurface& surface, const SurfacePagedRequest& request);
    void ConfigurePlanet(uint32 terrain, PagedSurface& surface, const PlanetSurfacePagedRequest& request,
                         const PlanetSurfaceHeights& heights);
    void ChangePlanetInputs(PagedSurface& surface, const PlanetSurfacePagedRequest& request,
                            const PlanetSurfaceHeights& heights, bool rulesChanged);
    void ResetTable(uint32 terrain, PagedSurface& surface);
    void CollectStreamChanges(uint32 terrain, PagedSurface& surface);
    // Gives the field `slots` and restarts its cache when the count changes. Every stream stays configured;
    // its slots and table entries are gone, so its wanted pages load again.
    void SizeField(uint32 slots);

    PageStreaming::PageResidencyManager& m_Residency;
    SurfacePageProfile m_Profile;
    uint32 m_Slots = 0; ///< the field's slots now: m_Profile.Slots, or PlanetSlots while it holds a planet
    PageTableBudget m_TableBudget;
    PageCacheGeometry m_Geometry;
    uint32& m_NextStream;
    uint32 m_FramesInFlight = 0;
    uint64 m_TableVersion = 0;
    FlatMap<uint32, PagedSurface> m_Surfaces; // by render index: a few terrains
    std::vector<PageStreaming::PageWant> m_Wants; // reused per request
    std::vector<SurfacePageUpload> m_Uploads;
    std::vector<PageChangeRect> m_ChangeRects;
    std::vector<uint32> m_Forgotten; // reused per Update
    std::vector<PageStreaming::PageAddress> m_LateEntries; // reused per Update
    std::vector<PageStreaming::PageFaceRect> m_FaceRects;  // reused by RefreshLayeredPages
};

} // namespace GameEngine::TerrainECS
