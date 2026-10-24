#pragma once

#include "TerrainECS/PageLevelRule.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

namespace GameEngine::TerrainECS
{

class CpuHeightPageCache;

/// What a caller does with an answer coarser than the finest data the terrain has at the point
/// (design page-streaming D3). Every caller names its policy.
enum class SurfaceHeightPolicy : uint8
{
    RequireFinest, ///< a coarse answer counts as unsampled (a precision check, a fill, a bake)
    AcceptCoarse,  ///< a coarse answer is an answer (a pick, a draped ring, a skyline)
};

/// How an answer relates to the finest data the terrain has at the point.
enum class SurfaceHeightDetail : uint8
{
    Finest, ///< from the finest level present in the terrain's source there
    Coarse, ///< from a coarser pinned or resident level: the finest page is not on the CPU yet
};

/// One height answer.
struct SurfaceHeightSample
{
    float32 Height = 0.0f; ///< world meters
    uint32 Level = 0;      ///< the pyramid level that answered (0 the finest)
    SurfaceHeightDetail Detail = SurfaceHeightDetail::Finest;
};

/// The CPU height of a paged terrain at a world point, from its CPU height pages (CpuHeightPageCache):
/// the bilinear sample of the finest resident page over the point, the resolve's page lattice and
/// apron (LocatePageTexel), never a read outside a page. Inside the footprint it is never unsampled
/// once its pages have had one Update: the pinned top page ends every walk (design D3). The planar
/// arm of page-streaming-planets 3.11; a planet's spherical arm takes the same call.
///
/// A view of the cache: valid until the cache's next Update, on the main thread.
class SurfaceHeightQuery
{
public:
    SurfaceHeightQuery() = default;
    /// The query of stream `stream` of `cache`, placed by `shape` (its pyramid in world meters) with
    /// its normalized samples at world height originY + sample x heightScale.
    SurfaceHeightQuery(const CpuHeightPageCache& cache, uint32 stream, const PageRequestTerrain& shape,
                       float32 originY, float32 heightScale);

    /// False when there is no paged terrain to read.
    bool IsValid() const { return m_Cache != nullptr; }

    /// The height under `worldPoint` (its X and Z on a planar terrain). False outside the footprint,
    /// before the terrain's pages have had an Update, and, under RequireFinest, for a coarse answer;
    /// `out` is written only when it returns true.
    bool SampleHeight(const Mathematics::Vector3& worldPoint, SurfaceHeightPolicy policy, SurfaceHeightSample& out) const;

    /// The world height range of the whole terrain, the bound a ray march over it may assume.
    float32 MinHeight() const { return m_Shape.MinHeight; }
    float32 MaxHeight() const { return m_Shape.MaxHeight; }

private:
    const CpuHeightPageCache* m_Cache = nullptr;
    uint32 m_Stream = 0;
    PageRequestTerrain m_Shape;
    float32 m_OriginY = 0.0f;
    float32 m_HeightScale = 0.0f;
};

} // namespace GameEngine::TerrainECS
