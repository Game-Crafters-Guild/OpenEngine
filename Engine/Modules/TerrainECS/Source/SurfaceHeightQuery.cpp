#include "TerrainECS/SurfaceHeightQuery.h"

#include "TerrainECS/CpuHeightPageCache.h"
#include "TerrainECS/PagedHeightSampler.h"
#include "TerrainECS/TerrainAtlas.h"

namespace GameEngine::TerrainECS
{

SurfaceHeightQuery::SurfaceHeightQuery(const CpuHeightPageCache& cache, uint32 stream, const PageRequestTerrain& shape,
                                       float32 originY, float32 heightScale)
    : m_Cache(&cache)
    , m_Stream(stream)
    , m_Shape(shape)
    , m_OriginY(originY)
    , m_HeightScale(heightScale)
{
}

bool SurfaceHeightQuery::SampleHeight(const Mathematics::Vector3& worldPoint, SurfaceHeightPolicy policy,
                                      SurfaceHeightSample& out) const
{
    if (!m_Cache || m_Shape.SamplesX < 2u || m_Shape.SamplesZ < 2u)
        return false;
    const std::span<const PageStreaming::PageStoreLevel> levels = m_Cache->Levels(m_Stream);
    if (levels.empty())
        return false;
    const float32 sizeX = static_cast<float32>(m_Shape.SamplesX - 1u) * m_Shape.Level0TexelX;
    const float32 sizeZ = static_cast<float32>(m_Shape.SamplesZ - 1u) * m_Shape.Level0TexelZ;
    const float32 u = (worldPoint.x - m_Shape.OriginX) / sizeX;
    const float32 v = (worldPoint.z - m_Shape.OriginZ) / sizeZ;
    if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f))
        return false; // outside the footprint (or a degenerate placement)

    // Down the pyramid from level 0: the first resident page answers. A page absent from the
    // source leaves its parent the finest data there; a page the CPU has not fetched yet makes
    // every coarser answer Coarse.
    bool finerMissing = false;
    for (uint32 level = 0; level < levels.size(); ++level)
    {
        const PageTexel texel = LocatePageTexel(levels, level, u, v);
        std::span<const float32> samples;
        const CpuPageState state = m_Cache->FindPage(
            m_Stream, PageStreaming::PageAddress{0, static_cast<uint8>(level), texel.PageX, texel.PageZ}, samples);
        if (state == CpuPageState::Missing)
        {
            finerMissing = true;
            continue;
        }
        if (state == CpuPageState::Absent)
            continue;
        const SurfaceHeightDetail detail = finerMissing ? SurfaceHeightDetail::Coarse : SurfaceHeightDetail::Finest;
        if (policy == SurfaceHeightPolicy::RequireFinest && detail == SurfaceHeightDetail::Coarse)
            return false;
        constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
        const float32 normalized = SampleGridBilinearTexel(samples.data(), kStride, kStride, texel.StrideX, texel.StrideZ);
        out = SurfaceHeightSample{m_OriginY + normalized * m_HeightScale, level, detail};
        return true;
    }
    return false; // not even the top page is on the CPU yet
}

} // namespace GameEngine::TerrainECS
