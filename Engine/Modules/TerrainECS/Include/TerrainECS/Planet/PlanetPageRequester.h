#pragma once

#include "TerrainECS/PageLevelRule.h"
#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "Types/Types.h"

#include <memory>
#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{
class GeneratedHeightPageProvider;
}

// The level rule of a planet's height pages (design page-streaming-planets 3.5): the planar rule
// on six EqualAngleCube faces, with each page's footprint on the sphere and a horizon test.
namespace GameEngine::TerrainECS
{

/// A planet's height pyramid as the level rule sees it. Heights are h = r - R, in meters.
struct PlanetPageRequestTerrain
{
    float64 ReferenceRadius = 0.0; ///< R
    uint32 FaceSamples = 0;        ///< level-0 samples along a face's side
    uint32 CacheSlots = 0;         ///< the slots of the field's height cache (the profile's budget)
    /// The planet's generator, of FaceSamples a side: each page's height range (PlanetPageHeightRange).
    std::shared_ptr<const PageStreaming::GeneratedHeightPageProvider> Heights;
};

/// The level-0 samples along a face's side of a planet of radius `referenceRadius` meters whose
/// level-0 texel is at most `level0Texel` meters: the fewest of the form m x 2^k + 1, m at most
/// kPageOwnedSamples, so every level halves to the top page. 0 when the face would pass
/// PageStreaming::kPageStoreMaxSamplesPerAxis.
uint32 PlanetFaceSamples(float64 referenceRadius, float32 level0Texel);

/// The largest whole-meter radius whose PlanetFaceSamples at `level0Texel` is not 0 and is at most
/// `samples`; 0 when no whole meter fits. With PageStreaming::kPageStoreMaxSamplesPerAxis, it is the
/// largest planet the page store holds at that texel.
uint64 LargestPlanetRadius(uint32 samples, float32 level0Texel);

/// A planet request made to fit a field's slots (PlanetPageRequester::BuildWithinSlots).
struct PageSlotFit
{
    uint32 Passes = 0;          ///< the times the request was made one level coarser
    uint32 FullDetailPages = 0; ///< the pages the request wants at full detail
};

/// The pages a planet's field needs for its cameras, each frame: the geometry over the disc, the
/// shading over the view wedge. Holds the planet's pyramid and its scratch, so a frame allocates
/// nothing once the request set's size has been seen.
class PlanetPageRequester
{
public:
    PlanetPageRequester();
    ~PlanetPageRequester();
    PlanetPageRequester(PlanetPageRequester&&) noexcept;
    PlanetPageRequester& operator=(PlanetPageRequester&&) noexcept;

    /// (Re)configure for `planet`: its pyramid and pinned levels (FirstPinnedPageLevel on six faces),
    /// refined over `area`.
    void Configure(const PlanetPageRequestTerrain& planet, PageRequestArea area);

    /// The pinned pages, then each face's quadtree as PageRequester walks it, with the exact
    /// distance to the page's region between R + its min and R + its max height, and the horizon
    /// on the floor sphere (CBT_CornerOccluded's rule). `cameras` are relative to the planet's center;
    /// the wedge lies in the plane tangent to the sphere at each camera's foot.
    void Build(std::span<const PageRequestCamera> cameras, const PageLevelView& view,
               std::vector<PageStreaming::PageWant>& out);
    /// Build at `view`, then one level coarser everywhere (the pixel error doubled) while the pages
    /// pass `slots`. The walk ends: the pinned pages fit a quarter of the slots.
    PageSlotFit BuildWithinSlots(std::span<const PageRequestCamera> cameras, const PageLevelView& view, uint32 slots,
                                 std::vector<PageStreaming::PageWant>& out);

    uint32 FirstPinnedLevel() const { return m_FirstPinned; }
    const std::vector<PageStreaming::PageStoreLevel>& Levels() const { return m_Levels; }

private:
    // A camera on the sphere and its wedge: defined in the .cpp.
    struct Camera;
    Camera PrepareCamera(const PageRequestCamera& view) const;
    // Each visited page's geometry on the sphere, built once per address: defined in the .cpp.
    struct PageShapes;

    // The nearest distance from any camera whose `area` reaches `address`, and its local level-0
    // texel; false when no camera reaches it.
    bool NearestReach(const PageStreaming::PageAddress& address, PageRequestArea area, float32& outDistance,
                      float32& outTexel);

    PlanetPageRequestTerrain m_Planet;
    std::vector<PageStreaming::PageStoreLevel> m_Levels;
    std::vector<Camera> m_Cameras;
    std::vector<PageStreaming::PageAddress> m_Frontier;
    uint32 m_FirstPinned = 0;
    PageRequestArea m_Area = PageRequestArea::Disc;
    float64 m_FloorRadius = 0.0; ///< R + the lowest height of the six top pages
    std::unique_ptr<PageShapes> m_Shapes;
};

} // namespace GameEngine::TerrainECS
