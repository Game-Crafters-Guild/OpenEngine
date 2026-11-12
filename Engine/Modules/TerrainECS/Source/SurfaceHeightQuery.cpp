#include "TerrainECS/SurfaceHeightQuery.h"

#include "TerrainECS/PagedHeightSampler.h"
#include "TerrainECS/Planet/PlanetChart.h"
#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainPages.h"
#include "TerrainECS/TerrainService.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Mathematics/Interpolation.h"

#include "CBTTerrain/CBTPlanetShading.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace GameEngine::TerrainECS
{

namespace
{

// The overlay's delta at `texel` of a page of `level`: the same four taps and weights as
// SampleGridBilinearTexel over the page's stride grid, read from the layer's page there. The
// bilinear sum is linear, so base plus this equals the bilinear sample of the uploaded page (at a
// lattice point bit for bit: the weights are zero there).
float32 OverlayDelta(const PageStreaming::HeightPageLayer& overlay, uint32 level, const PageTexel& texel)
{
    const std::span<const float32> deltas =
        overlay.PageDeltas(PageStreaming::PageAddress{0, static_cast<uint8>(level), texel.PageX, texel.PageZ});
    if (deltas.empty())
        return 0.0f;
    constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
    const float32 tx = std::clamp(texel.StrideX, 0.0f, static_cast<float32>(kStride - 1u));
    const float32 tz = std::clamp(texel.StrideZ, 0.0f, static_cast<float32>(kStride - 1u));
    const uint32 x0 = static_cast<uint32>(std::floor(tx));
    const uint32 z0 = static_cast<uint32>(std::floor(tz));
    const uint32 x1 = std::min(x0 + 1u, kStride - 1u);
    const uint32 z1 = std::min(z0 + 1u, kStride - 1u);
    const float32 fx = tx - static_cast<float32>(x0);
    const float32 fz = tz - static_cast<float32>(z0);
    const auto tap = [&deltas](uint32 x, uint32 z) { return deltas[static_cast<std::size_t>(z) * kStride + x]; };
    return Math::Lerp(Math::Lerp(tap(x0, z0), tap(x1, z0), fx), Math::Lerp(tap(x0, z1), tap(x1, z1), fx), fz);
}

} // namespace

SurfaceHeightQuery::SurfaceHeightQuery(const PageStreaming::CpuPageDirectory& pages,
                                       std::span<const PageStreaming::PageStoreLevel> levels,
                                       const PageRequestTerrain& shape, float32 originY, float32 heightScale,
                                       const PageStreaming::HeightPageLayer* overlay, const SurfaceHeightEdits& edits)
    : m_Pages(pages)
    , m_Overlay(overlay)
    , m_Levels(levels)
    , m_Shape(shape)
    , m_OriginY(originY)
    , m_HeightScale(heightScale)
    , m_Edits(edits)
{
    m_FootprintSizeX = LatticeSizeX();
    m_FootprintSizeZ = LatticeSizeZ();
}

SurfaceHeightQuery::SurfaceHeightQuery(const PageRequestTerrain& shape, float32 originY, float32 heightScale)
    : m_Shape(shape)
    , m_OriginY(originY)
    , m_HeightScale(heightScale)
{
    m_FootprintSizeX = LatticeSizeX();
    m_FootprintSizeZ = LatticeSizeZ();
}

SurfaceHeightQuery::SurfaceHeightQuery(const TerrainData& wholeMap, const TerrainHandle& handle,
                                       const PageRequestTerrain& shape, float32 originY, float32 heightScale)
    : m_WholeMap(&wholeMap)
    , m_WholeMapIdentity((static_cast<uint64>(handle.Generation) << 32u) | handle.Index)
    , m_Shape(shape)
    , m_OriginY(originY)
    , m_HeightScale(heightScale)
{
    m_FootprintSizeX = LatticeSizeX();
    m_FootprintSizeZ = LatticeSizeZ();
}

SurfaceHeightQuery::SurfaceHeightQuery(const PageStreaming::CpuPageDirectory& pages,
                                       std::span<const PageStreaming::PageStoreLevel> levels,
                                       const SurfaceHeightSphere& sphere)
    : m_Pages(pages)
    , m_Sphere(sphere)
    , m_Levels(levels)
{
}

float32 SurfaceHeightQuery::LatticeSizeX() const
{
    return m_Shape.SamplesX < 2u ? 0.0f : static_cast<float32>(m_Shape.SamplesX - 1u) * m_Shape.Level0TexelX;
}

float32 SurfaceHeightQuery::LatticeSizeZ() const
{
    return m_Shape.SamplesZ < 2u ? 0.0f : static_cast<float32>(m_Shape.SamplesZ - 1u) * m_Shape.Level0TexelZ;
}

void SurfaceHeightQuery::ClipFootprint(float32 sizeX, float32 sizeZ)
{
    m_FootprintSizeX = std::min(sizeX, LatticeSizeX());
    m_FootprintSizeZ = std::min(sizeZ, LatticeSizeZ());
}

bool SurfaceHeightQuery::ContainsXZ(float32 worldX, float32 worldZ) const
{
    if (!IsValid())
        return false;
    const float32 u = (worldX - m_Shape.OriginX) / m_FootprintSizeX;
    const float32 v = (worldZ - m_Shape.OriginZ) / m_FootprintSizeZ;
    return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f; // false for a degenerate placement too
}

bool SurfaceHeightQuery::SampleHeight(const Mathematics::Vector3& worldPoint, SurfaceHeightPolicy policy,
                                      SurfaceHeightSample& out) const
{
    if (IsSpherical())
        return SampleSphere(worldPoint, policy, out);
    if (!ContainsXZ(worldPoint.x, worldPoint.z))
        return false;
    const float32 u = (worldPoint.x - m_Shape.OriginX) / LatticeSizeX();
    const float32 v = (worldPoint.z - m_Shape.OriginZ) / LatticeSizeZ();
    if (m_WholeMap)
    {
        out = SurfaceHeightSample{m_OriginY + m_WholeMap->Heightfield.SampleBilinear(u, v) * m_HeightScale, 0,
                                  SurfaceHeightDetail::Finest};
        return true;
    }
    return SamplePaged(u, v, policy, out);
}

bool SurfaceHeightQuery::SamplePaged(float32 u, float32 v, SurfaceHeightPolicy policy, SurfaceHeightSample& out) const
{
    if (m_Pages.Empty() || m_Levels.empty())
        return false; // no page source yet: unsampled under every policy
    // Down the pyramid from level 0: the first resident page answers. A page absent from the
    // source leaves its parent the finest data there; a page the CPU has not fetched yet makes
    // every coarser answer Coarse.
    bool finerMissing = false;
    for (uint32 level = 0; level < m_Levels.size(); ++level)
    {
        const PageTexel texel = LocatePageTexel(m_Levels, level, u, v);
        const PageStreaming::CpuPageEntry entry =
            m_Pages.Page(PageStreaming::PageAddress{0, static_cast<uint8>(level), texel.PageX, texel.PageZ});
        if (entry == PageStreaming::CpuPageEntry::Missing)
        {
            finerMissing = true;
            continue;
        }
        if (entry == PageStreaming::CpuPageEntry::Absent)
            continue;
        const SurfaceHeightDetail detail = finerMissing ? SurfaceHeightDetail::Coarse : SurfaceHeightDetail::Finest;
        if (policy == SurfaceHeightPolicy::RequireFinest && detail == SurfaceHeightDetail::Coarse)
            return false;
        constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
        const std::span<const float32> samples = PageStreaming::HeightSamples(m_Pages.BytesOf(entry));
        float32 normalized = SampleGridBilinearTexel(samples.data(), kStride, kStride, texel.StrideX, texel.StrideZ);
        if (m_Overlay)
            normalized += OverlayDelta(*m_Overlay, level, texel);
        out = SurfaceHeightSample{m_OriginY + normalized * m_HeightScale, level, detail};
        return true;
    }
    return false; // not even the top page is on the CPU yet
}

bool SurfaceHeightQuery::SampleSphere(const Mathematics::Vector3& worldPoint, SurfaceHeightPolicy policy,
                                      SurfaceHeightSample& out) const
{
    const Mathematics::Vector3 direction = worldPoint - m_Sphere.Center;
    if (m_Pages.Empty() || m_Levels.empty() || !(direction.LengthSquared() > 0.0f))
        return false;
    const PageStreaming::PageFacePoint point = PageStreaming::EqualAngleFacePointOf(direction, m_Sphere.FaceSamples);
    // The walk of SamplePaged, on the face of the direction.
    bool finerMissing = false;
    for (uint32 level = 0; level < m_Levels.size(); ++level)
    {
        const PlanetPageTexel texel = LocatePlanetPageTexel(m_Sphere.FaceSamples, point, level);
        const PageStreaming::CpuPageEntry entry = m_Pages.Page(texel.Address);
        if (entry == PageStreaming::CpuPageEntry::Missing)
        {
            finerMissing = true;
            continue;
        }
        if (entry == PageStreaming::CpuPageEntry::Absent)
            continue;
        const SurfaceHeightDetail detail = finerMissing ? SurfaceHeightDetail::Coarse : SurfaceHeightDetail::Finest;
        if (policy == SurfaceHeightPolicy::RequireFinest && detail == SurfaceHeightDetail::Coarse)
            return false;
        float64 height = SamplePlanetPageTexel(PageStreaming::HeightSamples(m_Pages.BytesOf(entry)), texel);
        // The layers are linear: their level-0 blend at the point is what ApplyToPage adds to the drawn page.
        for (const PageStreaming::HeightPageLayer* layer : {m_Sphere.Modifiers, m_Sphere.Overlay})
            if (layer)
                height += layer->SampleLevel0(point.Face, point.X, point.Z);
        out = SurfaceHeightSample{static_cast<float32>(height), level, detail};
        return true;
    }
    return false; // not even the top page is on the CPU yet
}

bool SurfaceHeightQuery::FillFinestPage(uint32 pageX, uint32 pageZ, std::span<float32> out) const
{
    assert(out.size() == static_cast<std::size_t>(kFinestPageGridSamples) * kFinestPageGridSamples);
    if (!HasPageSource() || out.size() != static_cast<std::size_t>(kFinestPageGridSamples) * kFinestPageGridSamples)
        return false;
    const PageStreaming::CpuPageEntry entry = m_Pages.Page(PageStreaming::PageAddress{0, 0, pageX, pageZ});
    if (PageStreaming::State(entry) != PageStreaming::CpuPageState::Resident)
        return false;
    const std::span<const float32> samples = PageStreaming::HeightSamples(m_Pages.BytesOf(entry));
    // Grid sample i of the page is its stride sample i + kPageApronSamples: the first owned sample on.
    constexpr uint32 kApron = PageStreaming::kPageApronSamples;
    constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
    const std::span<const float32> deltas =
        m_Overlay ? m_Overlay->PageDeltas(PageStreaming::PageAddress{0, 0, pageX, pageZ}) : std::span<const float32>();
    for (uint32 z = 0; z < kFinestPageGridSamples; ++z)
        for (uint32 x = 0; x < kFinestPageGridSamples; ++x)
        {
            const std::size_t stored = static_cast<std::size_t>(z + kApron) * kStride + x + kApron;
            float32 normalized = samples[stored];
            if (!deltas.empty())
                normalized += deltas[stored];
            out[static_cast<std::size_t>(z) * kFinestPageGridSamples + x] = normalized;
        }
    return true;
}

bool SurfaceHeightQuery::FinestPageAt(float32 worldX, float32 worldZ, uint32& outPageX, uint32& outPageZ) const
{
    if (m_Pages.Empty() || m_Levels.empty() || !ContainsXZ(worldX, worldZ))
        return false;
    const PageTexel texel = LocatePageTexel(m_Levels, 0, (worldX - m_Shape.OriginX) / LatticeSizeX(),
                                            (worldZ - m_Shape.OriginZ) / LatticeSizeZ());
    outPageX = texel.PageX;
    outPageZ = texel.PageZ;
    return true;
}

bool SurfaceHeightQuery::CollectPageChanges(const SurfaceHeightPageStamp& since,
                                            std::vector<Mathematics::Rect>& out) const
{
    if (m_WholeMap)
        return true;
    const uint64 end = m_Pages.ChangesEnd();
    if (since.Epoch != m_Pages.Epoch || since.Change < m_Pages.ChangesBegin() || since.Change > end)
        return false;
    for (uint64 change = since.Change; change < end; ++change)
    {
        const PageStreaming::PageAddress& page = m_Pages.Changes[change % m_Pages.Changes.size()];
        if (page.Face != 0 || page.Level >= m_Levels.size())
            continue; // a planar terrain reads face 0 only
        // Page p of a level owns its lattice coordinates [p, p + 1) x kPageOwnedSamples (the last page
        // to the edge, PageAndStride), level-L coordinate c at level-0 sample c x 2^L. One level texel
        // more on every side covers the bilinear taps at the page's edges.
        const float32 levelScale = static_cast<float32>(1u << page.Level);
        const float32 texelX = m_Shape.Level0TexelX * levelScale;
        const float32 texelZ = m_Shape.Level0TexelZ * levelScale;
        const PageStreaming::PageStoreLevel& level = m_Levels[page.Level];
        constexpr float32 kOwned = static_cast<float32>(PageStreaming::kPageOwnedSamples);
        const float32 minX = m_Shape.OriginX + (static_cast<float32>(page.X) * kOwned - 1.0f) * texelX;
        const float32 minZ = m_Shape.OriginZ + (static_cast<float32>(page.Z) * kOwned - 1.0f) * texelZ;
        const float32 maxX = page.X + 1u >= level.PagesX
                                 ? m_Shape.OriginX + LatticeSizeX() + texelX
                                 : m_Shape.OriginX + (static_cast<float32>(page.X + 1u) * kOwned + 1.0f) * texelX;
        const float32 maxZ = page.Z + 1u >= level.PagesZ
                                 ? m_Shape.OriginZ + LatticeSizeZ() + texelZ
                                 : m_Shape.OriginZ + (static_cast<float32>(page.Z + 1u) * kOwned + 1.0f) * texelZ;
        out.push_back(Mathematics::Rect::FromEdges(minX, minZ, maxX, maxZ));
    }
    return true;
}

bool SurfaceHeightQuery::FinestPageChangedSince(const SurfaceHeightPageStamp& since, uint32 pageX, uint32 pageZ) const
{
    if (m_WholeMap)
        return false;
    const uint64 end = m_Pages.ChangesEnd();
    if (since.Epoch != m_Pages.Epoch || since.Change < m_Pages.ChangesBegin() || since.Change > end)
        return true;
    const PageStreaming::PageAddress page{0, 0, pageX, pageZ};
    for (uint64 change = since.Change; change < end; ++change)
        if (m_Pages.Changes[change % m_Pages.Changes.size()] == page)
            return true;
    return false;
}

void SurfaceHeightQuery::HeightRange(float32& outLow, float32& outHigh) const
{
    outLow = m_Shape.MinHeight;
    outHigh = m_Shape.MaxHeight;
    if (!m_WholeMap || m_WholeMap->Heightfield.IsEmpty())
        return;
    // The range the baked field holds, not the nominal [0, 1] band: modifier volumes drive samples
    // outside it and the renderer draws them there.
    const Terrain::HeightfieldData& field = m_WholeMap->Heightfield;
    float32 minNormalized = 0.0f;
    float32 maxNormalized = 1.0f;
    if (!m_WholeMap->Quadtree.TryGetGlobalHeightRange(field.GetWidth(), field.GetHeight(), minNormalized,
                                                      maxNormalized))
        field.GetMinMax(0, 0, static_cast<int32>(field.GetWidth()), static_cast<int32>(field.GetHeight()),
                        minNormalized, maxNormalized);
    // The scale orders the two products; the extremes keep a negative scale from inverting the range.
    const float32 a = m_OriginY + minNormalized * m_HeightScale;
    const float32 b = m_OriginY + maxNormalized * m_HeightScale;
    outLow = std::min(a, b);
    outHigh = std::max(a, b);
}

namespace
{

constexpr float32 kNotCovered = -1.0f;
// The share of the base relief envelope added for the sculpt overlay and the modifier layers.
constexpr float32 kPlanetLayerMarginShare = 0.25f;
// Everest's height over Earth's atmosphere height. The sky's air is Earth's air scaled to the
// atmosphere height, so a crest at this share has air above it as thick as Everest has.
constexpr float32 kHighestCrestShareOfAtmosphere = 8849.0f / Rendering::SkyPlanet::kEarthAtmosphereHeightMeters;

// The distance from `point` to the surface of `terrain`, or kNotCovered when the terrain does not
// cover the point. A planet is centered on the world origin, as the renderer draws it. It covers
// every point but its center: the direction of the point is the query.
float32 DistanceToSurface(TerrainService& service, const Components::Terrain& terrain,
                          const Components::WorldTransform& transform, const Mathematics::Vector3& point)
{
    if (terrain.Domain == Components::TerrainDomain::Spherical)
    {
        const float32 radial = point.Length();
        if (!(radial > 0.0f))
            return kNotCovered;
        return std::abs(radial - terrain.PlanetRadius);
    }
    const SurfaceHeightQuery query = service.SurfaceQuery(terrain, transform);
    if (!query.ContainsXZ(point.x, point.z))
        return kNotCovered;
    float32 low = 0.0f;
    float32 high = 0.0f;
    query.HeightRange(low, high);
    return std::max({low - point.y, point.y - high, 0.0f});
}

} // namespace

PlanetReliefShell PlanetReliefShellOf(float32 radius, const Components::TerrainPlanetRelief& relief)
{
    const float32 reach =
        CBTTerrain::kReliefEnvelope * std::abs(relief.Amplitude) * (1.0f + kPlanetLayerMarginShare);
    return {std::max(radius - reach, 0.0f), radius + reach};
}

TerrainHeightRangeMeters PlanetHeightRangeMeters(const Components::TerrainPlanetRelief& relief)
{
    const float32 reach = CBTTerrain::kReliefEnvelope * std::abs(relief.Amplitude);
    return {-reach, reach};
}

float32 AtmosphereHeightMeters(const Components::Terrain& terrain, const Components::TerrainPlanetRelief* relief)
{
    // A script can write a radius at or below 0; the scene reader and the Inspector refuse it.
    if (!(terrain.PlanetRadius > 0.0f))
        return 0.0f;
    switch (terrain.AtmosphereMode)
    {
    case Components::TerrainAtmosphereMode::ShareOfRadius:
        return terrain.AtmosphereShare * terrain.PlanetRadius;
    case Components::TerrainAtmosphereMode::HeightMeters:
        return std::min(terrain.AtmosphereHeight, terrain.PlanetRadius);
    case Components::TerrainAtmosphereMode::Auto:
        break;
    }
    const float32 earthLike = Rendering::SkyPlanet::kEarthAtmosphereShare * terrain.PlanetRadius;
    const float32 reliefTop = relief ? PlanetHeightRangeMeters(*relief).Max : 0.0f;
    // The radius is above 0 and Earth's ratio (100 km of air on a 6360 km radius) is below 1: the range is not empty.
    return std::clamp(reliefTop / kHighestCrestShareOfAtmosphere, earthLike, terrain.PlanetRadius);
}

SurfaceHeightTerrain FindSurfaceHeightTerrain(ECS::World& world, const Mathematics::Vector3& point)
{
    SurfaceHeightTerrain found;
    TerrainService* service = TerrainService::TryGet();
    if (!service)
        return found;
    float32 nearest = 0.0f;
    ECS::EntityHandle foundEntity{};
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle entity, const Components::Terrain& terrain, const Components::WorldTransform& transform) {
            const float32 distance = DistanceToSurface(*service, terrain, transform, point);
            if (distance < 0.0f || (found.Terrain && distance >= nearest))
                return;
            found = {&terrain, &transform, static_cast<uint32>(entity.id)};
            foundEntity = entity;
            nearest = distance;
        });
    if (found.Terrain && found.Terrain->Domain == Components::TerrainDomain::Spherical)
        found.Relief = world.GetComponent<Components::TerrainPlanetRelief>(foundEntity);
    return found;
}

