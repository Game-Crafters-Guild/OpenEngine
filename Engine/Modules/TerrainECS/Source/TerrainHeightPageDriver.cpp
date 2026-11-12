#include "TerrainECS/TerrainHeightPageDriver.h"

#include "TerrainECS/SurfaceHeightQuery.h"
#include "TerrainECS/TerrainDefaultSurfaceRules.h"

#include "TerrainECS/TerrainHeightFunction/RevocableTerrainHeightFunction.h"
#include "TerrainECS/HeightPageStoreLoader.h"
#include "TerrainECS/TerrainPageFeature.h"
#include "TerrainECS/Planet/PlanetPageRequester.h"
#include "TerrainECS/TerrainSurface/TerrainSurfacePageSource.h"
#include "TerrainECS/TerrainSurface/TerrainSurfacePages.h"
#include "TerrainECS/TerrainPages.h"
#include "TerrainECS/TerrainHeightSource.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainService.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTPlanetShading.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageStore/PageStoreAccess.h"
#include "PageStreaming/PageStore/PageStoreReader.h"
#include "Rendering/Core/Device.h"
#include "Types/Fnv1a.h"

#include <bit>
#include <cmath>
#include <format>
#include <span>

namespace GameEngine::TerrainECS
{

using Hashing::HashCombine;

PageStreaming::GeneratedHeightLattice MakeGeneratedHeightLattice(const TiledTerrainData& tiled)
{
    PageStreaming::GeneratedHeightLattice lattice;
    switch (tiled.Config.Base.Source)
    {
    case Components::TerrainBaseSource::HeightmapAsset:
        lattice.Source = PageStreaming::GeneratedHeightSource::Heightmap;
        lattice.Heightmap = tiled.Config.Base.Heightmap;
        lattice.AuthoredSizeX = tiled.Config.WorldSizeX;
        lattice.AuthoredSizeZ = tiled.Config.WorldSizeZ;
        break;
    case Components::TerrainBaseSource::Function:
        if (const std::shared_ptr<const RevocableTerrainHeightFunction> function = tiled.Config.Base.HeightFunction)
        {
            const float32 band = FunctionBaseHeightBand(tiled.Config);
            lattice.Source = PageStreaming::GeneratedHeightSource::Function;
            // Any result but Filled leaves the page absent.
            lattice.FunctionRow = [function, names = tiled.Config.Base.Names, band](
                                      float32 localZ, std::span<const float32> localX, float32 minResolvedWavelength,
                                      std::span<float32> out) {
                return FillFunctionBaseRow(*function, names, localZ, localX, minResolvedWavelength, band, out) ==
                       FunctionBaseRowFill::Filled;
            };
            const TerrainHeightFunctionKey& key = tiled.Config.Base.HeightFunctionKey;
            lattice.FunctionIdentity = HashCombine(HashCombine(reinterpret_cast<uintptr_t>(function.get()), key.Generation),
                                                   HashCombine(key.Context.Seed, std::bit_cast<uint32>(band)));
            break;
        }
        lattice.Source = PageStreaming::GeneratedHeightSource::Flat;
        break;
    default:
        lattice.Source = tiled.Config.Base.Noise ? PageStreaming::GeneratedHeightSource::Noise
                                                 : PageStreaming::GeneratedHeightSource::Flat;
        break;
    }
    lattice.TerrainOriginX = tiled.WorldOriginX;
    lattice.TerrainOriginZ = tiled.WorldOriginZ;
    lattice.TileWorldSize = tiled.Config.TileWorldSize;
    lattice.TileSamples = tiled.Config.TileConfig.HeightmapWidth;
    lattice.TilesX = tiled.Config.TilesPerAxisX;
    lattice.TilesZ = tiled.Config.TilesPerAxisZ;
    if (tiled.Config.Base.Noise)
        lattice.Noise = *tiled.Config.Base.Noise;
    return lattice;
}

uint64 GeneratedHeightIdentity(const PageStreaming::GeneratedHeightLattice& lattice)
{
    uint64 hash = HashCombine(0x9E3779B97F4A7C15ull, static_cast<uint64>(lattice.Source));
    hash = HashCombine(hash, std::bit_cast<uint32>(lattice.TerrainOriginX));
    hash = HashCombine(hash, std::bit_cast<uint32>(lattice.TerrainOriginZ));
    hash = HashCombine(hash, std::bit_cast<uint32>(lattice.TileWorldSize));
    hash = HashCombine(hash, lattice.TileSamples);
    hash = HashCombine(hash, lattice.TilesX);
    hash = HashCombine(hash, lattice.TilesZ);
    hash = HashCombine(hash, reinterpret_cast<uintptr_t>(lattice.Heightmap.get()));
    hash = HashCombine(hash, std::bit_cast<uint32>(lattice.AuthoredSizeX));
    hash = HashCombine(hash, std::bit_cast<uint32>(lattice.AuthoredSizeZ));
    hash = HashCombine(hash, std::bit_cast<uint32>(lattice.Noise.TopFrequency));
    hash = HashCombine(hash, lattice.Noise.Octaves);
    hash = HashCombine(hash, std::bit_cast<uint32>(lattice.Noise.Gain));
    hash = HashCombine(hash, lattice.Noise.TopSeed);
    hash = HashCombine(hash, std::bit_cast<uint32>(lattice.Noise.Amplitude));
    return HashCombine(hash, lattice.FunctionIdentity);
}

namespace
{

uint64 PlanetIdentity(float64 radius, uint32 faceSamples, const Components::TerrainPlanetRelief& relief)
{
    uint64 hash = HashCombine(0x2545F4914F6CDD1Dull, std::bit_cast<uint64>(radius));
    hash = HashCombine(hash, faceSamples);
    hash = HashCombine(hash, std::bit_cast<uint32>(relief.Amplitude));
    hash = HashCombine(hash, std::bit_cast<uint32>(relief.Frequency));
    return HashCombine(hash, relief.Octaves);
}


// The surface pages of the terrain of `height` over the wedges of `views`, one texel a pixel
// (kSurfacePageTargetPixels). The surface stores lie on the tile lattice, which a cooked height store
// on its own grid does not.
void RequestSurfacePages(TerrainSurfacePages& pages, const PagedTerrainRequest& height,
                         const TerrainSurfacePageSource& source, const TiledRenderExtent& extent,
                         std::span<const PageRequestCamera> views, float32 focalScale)
{
    const PageStreaming::PageStoreLevel& level0 = source.Planes.front()->Levels().front();
    SurfacePagedRequest request;
    request.Terrain = height.Terrain;
    request.TerrainGeneration = height.TerrainGeneration;
    request.Name = height.Name;
    request.Source = &source;
    request.Shape = height.Shape;
    request.Shape.SamplesX = level0.SamplesX;
    request.Shape.SamplesZ = level0.SamplesZ;
    request.Shape.Level0TexelX = extent.WorldSizeX / static_cast<float32>(level0.SamplesX - 1u);
    request.Shape.Level0TexelZ = extent.WorldSizeZ / static_cast<float32>(level0.SamplesZ - 1u);
    const PageLevelView view =
        MakePageLevelView(kPageLevelReferenceRows, 2.0f * std::atan(1.0f / focalScale), kSurfacePageTargetPixels);
    pages.Request(request, views, view);
}
// The surface pages of planet `height` over the view wedges of `views`, generated from the default rows
// of its relief, one texel a pixel (kSurfacePageTargetPixels).
void RequestPlanetSurfacePages(TerrainPages& pages, const PlanetPagedRequest& height,
                               const Components::TerrainPlanetRelief& relief, std::span<const PageRequestCamera> views,
                               float32 fieldOfView)
{
    const TerrainHeightRangeMeters range = PlanetHeightRangeMeters(relief);
    const Components::TerrainSurfaceRulesEffect rules = MakeDefaultPlanetSurfaceRules(range.Min, range.Max);
    PlanetSurfacePagedRequest request;
    request.Planet = height.Planet;
    request.Name = height.Name;
    request.Rules = &rules;
    request.RulesIdentity = HashCombine(std::bit_cast<uint32>(range.Min), std::bit_cast<uint32>(range.Max));
    request.LayerChanged = height.OverlayChanged;
    request.LayerChangedSince = height.OverlayChangedSince;
    pages.RequestPlanetSurface(request, views,
                               MakePageLevelView(kPageLevelReferenceRows, fieldOfView, kSurfacePageTargetPixels));
}
} // namespace

void RequestPlanetHeightPages(TerrainService& service, const Rendering::IDevice& device, uint32 planet,
                              const Mathematics::Vector3& center, uint32 renderTerrain, std::string_view name,
                              const Components::Terrain& terrain,
                              const Components::TerrainPlanetRelief& relief,
                              std::span<const Mathematics::Vector3> cameras,
                              std::span<const PageRequestCamera> views, float32 focalScale)
{
    if (focalScale <= 0.0f || cameras.empty())
        return;
    TerrainPages& pages = service.GetTerrainPages();
    const float64 radius = terrain.PlanetRadius;
    const float32 texel = terrain.SamplesPerMeter > 0.0f ? 1.0f / terrain.SamplesPerMeter : 0.0f;
    const uint32 faceSamples = PlanetFaceSamples(radius, texel);
    if (faceSamples == 0)
    {
        pages.ReportRefusal(planet, std::format("{} does not stream height pages: a planet of radius {} m at {} "
                                                "samples a meter passes the page store's samples a side; lower its "
                                                "Samples Per Meter",
                                                name.empty() ? std::string_view("A planet") : name, radius,
                                                terrain.SamplesPerMeter));
        return;
    }

    PlanetPagedRequest request;
    request.Planet = planet;
    request.Center = center;
    request.Name = name;
    request.SourceIdentity = PlanetIdentity(radius, faceSamples, relief);
    request.Shape.ReferenceRadius = radius;
    request.Shape.FaceSamples = faceSamples;
    // The narrow arm (the web) pages a planet as the desktop does, a level coarser where it would pass its
    // slots, and draws the closed-form relief: it reads no pages.
    const bool narrow = CBTTerrain::CBTDeviceRunsNarrowArm(
        device.PreferredShaderSource() == Rendering::ShaderSourceKind::Wgsl, device.GetCapabilities().supportsShaderInt64);
    request.FitToSlots = narrow;
    request.GpuPages = !narrow;
    request.RenderTerrain = renderTerrain;
    if (pages.NeedsPlanetSource(planet, request.SourceIdentity))
    {
        PageStreaming::GeneratedPlanetLattice lattice;
        lattice.ReferenceRadius = radius;
        lattice.FaceSamples = faceSamples;
        lattice.HeightRow = [relief](std::span<const Mathematics::Vector3> directions, float32 minResolvedArc,
                                     std::span<float32> outHeights) {
            CBTTerrain::PlanetReliefRow(directions, relief.Amplitude, relief.Frequency, relief.Octaves,
                                        minResolvedArc, outHeights);
        };
        lattice.HeightRange = [relief](const Mathematics::Vector3& direction, float32 chord, float32 minResolvedArc) {
            const CBTTerrain::PlanetReliefRange range =
                CBTTerrain::PlanetReliefRangeNear(direction.x, direction.y, direction.z, chord, relief.Amplitude,
                                                  relief.Frequency, relief.Octaves, minResolvedArc);
            return PageStreaming::PageHeightRange{range.Min, range.Max};
        };
        request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(std::move(lattice));
    }
    if (const TerrainService::PlanetPageOverlay* overlay = service.FindPlanetPageOverlay(planet))
    {
        request.Overlay = overlay->Layer;
        request.Modifiers = overlay->Modifiers;
        request.OverlayVersion = overlay->Version;
        request.OverlayChanged = overlay->Changed;
        request.OverlayChangedSince = overlay->ChangedSince;
    }
    const float32 fieldOfView = 2.0f * std::atan(1.0f / focalScale);
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, fieldOfView, terrain.TargetPixelError);
    pages.RequestPlanet(request, cameras, view);
    if (request.GpuPages)
        RequestPlanetSurfacePages(pages, request, relief, views, fieldOfView);
}

