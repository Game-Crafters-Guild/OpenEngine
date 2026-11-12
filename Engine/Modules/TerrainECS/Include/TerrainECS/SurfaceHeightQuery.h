#pragma once

#include "TerrainECS/PageLevelRule.h"
#include "PageStreaming/CpuPage/CpuPageDirectory.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
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
struct TerrainPlanetRelief;
struct WorldTransform;
} // namespace GameEngine::Components

namespace GameEngine::TerrainECS
{

struct TerrainData;
struct TerrainHandle;
struct TerrainHeightRangeMeters;

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

/// A planet's pages as SurfaceHeightQuery's spherical arm reads them (design page-streaming-planets 3.11).
struct SurfaceHeightSphere
{
    Mathematics::Vector3 Center;   ///< the planet's center in world space
    float64 ReferenceRadius = 0.0; ///< meters; the pages hold heights above it
    uint32 FaceSamples = 0;        ///< level-0 samples along a face's side
    /// What the planet's layers add to its pages, as its pages hold them (null: nothing).
    const PageStreaming::HeightPageLayer* Modifiers = nullptr;
    const PageStreaming::HeightPageLayer* Overlay = nullptr;
};

/// Samples per axis SurfaceHeightQuery::FillFinestPage writes: a level-0 page's owned samples and
/// the first sample of the next page, so the pages' grids meet on a shared row and column.
inline constexpr uint32 kFinestPageGridSamples = PageStreaming::kPageOwnedSamples + 1u;

/// The CPU height of one terrain at a world point: the one reader type every CPU reader of terrain
/// height takes (design page-streaming D3; page-streaming-planets 3.11).
///
/// Four arms behind one SampleHeight:
/// - a paged terrain reads its pages on the CPU (the residency's CpuPages directory): the bilinear
///   sample of the finest page over the point plus the terrain's modifier overlay there, on the
///   resolve's page lattice and apron (LocatePageTexel), never a read outside a page. The overlay is
///   read as it is now, so an edit shows from the frame its overlay is rebaked. Inside the footprint
///   it answers from its first Update on: the top page ends every walk.
/// - a terrain whose pages have no source yet (a heightmap whose height store is still cooking)
///   answers unsampled everywhere, under every policy: never a stale height. Its footprint and
///   lattice still answer, so a caller tells "wait" from "no ground".
/// - a whole-map terrain (one heightfield) answers its bilinear sample, always Finest at level 0.
/// - a planet (IsSpherical) reads its CPU pages by direction from its center: the page sampler of
///   its page bodies (SamplePlanetPageTexel) plus its layers at the level-0 point, as its pages are drawn.
///
/// Corner (0, 0) of the lattice sits on the footprint's minimum corner. The footprint is the
/// lattice's extent or less: a tiled terrain's lattice covers whole tiles, its footprint its authored
/// size (ClipFootprint). A view of the terrain's data: valid until the next height-page Update, on
/// the main thread.
class SurfaceHeightQuery
{
public:
    SurfaceHeightQuery() = default;
    /// The query of a stream's CPU pages `pages`, whose pyramid is `levels`, placed by `shape` (its
    /// pyramid in world meters) with its normalized samples, plus `overlay` (null: none), at world
    /// height originY + sample x heightScale; `edits` is where its heights stand.
    SurfaceHeightQuery(const PageStreaming::CpuPageDirectory& pages,
                       std::span<const PageStreaming::PageStoreLevel> levels, const PageRequestTerrain& shape,
                       float32 originY, float32 heightScale, const PageStreaming::HeightPageLayer* overlay,
                       const SurfaceHeightEdits& edits);
    /// The query of a terrain placed by `shape` whose pages have no source yet: every sample is
    /// unsampled.
    SurfaceHeightQuery(const PageRequestTerrain& shape, float32 originY, float32 heightScale);
    /// The query of the whole-map terrain `wholeMap`, whose handle is `handle`, its heightfield
    /// spread over `shape`'s footprint.
    SurfaceHeightQuery(const TerrainData& wholeMap, const TerrainHandle& handle, const PageRequestTerrain& shape,
                       float32 originY, float32 heightScale);
    /// The query of a planet's CPU pages `pages`, whose pyramid is `levels`, placed by `sphere`.
    SurfaceHeightQuery(const PageStreaming::CpuPageDirectory& pages,
                       std::span<const PageStreaming::PageStoreLevel> levels, const SurfaceHeightSphere& sphere);

