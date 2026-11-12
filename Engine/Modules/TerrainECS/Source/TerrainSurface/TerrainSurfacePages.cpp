#include "TerrainECS/TerrainSurface/TerrainSurfacePages.h"

#include "TerrainECS/TerrainSurface/TerrainSurfacePageSource.h"
#include "TerrainECS/Planet/PlanetSurfacePageProvider.h"
#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainHeightSource.h"
#include "TerrainECS/TerrainPages.h"
#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageLoader.h"
#include "PageStreaming/PageResidency.h"
#include "Logger/Logger.h"
#include "Types/ParseNumber.h"
#include "PageWedge.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <numbers>
#include <optional>

namespace GameEngine::TerrainECS
{

SurfacePageProfile DesktopSurfacePageProfile()
{
    // The landing eye 1.7 m over an Earth of 4,000 m relief amplitude (0.5 m texel, 15 streamed levels): the
    // reach ends at the horizon over the floor sphere, from the eye and from the top of the relief.
    constexpr float64 kRadius = 6371000.0;
    constexpr float64 kReliefReach = 6000.0; ///< 4,000 m of amplitude reaches 1.5 x that either way
    constexpr float64 kEyeHeight = 1.7;
    constexpr float32 kTexel = 0.5f;
    constexpr uint32 kStreamedLevels = 15u;
    constexpr float32 kAspect = 16.0f / 9.0f;
    constexpr float32 kVerticalFov = std::numbers::pi_v<float32> / 3.0f;
    const float64 floorRadius = kRadius - kReliefReach;
    const float64 eye = kRadius + kEyeHeight;
    const float64 top = kRadius + kReliefReach;
    const float64 floorSquared = floorRadius * floorRadius;
    const float64 horizon = std::sqrt(eye * eye - floorSquared) + std::sqrt(top * top - floorSquared);
    const float32 halfFov = std::atan(kAspect * std::tan(0.5f * kVerticalFov));
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, kVerticalFov, kSurfacePageTargetPixels);
    constexpr uint32 kPlanarSlots = 2048u;
    return SurfacePageProfile{
        kPlanarSlots, WedgePageCacheSlots(view, halfFov, kTexel, static_cast<float32>(horizon), kStreamedLevels)};
}

SurfacePageProfile PlatformSurfacePageProfile()
{
#if defined(PLATFORM_STEAMDECK)
    SurfacePageProfile profile = kSteamDeckSurfacePages;
#else
    SurfacePageProfile profile = DesktopSurfacePageProfile();
#endif
#ifndef NDEBUG
    // GE_SURFACE_PAGE_SLOTS sets both counts in a build that is not Release, for a capture of a
    // smaller pool (a Steam Deck look on the desktop). A Release build never reads it.
    if (const char* slots = std::getenv("GE_SURFACE_PAGE_SLOTS"))
    {
        const std::optional<uint64_t> count = ParseUnsigned(slots);
        if (count && *count > 0u && *count <= std::numeric_limits<uint32>::max())
            profile = SurfacePageProfile{static_cast<uint32>(*count), static_cast<uint32>(*count)};
    }
#endif
    return profile;
}

bool TerrainSurfaceIsPaged(const TerrainHeightSourceInputs& height, const TerrainSurfacePageSource& surface)
{
    return TerrainHeightIsPaged(height) && !surface.Planes.empty() && surface.PinnedLevelsBaked;
}

TerrainSurfacePages::TerrainSurfacePages(PageStreaming::PageResidencyManager& residency,
                                         const SurfacePageProfile& profile, const PageTableBudget& tableBudget,
                                         uint32& nextStream)
    : m_Residency(residency)
    , m_Profile(profile)
    , m_Slots(profile.Slots)
    , m_TableBudget(tableBudget)
    , m_Geometry(MakePageCacheGeometry(profile.Slots))
    , m_NextStream(nextStream)
{
}

