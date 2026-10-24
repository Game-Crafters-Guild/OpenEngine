#pragma once

#include "TerrainECS/PageLevelRule.h"
#include "PageStreaming/CpuPageDirectory.h"
#include "PageStreaming/HeightPageOverlay.h"
#include "PageStreaming/PageStoreFormat.h"
#include "Mathematics/Rect.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Components
{
struct Terrain;
struct WorldTransform;
} // namespace GameEngine::Components

namespace GameEngine::TerrainECS
{

struct TerrainData;
struct TerrainHandle;

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

/// Where a paged terrain's heights stand, for a reader that keeps its place in their edits
/// (PlanarHeightfieldEditCursor). A new Source changes every height. A new OverlayVersion changes
/// the level-0 samples OverlayChanged when the reader holds OverlayChangedSince, else any sample.
struct SurfaceHeightEdits
{
    uint64 Source = 0; ///< the page source's identity; 0 while the terrain has none
    uint64 OverlayVersion = 0;
    PageStreaming::PageSampleRect OverlayChanged;
    uint64 OverlayChangedSince = 0;
};

/// Where a paged terrain's CPU pages stand, for a reader that keeps answers across frames
/// (SurfaceHeightQuery::CollectPageChanges): the stream's configuration and its next page change.
/// Zero on a terrain with no pages.
struct SurfaceHeightPageStamp
{
    uint64 Epoch = 0;
    uint64 Change = 0;
};

/// Samples per axis SurfaceHeightQuery::FillFinestPage writes: a level-0 page's owned samples and
/// the first sample of the next page, so the pages' grids meet on a shared row and column.
inline constexpr uint32 kFinestPageGridSamples = PageStreaming::kPageOwnedSamples + 1u;

/// The CPU height of one planar terrain at a world point: the one reader type every CPU reader of
/// terrain height takes (design page-streaming D3; the planar arm of page-streaming-planets 3.11).
///
/// Three arms behind one SampleHeight:
/// - a paged terrain reads its pages on the CPU (the residency's CpuPages directory): the bilinear
///   sample of the finest page over the point plus the terrain's modifier overlay there, on the
///   resolve's page lattice and apron (LocatePageTexel), never a read outside a page. The overlay is
///   read as it is now, so an edit shows from the frame its overlay is rebaked. Inside the footprint
///   it answers from its first Update on: the top page ends every walk.
/// - a terrain whose pages have no source yet (a heightmap whose height store is still cooking)
///   answers unsampled everywhere, under every policy: never a stale height. Its footprint and
///   lattice still answer, so a caller tells "wait" from "no ground".
/// - a whole-map terrain (one heightfield) answers its bilinear sample, always Finest at level 0.
///
/// Corner (0, 0) of the lattice sits on the footprint's minimum corner. A view of the terrain's
/// data: valid until the next height-page Update, on the main thread.
class SurfaceHeightQuery
{
public:
    SurfaceHeightQuery() = default;
    /// The query of a stream's CPU pages `pages`, whose pyramid is `levels`, placed by `shape` (its
    /// pyramid in world meters) with its normalized samples, plus `overlay` (null: none), at world
    /// height originY + sample x heightScale; `edits` is where its heights stand.
    SurfaceHeightQuery(const PageStreaming::CpuPageDirectory& pages,
                       std::span<const PageStreaming::PageStoreLevel> levels, const PageRequestTerrain& shape,
                       float32 originY, float32 heightScale, const PageStreaming::HeightPageOverlay* overlay,
                       const SurfaceHeightEdits& edits);
    /// The query of a terrain placed by `shape` whose pages have no source yet: every sample is
    /// unsampled.
    SurfaceHeightQuery(const PageRequestTerrain& shape, float32 originY, float32 heightScale);
    /// The query of the whole-map terrain `wholeMap`, whose handle is `handle`, its heightfield
    /// spread over `shape`'s footprint.
    SurfaceHeightQuery(const TerrainData& wholeMap, const TerrainHandle& handle, const PageRequestTerrain& shape,
                       float32 originY, float32 heightScale);

    /// False when there is no planar terrain: no footprint.
    bool IsValid() const { return m_Shape.SamplesX >= 2u && m_Shape.SamplesZ >= 2u; }

    /// The height under `worldPoint` (its X and Z). False outside the footprint, while the terrain's
    /// pages have no source or have not had an Update, and, under RequireFinest, for a coarse answer;
    /// `out` is written only when it returns true.
    bool SampleHeight(const Mathematics::Vector3& worldPoint, SurfaceHeightPolicy policy, SurfaceHeightSample& out) const;

    /// Whether world (x, z) lies on the footprint. Outside it there is no ground and never will be;
    /// inside it a false SampleHeight is a page still on its way, which a caller waits out.
    bool ContainsXZ(float32 worldX, float32 worldZ) const;

    /// True on a paged terrain whose pages have a source: its CPU pages answer (or will), not a
    /// heightfield. False on a whole-map terrain and while the pages have no source yet.
    bool HasPageSource() const { return !m_Pages.Entries.empty() && !m_Levels.empty(); }

    /// Writes the normalized level-0 samples of finest page (pageX, pageZ) to `out`
    /// (kFinestPageGridSamples squared, row-major, X fastest), each plus the modifier overlay there:
    /// lattice samples 128 pageX to 128 pageX + 128 on X, the same on Z. They come from the page
    /// alone: its owned samples and its far apron, which equals the next page's first sample bit for
    /// bit. Past the field's last sample a value repeats the edge (the page store's apron rule).
    /// False, writing nothing, unless that page is on the CPU at level 0.
    bool FillFinestPage(uint32 pageX, uint32 pageZ, std::span<float32> out) const;