SurfaceHeightQuery ResolveSurfaceHeightQuery(ECS::World& world, const Mathematics::Vector3& point)
{
    TerrainService* service = TerrainService::TryGet();
    const SurfaceHeightTerrain terrain = FindSurfaceHeightTerrain(world, point);
    if (!service || !terrain.Terrain)
        return {};
    if (terrain.Terrain->Domain == Components::TerrainDomain::Spherical)
    {
        const TerrainPages* pages = service->FindTerrainPages();
        return pages ? pages->PlanetSurfaceQuery(terrain.EntityId) : SurfaceHeightQuery{};
    }
    return service->SurfaceQuery(*terrain.Terrain, *terrain.Transform);
}

bool FetchFinestSurfaceHeightPage(ECS::World& world, const Mathematics::Vector3& point)
{
    TerrainService* service = TerrainService::TryGet();
    const SurfaceHeightTerrain terrain = FindSurfaceHeightTerrain(world, point);
    if (!service || !terrain.Terrain || terrain.Terrain->Domain != Components::TerrainDomain::Planar)
        return false;
    return service->FetchFinestHeightPage(*terrain.Terrain, point);
}

void RequestSurfaceHeightWindow(const Mathematics::Vector3& center)
{
    if (TerrainService* service = TerrainService::TryGet())
        service->GetTerrainPages().RequestReaderWindow(center);
}

} // namespace GameEngine::TerrainECS
