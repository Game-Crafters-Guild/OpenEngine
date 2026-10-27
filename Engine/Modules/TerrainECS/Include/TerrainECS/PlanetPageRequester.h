#pragma once

#include "TerrainECS/PageLevelRule.h"
#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "Mathematics/Vector3.h"
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

/// The pages a planet's geometry needs for its cameras, each frame. Holds the planet's pyramid and
/// its scratch, so a frame allocates nothing once the request set's size has been seen.
class PlanetPageRequester
{
public:
    PlanetPageRequester();
    ~PlanetPageRequester();
    PlanetPageRequester(PlanetPageRequester&&) noexcept;
    PlanetPageRequester& operator=(PlanetPageRequester&&) noexcept;

    /// (Re)configure for `planet`: its pyramid and pinned levels (FirstPinnedPageLevel on six faces).
    void Configure(const PlanetPageRequestTerrain& planet);

    /// The pinned pages, then each face's quadtree as HeightPageRequester walks it, with the exact
    /// distance to the page's region between R + its min and R + its max height, and the horizon
    /// on the floor sphere (CBT_CornerOccluded's rule). `cameras` are relative to the planet's center.
    void Build(std::span<const Mathematics::Vector3> cameras, const PageLevelView& view,
               std::vector<PageStreaming::PageWant>& out);

    uint32 FirstPinnedLevel() const { return m_FirstPinned; }
    const std::vector<PageStreaming::PageStoreLevel>& Levels() const { return m_Levels; }

private:
    struct Camera
    {
        float64 X = 0.0;
        float64 Y = 0.0;
        float64 Z = 0.0;
        float64 Length = 0.0;
        float64 FloorAngle = 0.0; ///< acos(f / |c|)
        bool UnderFloor = false;  ///< |c| <= f: the camera reaches every page
    };

    // Each visited page's geometry on the sphere, built once per address: defined in the .cpp.
    struct PageShapes;

    // The nearest distance from any camera that reaches `address`, and its local level-0 texel;
    // false when no camera reaches it.
    bool NearestReach(const PageStreaming::PageAddress& address, float32& outDistance, float32& outTexel);

    PlanetPageRequestTerrain m_Planet;
    std::vector<PageStreaming::PageStoreLevel> m_Levels;
    std::vector<Camera> m_Cameras;
    std::vector<PageStreaming::PageAddress> m_Frontier;
    uint32 m_FirstPinned = 0;
    float64 m_FloorRadius = 0.0; ///< R + the lowest height of the six top pages
    std::unique_ptr<PageShapes> m_Shapes;
};

} // namespace GameEngine::TerrainECS