namespace
{

// TerrainSurfacePageSource::PlaneMask's bit of plane `kind`.
constexpr uint32 PlaneBit(PageStreaming::PageFieldKind kind)
{
    return 1u << (static_cast<uint32>(kind) - static_cast<uint32>(PageStreaming::PageFieldKind::Splat));
}

// The planes a planet's generated surface page holds: the splat, then the normal (MakePagePlanes).
constexpr uint32 kPlanetSurfacePlaneMask =
    PlaneBit(PageStreaming::PageFieldKind::Splat) | PlaneBit(PageStreaming::PageFieldKind::Normal);

std::array<const PageStreaming::HeightPageLayer*, 2> LayerPointers(
    const std::array<std::shared_ptr<const PageStreaming::HeightPageLayer>, 2>& layers)
{
    return {layers[0].get(), layers[1].get()};
}

PageStreaming::PageSource PlanetSurfaceSource(const PlanetSurfacePagedRequest& request,
                                              const PlanetSurfaceHeights& heights)
{
    const std::array<const PageStreaming::HeightPageLayer*, 2> layers = LayerPointers(heights.Layers);
    auto provider = std::make_shared<const PlanetSurfacePageProvider>(heights.Shape.Heights, layers, *request.Rules,
                                                                      heights.Shape.ReferenceRadius);
    PageStreaming::PageSource source;
    source.GeneratedPlanes = PlanetSurfacePageProvider::MakePagePlanes(std::move(provider));
    return source;
}

} // namespace

void TerrainSurfacePages::ResetTable(uint32 terrain, PagedSurface& surface)
{
    if (surface.Planet)
    {
        const std::span<const PageStreaming::PageStoreLevel> levels = surface.PlanetRequester.Levels();
        surface.Table.Reset(levels.front().SamplesX, levels.front().SamplesZ, PageStreaming::kPageStoreMaxFaces,
                            m_Slots, m_TableBudget);
        surface.Packed.Update(surface.Table, m_Geometry, ++m_TableVersion);
        m_ChangeRects.push_back(PageChangeRect{terrain});
        return;
    }
    surface.Table.Reset(surface.Shape.SamplesX, surface.Shape.SamplesZ, 1u, m_Slots, m_TableBudget);
    surface.Packed.Update(surface.Table, m_Geometry, ++m_TableVersion);
    m_ChangeRects.push_back(PageChangeRect{terrain});
}

void TerrainSurfacePages::Configure(PagedSurface& surface, const SurfacePagedRequest& request)
{
    const TerrainSurfacePageSource& source = *request.Source;
    if (surface.Stream == 0)
        surface.Stream = m_NextStream++;
    surface.Planet = false;
    surface.Label = request.Name.empty() ? std::format("terrain {}", request.Terrain) : std::string(request.Name);
    surface.Planes.clear();
    for (const std::shared_ptr<const PageStreaming::PageStoreAccess>& plane : source.Planes)
        surface.Planes.push_back(plane.get());
    surface.PlaneMask = source.PlaneMask;
    surface.ShownPlaneMask = source.PlaneMask;
    surface.Shape = request.Shape;
    surface.Shape.CacheSlots = m_Slots;
    surface.Requester.Configure(surface.Shape, PageRequestArea::Wedge);
    ResetTable(request.Terrain, surface);
    m_Residency.Configure(surface.Stream, kSurfacePageField,
                          PageStreaming::PageSource{nullptr, nullptr, source.Planes});
    Logger::Log::Info("Terrain pages: {} (terrain {} generation {}) streams its surface pages ({} planes, {} x {} "
                      "samples, {} levels)",
                      surface.Label, request.Terrain, request.TerrainGeneration, source.Planes.size(),
                      surface.Shape.SamplesX, surface.Shape.SamplesZ, surface.Table.Levels().size());
}

void TerrainSurfacePages::ChangePlanes(PagedSurface& surface, const SurfacePagedRequest& request)
{
    const TerrainSurfacePageSource& source = *request.Source;
    surface.Planes.clear();
    for (const std::shared_ptr<const PageStreaming::PageStoreAccess>& plane : source.Planes)
        surface.Planes.push_back(plane.get());
    surface.PlaneMask = source.PlaneMask;
    // A removed plane goes at once; an added plane shows when every resident page holds it.
    surface.ShownPlaneMask &= source.PlaneMask;
    surface.Packed.Update(surface.Table, m_Geometry, ++m_TableVersion);
    m_ChangeRects.push_back(PageChangeRect{request.Terrain});
    m_Residency.ChangePlanes(surface.Stream, PageStreaming::PageSource{nullptr, nullptr, source.Planes});
    Logger::Log::Info("Terrain pages: {} (terrain {} generation {}) streams {} surface planes and keeps its "
                      "resident pages",
                      surface.Label, request.Terrain, request.TerrainGeneration, source.Planes.size());
}