void RequestTiledTerrainPages(TerrainService& service, const Rendering::IDevice& device, TiledTerrainData& tiled,
                              const TiledRenderExtent& extent, const TerrainHandle& renderTerrain,
                              std::string_view name, float32 originY, float32 heightScale, float32 targetPixelError,
                              std::span<const Mathematics::Vector3> cameras, std::span<const PageRequestCamera> views,
                              float32 focalScale)
{
    const bool gpuReadsPages = !CBTTerrain::CBTDeviceRunsNarrowArm(
        device.PreferredShaderSource() == Rendering::ShaderSourceKind::Wgsl,
        device.GetCapabilities().supportsShaderInt64);
    TerrainPages& pages = service.GetTerrainPages();
    const TiledTerrainBase& base = tiled.Config.Base;
    const bool generated = base.Source != Components::TerrainBaseSource::HeightmapAsset;

    PagedTerrainRequest request;
    request.Terrain = renderTerrain.Index;
    request.TerrainGeneration = renderTerrain.Generation;
    request.Name = name;
    uint32 samplesX = 0;
    uint32 samplesZ = 0;
    std::shared_ptr<const PageStreaming::PageStoreReader> store;
    PageStreaming::GeneratedHeightLattice lattice;
    bool decoded = false;
    if (generated)
    {
        lattice = MakeGeneratedHeightLattice(tiled);
        request.SourceIdentity = GeneratedHeightIdentity(lattice);
        samplesX = lattice.TilesX * (lattice.TileSamples - 1u) + 1u;
        samplesZ = lattice.TilesZ * (lattice.TileSamples - 1u) + 1u;
    }
    else
    {
        store = service.GetHeightPageStores().Store(base.HeightmapGuid, base.HeightmapContentVersion, name);
        if (store)
        {
            request.SourceIdentity = HashCombine(reinterpret_cast<uintptr_t>(store.get()), base.HeightmapContentVersion);
            samplesX = store->Layout().Header.SamplesX;
            samplesZ = store->Layout().Header.SamplesZ;
        }
        else if (base.Heightmap && !base.Heightmap->IsEmpty() &&
                 service.GetHeightPageStores().NeverOpensStore(base.HeightmapGuid))
        {
            // No store will open: its CPU pages read the decoded heightmap on the tile lattice, as its
            // tiles do. The provider holds the decoded field, so its address names it while it streams.
            lattice = MakeGeneratedHeightLattice(tiled);
            request.SourceIdentity = GeneratedHeightIdentity(lattice);
            samplesX = lattice.TilesX * (lattice.TileSamples - 1u) + 1u;
            samplesZ = lattice.TilesZ * (lattice.TileSamples - 1u) + 1u;
            decoded = true;
        }
    }

    TerrainHeightSourceInputs inputs;
    inputs.ModifiersPending = service.HeightModifiersMayApply() && !tiled.PageOverlayCurrent;
    inputs.GeneratedBase = generated;
    inputs.CookedStoreOpen = store != nullptr;
    inputs.DecodedHeightmapBase = decoded;
    inputs.GpuReadsPages = gpuReadsPages;
    // The terrain UV the CBT samples spans the tile grid's extent; a generated or decoded pyramid
    // that does not cover exactly that lattice would place every page wrong, so it pages only on the
    // lattice. A cooked store on its own grid pages too, but takes no modifier overlay (PageOverlayAllowance).
    const bool onLattice = samplesX == extent.UnifiedWidth && samplesZ == extent.UnifiedHeight;
    const std::string_view label = name.empty() ? std::string_view("A terrain") : name;
    const bool fitsLattice = samplesX > 1u && samplesZ > 1u && ((!generated && !decoded) || onLattice);
    std::string refusal;
    inputs.PagesOnLattice = fitsLattice;
    // The modifier system builds the overlay for every terrain with CPU pages: the CPU readers take
    // it on every GPU arm, and the GPU pages take it where they are drawn.
    const bool cpuPages = TerrainHeightHasCpuPages(inputs);
    const HeightPageOverlayAllowance allowance = PageOverlayAllowance(
        cpuPages, samplesX, samplesZ, extent.UnifiedWidth, extent.UnifiedHeight, pages.Profile());
    tiled.PageOverlayByteCap = allowance.ByteCap;
    tiled.PageOverlayOffGrid = allowance.OffGrid;
    if (!allowance.OffGrid)
        tiled.PageOverlayRefusedOffGrid = false; // its store is on the lattice now: the overlay is baked again
    if (allowance.ByteCap != 0 && tiled.PageOverlayRefusedBytes != 0)
        refusal = OverlayRefusal(label, tiled.PageOverlayRefusedBytes, pages.Profile());
    else if (allowance.ByteCap != 0 && tiled.PageOverlayRefusedOffGrid)
        refusal = OverlayGridRefusal(label, samplesX, samplesZ, extent.UnifiedWidth, extent.UnifiedHeight);
    // The GPU draws the pages only with a view to choose their levels for.
    request.GpuPages = TerrainHeightIsPaged(inputs) && focalScale > 0.0f;
    if (!refusal.empty() && (gpuReadsPages || cpuPages))
        pages.ReportRefusal(request.Terrain, refusal);
    if (!cpuPages)
        return; // not requested this frame: its pages are forgotten at the next update

    if (pages.NeedsSource(request.Terrain, request.SourceIdentity))
    {
        if (generated || decoded)
            request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
        else
            request.Source.Store = store;
    }
    request.Overlay = tiled.PageOverlay;
    request.OverlayVersion = tiled.PageOverlayVersion;
    request.OverlayChanged = tiled.PageOverlayChanged;
    request.OverlayChangedSince = tiled.PageOverlayChangedSince;
    request.Shape.OriginX = tiled.WorldOriginX;
    request.Shape.OriginZ = tiled.WorldOriginZ;
    request.Shape.Level0TexelX = extent.WorldSizeX / static_cast<float32>(samplesX - 1u);
    request.Shape.Level0TexelZ = extent.WorldSizeZ / static_cast<float32>(samplesZ - 1u);
    request.Shape.SamplesX = samplesX;
    request.Shape.SamplesZ = samplesZ;
    const float32 low = tiled.CachedGlobalMinH * heightScale + originY;
    const float32 high = tiled.CachedGlobalMaxH * heightScale + originY;
    request.Shape.MinHeight = std::min(low, high);
    request.Shape.MaxHeight = std::max(low, high);
    request.OriginY = originY;
    request.HeightScale = heightScale;
    const PageLevelView view = request.GpuPages ? MakePageLevelView(kPageLevelReferenceRows,
                                                                    2.0f * std::atan(1.0f / focalScale),
                                                                    targetPixelError)
                                                : PageLevelView{};
    pages.Request(request, cameras, view);
    if (TerrainSurfaceIsPaged(inputs, tiled.SurfacePages) && focalScale > 0.0f && !views.empty())
        RequestSurfacePages(pages.Surface(), request, tiled.SurfacePages, extent, views, focalScale);
}

void UpdateHeightPages(TerrainService& service, TerrainRenderFeature& terrainFeature, TerrainPageFeature& pages,
                       uint64 frameIndex, float32 deltaSeconds, uint32 framesInFlight)
{
    TerrainPages& heightPages = service.GetTerrainPages();
    heightPages.Update(frameIndex, deltaSeconds, false, framesInFlight);
    heightPages.RestartOnDeviceEpoch(pages.DeviceEpoch());
    pages.Publish(heightPages);
    for (const HeightPageDirtyRect& rect : heightPages.DirtyRects())
        terrainFeature.AccumulateAtlasHeightDirtyUV(TerrainHandle{rect.Terrain, rect.TerrainGeneration}, rect.MinU,
                                                    rect.MinV, rect.MaxU, rect.MaxV);
    heightPages.DirtyRects().clear();
    if (heightPages.PlanetDirtyRegions().Count != 0u)
        service.AccumulatePlanetPageDirtyRegions(heightPages.PlanetDirtyRegions());
    heightPages.PlanetDirtyRegions() = {};
}

} // namespace GameEngine::TerrainECS