    /// The level-0 page over world (x, z): the page a false RequireFinest sample there waits on.
    /// False off a paged terrain with a source, or outside the footprint.
    bool FinestPageAt(float32 worldX, float32 worldZ, uint32& outPageX, uint32& outPageZ) const;

    /// The footprint's minimum corner and size, in world meters.
    float32 FootprintOriginX() const { return m_Shape.OriginX; }
    float32 FootprintOriginZ() const { return m_Shape.OriginZ; }
    float32 FootprintSizeX() const;
    float32 FootprintSizeZ() const;
    /// The finest lattice's samples per axis.
    uint32 LatticeSamplesX() const { return m_Shape.SamplesX; }
    uint32 LatticeSamplesZ() const { return m_Shape.SamplesZ; }
    /// Meters between the finest lattice's corners. A caller aligned to it reads heights at the
    /// samples the author edits.
    float32 LatticeSpacingX() const { return m_Shape.Level0TexelX; }
    float32 LatticeSpacingZ() const { return m_Shape.Level0TexelZ; }

    /// The world height range of the whole terrain, the bound a ray march over it may assume. A
    /// whole-map terrain reads it from its heightfield, so a caller asks once per use.
    void HeightRange(float32& outLow, float32& outHigh) const;
    /// The world height of a normalized sample of 0 and the meters per normalized unit.
    float32 OriginY() const { return m_OriginY; }
    float32 HeightScale() const { return m_HeightScale; }

    /// The whole-map terrain the query reads, or null on a paged terrain.
    const TerrainData* WholeMap() const { return m_WholeMap; }
    /// The whole-map terrain's handle as one value (its generation over its index), or 0 on a paged
    /// terrain: a reader that keeps answers keys on it, since a new heightfield can take a freed
    /// one's address.
    uint64 WholeMapIdentity() const { return m_WholeMapIdentity; }
    /// Where a paged terrain's heights stand (all zero on a whole-map terrain).
    const SurfaceHeightEdits& Edits() const { return m_Edits; }

    /// Where the terrain's CPU pages stand now.
    SurfaceHeightPageStamp PageStamp() const { return {m_Pages.Epoch, m_Pages.ChangesEnd()}; }
    /// Appends to `out` the world rectangles (X along world X, Y along world Z) where an answer may
    /// differ from the one it gave at `since` because a page landed on the CPU, was found absent from
    /// the source or was dropped: each rectangle covers every point whose walk reads the page. False
    /// when the record does not reach back to `since` (another stream, a restarted one, or more
    /// changes than the record keeps): every answer may differ. A whole-map terrain has no pages and
    /// reports nothing. Edits are not page changes: PlanarHeightfieldEditCursor reports those.
    bool CollectPageChanges(const SurfaceHeightPageStamp& since, std::vector<Mathematics::Rect>& out) const;
    /// True when level-0 page (pageX, pageZ) may hold other samples than it held at `since`: the
    /// record holds a change of that page since, or does not reach back to `since`. Changes of other
    /// pages and levels do not count. False on a whole-map terrain.
    bool FinestPageChangedSince(const SurfaceHeightPageStamp& since, uint32 pageX, uint32 pageZ) const;

private:
    bool SamplePaged(float32 u, float32 v, SurfaceHeightPolicy policy, SurfaceHeightSample& out) const;

    PageStreaming::CpuPageDirectory m_Pages; // empty: no page source
    const PageStreaming::HeightPageOverlay* m_Overlay = nullptr;
    const TerrainData* m_WholeMap = nullptr;
    uint64 m_WholeMapIdentity = 0;
    std::span<const PageStreaming::PageStoreLevel> m_Levels;
    PageRequestTerrain m_Shape;
    float32 m_OriginY = 0.0f;
    float32 m_HeightScale = 0.0f;
    SurfaceHeightEdits m_Edits;
};

/// The terrain ResolveSurfaceHeightQuery answers for: the first entity in `world` with a Terrain and
/// a WorldTransform, planar or spherical. Both are null when there is none; they point into the
/// world's storage until its next structural change.
struct SurfaceHeightTerrain
{
    const Components::Terrain* Terrain = nullptr;
    const Components::WorldTransform* Transform = nullptr;
};

/// Finds the terrain ResolveSurfaceHeightQuery answers for. A reader that takes more of that terrain
/// than its heights (its tiles, its modifier stack) finds it here, so it reads the same terrain.
SurfaceHeightTerrain FindSurfaceHeightTerrain(ECS::World& world);

/// The query of the terrain FindSurfaceHeightTerrain finds in `world`, matching the
/// single-active-terrain scope CBTTerrainECS::FindActiveTerrain and the gizmo use
/// (TerrainService::SurfaceQuery). Invalid when that terrain is spherical, absent, or has no height
/// data yet.
SurfaceHeightQuery ResolveSurfaceHeightQuery(ECS::World& world);

/// Keeps a CPU window of finest height pages around `center` at the next height-page Update, for
/// every paged terrain (TerrainHeightPages::RequestReaderWindow): a reader that needs the finest
/// height away from every camera and body (a navigation bake waiting on a page) asks on each frame
/// it waits.
void RequestSurfaceHeightWindow(const Mathematics::Vector3& center);

} // namespace GameEngine::TerrainECS