void TerrainSurfacePages::ConfigurePlanet(uint32 terrain, PagedSurface& surface,
                                          const PlanetSurfacePagedRequest& request, const PlanetSurfaceHeights& heights)
{
    if (surface.Stream == 0)
        surface.Stream = m_NextStream++;
    surface.Planet = true;
    surface.PlanetEntity = request.Planet;
    surface.Label = request.Name.empty() ? std::format("planet {}", request.Planet) : std::string(request.Name);
    surface.Planes.clear();
    surface.PlaneMask = kPlanetSurfacePlaneMask;
    surface.ShownPlaneMask = kPlanetSurfacePlaneMask;
    surface.Identity = heights.Identity;
    surface.LayerVersion = heights.LayerVersion;
    surface.Layers = heights.Layers;
    surface.RulesIdentity = request.RulesIdentity;
    surface.PlanetShape = heights.Shape;
    surface.PlanetShape.CacheSlots = m_Slots;
    surface.PlanetRequester.Configure(surface.PlanetShape, PageRequestArea::Wedge);
    ResetTable(terrain, surface);
    m_Residency.Configure(surface.Stream, kSurfacePageField, PlanetSurfaceSource(request, heights));
    Logger::Log::Info("Terrain pages: {} (terrain {}) generates its surface pages on six faces ({} samples a side, "
                      "{} levels)",
                      surface.Label, terrain, surface.PlanetShape.FaceSamples, surface.Table.Levels().size());
}

void TerrainSurfacePages::ChangePlanetInputs(PagedSurface& surface, const PlanetSurfacePagedRequest& request,
                                             const PlanetSurfaceHeights& heights, bool rulesChanged)
{
    // The provider holds a copy of the layers: a new one reads the new layers and rows, and the
    // resident pages they change rewrite, each keeping its old content until then.
    m_Residency.ChangeGeneratedPlanes(surface.Stream, PlanetSurfaceSource(request, heights));
    if (rulesChanged)
    {
        const PageStreaming::PageStoreLevel& level0 = surface.PlanetRequester.Levels().front();
        const PageStreaming::PageSampleRect face{0u, 0u, level0.SamplesX - 1u, level0.SamplesZ - 1u};
        for (uint8 index = 0; index < PageStreaming::kPageStoreMaxFaces; ++index)
            m_Residency.Refresh(surface.Stream, index, face);
    }
    else
    {
        const std::array<const PageStreaming::HeightPageLayer*, 2> held = LayerPointers(surface.Layers);
        const std::array<const PageStreaming::HeightPageLayer*, 2> requested = LayerPointers(heights.Layers);
        PageStreaming::RefreshLayeredPages(m_Residency, surface.Stream, held, surface.LayerVersion, requested,
                                           request.LayerChanged, request.LayerChangedSince,
                                           PageStreaming::kPageSurfaceWindowMarginSamples, m_FaceRects);
    }
    surface.LayerVersion = heights.LayerVersion;
    surface.Layers = heights.Layers;
    surface.RulesIdentity = request.RulesIdentity;
}

void TerrainSurfacePages::RequestPlanet(const PlanetSurfacePagedRequest& request, const PlanetSurfaceHeights& heights,
                                        std::span<const PageRequestCamera> cameras, const PageLevelView& view)
{
    if (m_FramesInFlight == 0 || request.Rules == nullptr || !heights.Shape.Heights)
        return; // the field's cache is configured by the first Update
    SizeField(std::max(m_Profile.Slots, m_Profile.PlanetSlots));
    const bool inserted = !m_Surfaces.Contains(heights.RenderTerrain);
    PagedSurface& surface = m_Surfaces.GetOrInsert(heights.RenderTerrain);
    if (inserted || !surface.Planet || surface.Identity != heights.Identity)
        ConfigurePlanet(heights.RenderTerrain, surface, request, heights);
    else if (surface.LayerVersion != heights.LayerVersion || surface.RulesIdentity != request.RulesIdentity)
        ChangePlanetInputs(surface, request, heights, surface.RulesIdentity != request.RulesIdentity);
    surface.Requested = true;
    surface.Fit = surface.PlanetRequester.BuildWithinSlots(cameras, view, m_Slots, m_Wants);
    m_Residency.Request(surface.Stream, m_Wants);
    surface.WantedPages = m_Wants;
}