    /// False when there is no planar terrain: no footprint. A planet's query is not valid: IsSpherical.
    bool IsValid() const { return m_Shape.SamplesX >= 2u && m_Shape.SamplesZ >= 2u; }
    /// True on a planet's query: SampleHeight answers by direction.
    bool IsSpherical() const { return m_Sphere.FaceSamples >= 2u; }
    /// The planet a spherical query reads.
    const SurfaceHeightSphere& Sphere() const { return m_Sphere; }

    /// The height under `worldPoint` (its X and Z). False outside the footprint, while the terrain's
    /// pages have no source or have not had an Update, and, under RequireFinest, for a coarse answer;
    /// `out` is written only when it returns true. On a planet: the height in meters above the
    /// reference radius on the line from the center through `worldPoint` (any point but the center).
    bool SampleHeight(const Mathematics::Vector3& worldPoint, SurfaceHeightPolicy policy, SurfaceHeightSample& out) const;

    /// Whether world (x, z) lies on the footprint. Outside it there is no ground and never will be;
    /// inside it a false SampleHeight is a page still on its way, which a caller waits out.
    bool ContainsXZ(float32 worldX, float32 worldZ) const;

    /// True on a paged terrain whose pages have a source: its CPU pages answer (or will), not a
    /// heightfield. False on a whole-map terrain and while the pages have no source yet.
    bool HasPageSource() const { return !m_Pages.Empty() && !m_Levels.empty(); }

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

    /// Clips the footprint to `sizeX` x `sizeZ` world meters from its minimum corner (a tiled
    /// terrain's authored size). A size past the lattice's extent keeps the lattice's extent.
    void ClipFootprint(float32 sizeX, float32 sizeZ);

    /// The footprint's minimum corner and size, in world meters.
    float32 FootprintOriginX() const { return m_Shape.OriginX; }
    float32 FootprintOriginZ() const { return m_Shape.OriginZ; }
    float32 FootprintSizeX() const { return m_FootprintSizeX; }
    float32 FootprintSizeZ() const { return m_FootprintSizeZ; }
    /// The footprint as a world XZ rectangle: X is world x, Y is world z.
    Mathematics::Rect Footprint() const
    {
        return Mathematics::Rect{m_Shape.OriginX, m_Shape.OriginZ, m_FootprintSizeX, m_FootprintSizeZ};
    }
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
    bool SampleSphere(const Mathematics::Vector3& worldPoint, SurfaceHeightPolicy policy, SurfaceHeightSample& out) const;
    // The lattice's extent in world meters: the span the uv of every page lookup is relative to.
    float32 LatticeSizeX() const;
    float32 LatticeSizeZ() const;

