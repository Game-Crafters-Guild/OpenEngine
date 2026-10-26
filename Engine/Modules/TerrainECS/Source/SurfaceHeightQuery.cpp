#include "TerrainECS/SurfaceHeightQuery.h"

#include "TerrainECS/PagedHeightSampler.h"
#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainService.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace GameEngine::TerrainECS
{

namespace
{

// The overlay's delta at stride texel (x, z) of the page `texel` names at `level` (stride texel s of
// page p is level sample p x kPageOwnedSamples + s - kPageApronSamples).
float32 OverlayTap(const PageStreaming::HeightPageOverlay& overlay, uint32 level, const PageTexel& texel, uint32 x,
                   uint32 z)
{
    const int64 sampleX =
        static_cast<int64>(texel.PageX) * PageStreaming::kPageOwnedSamples + x - PageStreaming::kPageApronSamples;
    const int64 sampleZ =
        static_cast<int64>(texel.PageZ) * PageStreaming::kPageOwnedSamples + z - PageStreaming::kPageApronSamples;
    if (sampleX < 0 || sampleZ < 0)
        return 0.0f; // the apron before the first sample: no tap inside the footprint reads it
    return overlay.Delta(level, static_cast<uint32>(sampleX), static_cast<uint32>(sampleZ));
}

// The overlay's delta at `texel` of a page of `level`: the same four taps and weights as
// SampleGridBilinearTexel over the page's stride grid, read from the overlay's level samples. The
// bilinear sum is linear, so base plus this equals the bilinear sample of the uploaded page (at a
// lattice point bit for bit: the weights are zero there).
float32 OverlayDelta(const PageStreaming::HeightPageOverlay& overlay, uint32 level, const PageTexel& texel)
{
    constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
    const float32 tx = std::clamp(texel.StrideX, 0.0f, static_cast<float32>(kStride - 1u));
    const float32 tz = std::clamp(texel.StrideZ, 0.0f, static_cast<float32>(kStride - 1u));
    const uint32 x0 = static_cast<uint32>(std::floor(tx));
    const uint32 z0 = static_cast<uint32>(std::floor(tz));
    const uint32 x1 = std::min(x0 + 1u, kStride - 1u);
    const uint32 z1 = std::min(z0 + 1u, kStride - 1u);
    const float32 fx = tx - static_cast<float32>(x0);
    const float32 fz = tz - static_cast<float32>(z0);
    const float32 d00 = OverlayTap(overlay, level, texel, x0, z0);
    const float32 d10 = OverlayTap(overlay, level, texel, x1, z0);
    const float32 d01 = OverlayTap(overlay, level, texel, x0, z1);
    const float32 d11 = OverlayTap(overlay, level, texel, x1, z1);
    return Math::Lerp(Math::Lerp(d00, d10, fx), Math::Lerp(d01, d11, fx), fz);
}

} // namespace

SurfaceHeightQuery::SurfaceHeightQuery(const PageStreaming::CpuPageDirectory& pages,
                                       std::span<const PageStreaming::PageStoreLevel> levels,
                                       const PageRequestTerrain& shape, float32 originY, float32 heightScale,
                                       const PageStreaming::HeightPageOverlay* overlay, const SurfaceHeightEdits& edits)
    : m_Pages(pages)
    , m_Overlay(overlay)
    , m_Levels(levels)
    , m_Shape(shape)
    , m_OriginY(originY)
    , m_HeightScale(heightScale)
    , m_Edits(edits)
{
}

SurfaceHeightQuery::SurfaceHeightQuery(const PageRequestTerrain& shape, float32 originY, float32 heightScale)
    : m_Shape(shape)
    , m_OriginY(originY)
    , m_HeightScale(heightScale)
{
}

SurfaceHeightQuery::SurfaceHeightQuery(const TerrainData& wholeMap, const TerrainHandle& handle,
                                       const PageRequestTerrain& shape, float32 originY, float32 heightScale)
    : m_WholeMap(&wholeMap)
    , m_WholeMapIdentity((static_cast<uint64>(handle.Generation) << 32u) | handle.Index)
    , m_Shape(shape)
    , m_OriginY(originY)
    , m_HeightScale(heightScale)
{
}

float32 SurfaceHeightQuery::FootprintSizeX() const
{
    return m_Shape.SamplesX < 2u ? 0.0f : static_cast<float32>(m_Shape.SamplesX - 1u) * m_Shape.Level0TexelX;
}

float32 SurfaceHeightQuery::FootprintSizeZ() const
{
    return m_Shape.SamplesZ < 2u ? 0.0f : static_cast<float32>(m_Shape.SamplesZ - 1u) * m_Shape.Level0TexelZ;
}

bool SurfaceHeightQuery::ContainsXZ(float32 worldX, float32 worldZ) const
{
    if (!IsValid())
        return false;
    const float32 u = (worldX - m_Shape.OriginX) / FootprintSizeX();
    const float32 v = (worldZ - m_Shape.OriginZ) / FootprintSizeZ();
    return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f; // false for a degenerate placement too
}

bool SurfaceHeightQuery::SampleHeight(const Mathematics::Vector3& worldPoint, SurfaceHeightPolicy policy,
                                      SurfaceHeightSample& out) const
{
    if (!ContainsXZ(worldPoint.x, worldPoint.z))
        return false;
    const float32 u = (worldPoint.x - m_Shape.OriginX) / FootprintSizeX();
    const float32 v = (worldPoint.z - m_Shape.OriginZ) / FootprintSizeZ();
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
    if (m_Pages.Entries.empty() || m_Levels.empty())
        return false; // no page source yet: unsampled under every policy
    // Down the pyramid from level 0: the first resident page answers. A page absent from the
    // source leaves its parent the finest data there; a page the CPU has not fetched yet makes
    // every coarser answer Coarse.
    bool finerMissing = false;
    for (uint32 level = 0; level < m_Levels.size(); ++level)
    {
        const PageTexel texel = LocatePageTexel(m_Levels, level, u, v);
        std::span<const uint8> bytes;
        const PageStreaming::CpuPageState state =
            m_Pages.Page(PageStreaming::PageAddress{0, static_cast<uint8>(level), texel.PageX, texel.PageZ}, bytes);
        if (state == PageStreaming::CpuPageState::Missing)
        {
            finerMissing = true;
            continue;
        }
        if (state == PageStreaming::CpuPageState::Absent)
            continue;
        const SurfaceHeightDetail detail = finerMissing ? SurfaceHeightDetail::Coarse : SurfaceHeightDetail::Finest;
        if (policy == SurfaceHeightPolicy::RequireFinest && detail == SurfaceHeightDetail::Coarse)
            return false;
        constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
        const std::span<const float32> samples = PageStreaming::HeightSamples(bytes);
        float32 normalized = SampleGridBilinearTexel(samples.data(), kStride, kStride, texel.StrideX, texel.StrideZ);
        if (m_Overlay)
            normalized += OverlayDelta(*m_Overlay, level, texel);
        out = SurfaceHeightSample{m_OriginY + normalized * m_HeightScale, level, detail};
        return true;
    }
    return false; // not even the top page is on the CPU yet
}

bool SurfaceHeightQuery::FillFinestPage(uint32 pageX, uint32 pageZ, std::span<float32> out) const
{
    assert(out.size() == static_cast<std::size_t>(kFinestPageGridSamples) * kFinestPageGridSamples);
    if (!HasPageSource() || out.size() != static_cast<std::size_t>(kFinestPageGridSamples) * kFinestPageGridSamples)
        return false;
    std::span<const uint8> bytes;
    if (m_Pages.Page(PageStreaming::PageAddress{0, 0, pageX, pageZ}, bytes) != PageStreaming::CpuPageState::Resident)
        return false;
    const std::span<const float32> samples = PageStreaming::HeightSamples(bytes);
    // Grid sample i of the page is its stride sample i + kPageApronSamples: the first owned sample on.
    constexpr uint32 kApron = PageStreaming::kPageApronSamples;
    constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
    const PageTexel page{pageX, pageZ, 0.0f, 0.0f};
    for (uint32 z = 0; z < kFinestPageGridSamples; ++z)
        for (uint32 x = 0; x < kFinestPageGridSamples; ++x)
        {
            float32 normalized = samples[static_cast<std::size_t>(z + kApron) * kStride + x + kApron];
            if (m_Overlay)
                normalized += OverlayTap(*m_Overlay, 0, page, x + kApron, z + kApron);
            out[static_cast<std::size_t>(z) * kFinestPageGridSamples + x] = normalized;
        }
    return true;
}

bool SurfaceHeightQuery::FinestPageAt(float32 worldX, float32 worldZ, uint32& outPageX, uint32& outPageZ) const
{
    if (m_Pages.Entries.empty() || m_Levels.empty() || !ContainsXZ(worldX, worldZ))
        return false;
    const PageTexel texel = LocatePageTexel(m_Levels, 0, (worldX - m_Shape.OriginX) / FootprintSizeX(),
                                            (worldZ - m_Shape.OriginZ) / FootprintSizeZ());
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
                                 ? m_Shape.OriginX + FootprintSizeX() + texelX
                                 : m_Shape.OriginX + (static_cast<float32>(page.X + 1u) * kOwned + 1.0f) * texelX;
        const float32 maxZ = page.Z + 1u >= level.PagesZ
                                 ? m_Shape.OriginZ + FootprintSizeZ() + texelZ
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

// The distance from `point` to the surface of `terrain`, or kNotCovered when the terrain does not
// cover the point. A planet is centered on the world origin, as the renderer draws it; its relief
// is HeightScale above PlanetRadius.
float32 DistanceToSurface(TerrainService& service, const Components::Terrain& terrain,
                          const Components::WorldTransform& transform, const Mathematics::Vector3& point)
{
    if (terrain.Domain == Components::TerrainDomain::Spherical)
    {
        const float32 radial = point.Length();
        if (radial > terrain.PlanetRadius + std::abs(terrain.HeightScale))
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

SurfaceHeightTerrain FindSurfaceHeightTerrain(ECS::World& world, const Mathematics::Vector3& point)
{
    SurfaceHeightTerrain found;
    TerrainService* service = TerrainService::TryGet();
    if (!service)
        return found;
    float32 nearest = 0.0f;
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
        [&](const Components::Terrain& terrain, const Components::WorldTransform& transform) {
            const float32 distance = DistanceToSurface(*service, terrain, transform, point);
            if (distance < 0.0f || (found.Terrain && distance >= nearest))
                return;
            found = {&terrain, &transform};
            nearest = distance;
        });
    return found;
}

SurfaceHeightQuery ResolveSurfaceHeightQuery(ECS::World& world, const Mathematics::Vector3& point)
{
    TerrainService* service = TerrainService::TryGet();
    const SurfaceHeightTerrain terrain = FindSurfaceHeightTerrain(world, point);
    if (!service || !terrain.Terrain || terrain.Terrain->Domain == Components::TerrainDomain::Spherical)
        return {};
    return service->SurfaceQuery(*terrain.Terrain, *terrain.Transform);
}

void RequestSurfaceHeightWindow(const Mathematics::Vector3& center)
{
    if (TerrainService* service = TerrainService::TryGet())
        service->GetHeightPages().RequestReaderWindow(center);
}

} // namespace GameEngine::TerrainECS