void TerrainSurfacePages::Request(const SurfacePagedRequest& request, std::span<const PageRequestCamera> cameras,
                                  const PageLevelView& view)
{
    if (m_FramesInFlight == 0 || request.Source == nullptr)
        return; // the field's cache is configured by the first Update
    const bool inserted = !m_Surfaces.Contains(request.Terrain);
    PagedSurface& surface = m_Surfaces.GetOrInsert(request.Terrain);
    surface.Generation = request.TerrainGeneration;
    const std::vector<std::shared_ptr<const PageStreaming::PageStoreAccess>>& planes = request.Source->Planes;
    const bool samePlanes = std::ranges::equal(
        surface.Planes, planes, {}, {}, [](const std::shared_ptr<const PageStreaming::PageStoreAccess>& plane) {
            return plane.get();
        });
    const bool sameSamples =
        request.Shape.SamplesX == surface.Shape.SamplesX && request.Shape.SamplesZ == surface.Shape.SamplesZ;
    if (!inserted && sameSamples && !samePlanes && !surface.Planes.empty() && !planes.empty())
    {
        ChangePlanes(surface, request);
    }
    else if (inserted || !samePlanes || !sameSamples)
    {
        Configure(surface, request);
    }
    else if (!SamePagePlacement(request.Shape, surface.Shape))
    {
        // Moved or rescaled: the resident pages are keyed by terrain UV and stay valid.
        surface.Shape = request.Shape;
        surface.Shape.CacheSlots = m_Slots;
        surface.Requester.Configure(surface.Shape, PageRequestArea::Wedge);
    }
    surface.Requested = true;
    if (!surface.Requester.Build(cameras, view, m_Wants))
        return; // a parked camera: the last request stands
    m_Residency.Request(surface.Stream, m_Wants);
    surface.WantedPages = m_Wants;
}

std::span<const PageStreaming::PageWant> TerrainSurfacePages::WantedPages(uint32 terrain) const
{
    const PagedSurface* surface = m_Surfaces.Find(terrain);
    if (!surface)
        return {};
    return surface->WantedPages;
}

void TerrainSurfacePages::ConfigureField(uint32 framesInFlight)
{
    m_FramesInFlight = framesInFlight;
    m_Residency.ConfigureField(kSurfacePageField, m_Slots, framesInFlight, kAtlasUpgradeFadeSeconds);
    for (auto& [key, surface] : m_Surfaces)
        ResetTable(key, surface); // the stream stays configured; its slots are gone, so its pages load again
}

void TerrainSurfacePages::ForgetUnrequested()
{
    m_Uploads.clear();
    m_Forgotten.clear();
    for (auto& [key, surface] : m_Surfaces)
    {
        if (!surface.Requested)
            m_Forgotten.push_back(key);
        surface.Requested = false;
    }
    for (uint32 key : m_Forgotten)
    {
        PagedSurface& forgotten = *m_Surfaces.Find(key);
        Logger::Log::Info("Terrain pages: {} (terrain {} generation {}) stops streaming its surface pages",
                          forgotten.Label, key, forgotten.Generation);
        m_Residency.Forget(forgotten.Stream);
        m_Surfaces.Erase(key);
    }
    bool holdsPlanet = false;
    for (const auto& [key, surface] : m_Surfaces)
        holdsPlanet = holdsPlanet || surface.Planet;
    if (!holdsPlanet)
        SizeField(m_Profile.Slots);
}