    PageStreaming::CpuPageDirectory m_Pages; // empty: no page source
    SurfaceHeightSphere m_Sphere;            // FaceSamples 0: not a planet
    const PageStreaming::HeightPageLayer* m_Overlay = nullptr;
    const TerrainData* m_WholeMap = nullptr;
    uint64 m_WholeMapIdentity = 0;
    std::span<const PageStreaming::PageStoreLevel> m_Levels;
    PageRequestTerrain m_Shape;
    float32 m_OriginY = 0.0f;
    float32 m_HeightScale = 0.0f;
    float32 m_FootprintSizeX = 0.0f;
    float32 m_FootprintSizeZ = 0.0f;
    SurfaceHeightEdits m_Edits;
};

/// The terrain ResolveSurfaceHeightQuery answers for at a point, planar or spherical. Both are null
/// when no terrain covers the point; they point into the world's storage until its next structural
/// change.
struct SurfaceHeightTerrain
{
    const Components::Terrain* Terrain = nullptr;
    const Components::WorldTransform* Transform = nullptr;
    uint32 EntityId = 0; ///< the terrain entity's id: a planet's key in the height pages
    /// A planet's base relief; null on a planar terrain and on a planet without one.
    const Components::TerrainPlanetRelief* Relief = nullptr;
};

/// Finds the terrain under `point`: a planar terrain covers the points inside its footprint, a
/// spherical terrain every point but its center. When several cover the point, the one
/// whose surface is nearest to it wins, so the answer does not depend on the order of the world's
/// storage. A reader that takes more of that terrain than its heights (its tiles, its modifier
/// stack) finds it here, so it reads the same terrain.
SurfaceHeightTerrain FindSurfaceHeightTerrain(ECS::World& world, const Mathematics::Vector3& point);

/// The radii in meters from a planet's center between which its ground lies.
struct PlanetReliefShell
{
    float32 Inner = 0.0f;
    float32 Outer = 0.0f;
};

/// The relief shell of a planet of `radius` with base relief `relief`: the radius -/+ the relief
/// envelope (the base noise reaches 1.5 times its amplitude), widened by a quarter for the sculpt
/// overlay and the modifier layers. Inner is never below zero.
PlanetReliefShell PlanetReliefShellOf(float32 radius, const Components::TerrainPlanetRelief& relief);

/// The height range of a planet's ground in meters about its radius: -/+ the relief envelope.
/// The rows of a planet rule set band height inside it (MakeDefaultPlanetSurfaceRules).
TerrainHeightRangeMeters PlanetHeightRangeMeters(const Components::TerrainPlanetRelief& relief);

/// The height of a spherical terrain's atmosphere top above its PlanetRadius, in meters: the one value the sky
/// and the Inspector read. ShareOfRadius gives AtmosphereShare x PlanetRadius; HeightMeters gives AtmosphereHeight,
/// and a stored height above the radius reads as the radius. Auto gives the larger of Earth's ratio (100 km of air
/// on a 6360 km radius) x PlanetRadius and the relief's top (PlanetHeightRangeMeters) over Everest's share of
/// Earth's atmosphere, at most PlanetRadius. A PlanetRadius at or below 0 gives 0. `relief` may be null: no relief.
float32 AtmosphereHeightMeters(const Components::Terrain& terrain, const Components::TerrainPlanetRelief* relief);

/// The query of the terrain FindSurfaceHeightTerrain finds under `point` (TerrainService::SurfaceQuery).
/// Spherical (IsSpherical) on a streamed planet; neither valid nor spherical when no terrain covers
/// the point or it has no height data yet.
SurfaceHeightQuery ResolveSurfaceHeightQuery(ECS::World& world, const Mathematics::Vector3& point);

/// Fetches the level-0 height page under `point` of the planar paged terrain FindSurfaceHeightTerrain
/// finds there, now, on the main thread (TerrainService::FetchFinestHeightPage). A caller that places
/// something on a Coarse answer calls it, then resolves its query again: the answer there is then
/// Finest. Returns whether that page is then on the CPU; false on a planet and off a paged terrain.
bool FetchFinestSurfaceHeightPage(ECS::World& world, const Mathematics::Vector3& point);

/// Keeps a CPU window of finest height pages around `center` at the next height-page Update, for
/// every paged terrain (TerrainPages::RequestReaderWindow): a reader that needs the finest
/// height away from every camera and body (a navigation bake waiting on a page) asks on each frame
/// it waits.
void RequestSurfaceHeightWindow(const Mathematics::Vector3& center);

} // namespace GameEngine::TerrainECS