void TerrainSurfacePages::SizeField(uint32 slots)
{
    if (slots == m_Slots)
        return;
    Logger::Log::Info("Terrain pages: the surface field holds {} slots ({} before)", slots, m_Slots);
    m_Slots = slots;
    m_Geometry = MakePageCacheGeometry(slots);
    for (auto& [key, surface] : m_Surfaces)
    {
        // The first pinned level follows the slots.
        if (surface.Planet)
        {
            surface.PlanetShape.CacheSlots = slots;
            surface.PlanetRequester.Configure(surface.PlanetShape, PageRequestArea::Wedge);
        }
        else
        {
            surface.Shape.CacheSlots = slots;
            surface.Requester.Configure(surface.Shape, PageRequestArea::Wedge);
        }
    }
    if (m_FramesInFlight != 0)
        ConfigureField(m_FramesInFlight);
}

void TerrainSurfacePages::CollectStreamChanges(uint32 terrain, PagedSurface& surface)
{
    const PageStreaming::PageCache& cache = m_Residency.FieldCache(kSurfacePageField);
    const uint32 writes = UpdatePageTable(cache, surface.Stream, surface.Table);
    // The fragment reads the table each frame: a late entry needs no dirty rect, only its change rect.
    m_LateEntries.clear();
    surface.Table.TakeLateEntries(m_LateEntries);
    const bool reveal = surface.ShownPlaneMask != surface.PlaneMask && !m_Residency.Refreshing(surface.Stream);
    if (reveal)
    {
        surface.ShownPlaneMask = surface.PlaneMask;
        m_ChangeRects.push_back(PageChangeRect{terrain});
    }
    if (writes != 0u || reveal)
        surface.Packed.Update(surface.Table, m_Geometry, ++m_TableVersion);
    // A planet's pages have no terrain UV: any change covers its whole terrain.
    const std::size_t firstChange = m_ChangeRects.size();
    AppendPageChangeRects(terrain, surface.Stream, surface.Table.Levels(), cache, m_LateEntries, m_ChangeRects);
    bool uploaded = false;
    for (const PageStreaming::PageUploadRequest& upload : cache.Uploads())
    {
        if (upload.Page.Stream != surface.Stream)
            continue;
        if (const std::vector<uint8>* bytes = m_Residency.Bytes(surface.Stream, upload.Page.Address))
        {
            m_Uploads.push_back(SurfacePageUpload{upload.Slot, surface.PlaneMask, *bytes});
            uploaded = true;
        }
    }
    if (surface.Planet && (m_ChangeRects.size() > firstChange || uploaded || writes != 0u))
    {
        m_ChangeRects.resize(firstChange);
        m_ChangeRects.push_back(PageChangeRect{terrain});
    }
}

void TerrainSurfacePages::Collect()
{
    for (auto& [key, surface] : m_Surfaces)
    {
        CollectStreamChanges(key, surface);
        LogPageResidencyChange(surface.Label, " surface", surface.Table, surface.WantedPages, surface.LoggedResidency);
    }
}

const PackedPageTable* TerrainSurfacePages::PackedTable(uint32 terrain, uint32& planeMask) const
{
    const PagedSurface* surface = m_Surfaces.Find(terrain);
    if (!surface)
        return nullptr;
    planeMask = surface->ShownPlaneMask;
    return &surface->Packed;
}

void TerrainSurfacePages::PagedTerrains(std::vector<uint32>& out) const
{
    out.clear();
    for (const auto& [key, surface] : m_Surfaces)
        out.push_back(key);
}

void TerrainSurfacePages::StreamCounts(std::vector<PageStreamCounts>& out) const
{
    const PageStreaming::PageCache& cache = m_Residency.FieldCache(kSurfacePageField);
    for (const auto& [key, surface] : m_Surfaces)
    {
        PageStreamCounts& counts = out.emplace_back();
        counts.Label = surface.Label;
        counts.Surface = true;
        counts.FitPasses = surface.Planet ? surface.Fit.Passes : 0u;
        counts.FullDetailPages = surface.Planet ? surface.Fit.FullDetailPages : 0u;
        counts.PagesProduced = m_Residency.PagesProduced(surface.Stream);
        CountWantedPages(cache, surface.Stream, surface.WantedPages,
                         static_cast<uint32>(surface.Table.Levels().size()), counts);
    }
}

PageSlotFit TerrainSurfacePages::PlanetFit(uint32 planet) const
{
    for (const auto& [key, surface] : m_Surfaces)
        if (surface.Planet && surface.PlanetEntity == planet)
            return surface.Fit;
    return {};
}

} // namespace GameEngine::TerrainECS
