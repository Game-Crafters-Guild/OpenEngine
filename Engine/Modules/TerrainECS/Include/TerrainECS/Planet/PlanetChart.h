#pragma once

#include "Mathematics/Vector3.h"
#include "PageStreaming/PageAddress.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{
struct PageFacePoint;
} // namespace GameEngine::PageStreaming

namespace GameEngine::TerrainECS
{

/// How a planet's faces split into charts (design page-streaming-planets 3.10): a chart is a square
/// block of 2^Shift level-0 pages a side, so a page maps to its chart by shifts. The last chart on an
/// axis holds the pages left over.
struct PlanetChartLayout
{
    uint32 FaceSamples = 0;  ///< level-0 samples along a face's side
    /// Level-0 pages along a face's side that own a cell; the last one can own fewer. When the cells
    /// fill whole pages, the store has one more page that owns the face's last sample alone; no body
    /// is built for it and no body reads it (the page before holds that sample in its apron).
    uint32 PagesPerSide = 0;
    uint32 Shift = 0;        ///< log2 of the pages along a chart's side
    uint32 ChartsPerSide = 0;
};

/// The layout of a planet of radius `referenceRadius` meters whose faces have `faceSamples` level-0
/// samples a side: the largest chart whose angle is at most min(2,048 m / R, 10 degrees) radians.
PlanetChartLayout ComputePlanetChartLayout(float64 referenceRadius, uint32 faceSamples);

/// One chart: a flat frame tangent to the planet at the chart's center direction, the frame its page
/// bodies are placed in. (TangentX, Up, TangentZ) is a rotation, Up the center direction, TangentX
/// the face's X axis made normal to Up. Each face's X x Z is its outward normal, so TangentZ points
/// against the face's Z axis. The plane lies at the reference radius: a plane point (x, z) is
/// planet-local R Up + x TangentX + z TangentZ.
struct PlanetChart
{
    uint8 Face = 0;
    uint32 X = 0;
    uint32 Z = 0;
    Mathematics::Vector3 Up;
    Mathematics::Vector3 TangentX;
    Mathematics::Vector3 TangentZ;
    /// The rectangle on the plane, in meters, that holds the chart's pages projected along the rays
    /// from the planet's center.
    float64 PlaneMinX = 0.0;
    float64 PlaneMinZ = 0.0;
    float64 PlaneSizeX = 0.0;
    float64 PlaneSizeZ = 0.0;
    /// The chart's first level-0 page and its pages along each axis.
    uint32 FirstPageX = 0;
    uint32 FirstPageZ = 0;
    uint32 PagesX = 0;
    uint32 PagesZ = 0;
};

/// The chart of level-0 page (pageX, pageZ) of `face`.
PlanetChart MakePlanetChartOfPage(const PlanetChartLayout& layout, float64 referenceRadius, uint8 face, uint32 pageX,
                                  uint32 pageZ);

/// The plane rectangle of the body of level-0 page (pageX, pageZ) of `chart`, in meters: the bounding
/// rectangle of the page's samples projected along the rays from the planet's center. The sample grid
/// is sheared on the plane away from the face's axes, so the bodies of one chart overlap where the
/// grid is sheared and leave no gap.
struct PlanetPageBodyRect
{
    float64 MinX = 0.0;
    float64 MinZ = 0.0;
    float64 SizeX = 0.0;
    float64 SizeZ = 0.0;
};
PlanetPageBodyRect PlanetPageBodyRectOf(const PlanetChartLayout& layout, float64 referenceRadius,
                                        const PlanetChart& chart, uint32 pageX, uint32 pageZ);

/// Appends the level-0 pages that planet-local `point` reaches by moving up to half the width of the page
/// under it along either face axis or both, past a cube edge on the next face. The page under the point
/// comes first; a page already in `out` is not added again.
void AppendPlanetWindowPages(const PlanetChartLayout& layout, const Mathematics::Vector3& point,
                             std::vector<PageStreaming::PageAddress>& out);

/// Appends the level-0 pages the body of page (face, pageX, pageZ) reads: the pages under its plane
/// rectangle widened by half a page, the margin the relief moves a sample's direction by. A page
/// already in `out` is not added again.
void AppendPlanetPageBodyReads(const PlanetChartLayout& layout, float64 referenceRadius, uint8 face, uint32 pageX,
                               uint32 pageZ, std::vector<PageStreaming::PageAddress>& out);

/// Where a face point falls on one level of a planet's pages: the page that owns it and the point on
/// that page's lattice, apron included (owned sample 0 at index 1).
struct PlanetPageTexel
{
    PageStreaming::PageAddress Address;
    float64 X = 0.0;
    float64 Z = 0.0;
};

/// The texel of face point `point` (level-0 sample coordinates) on level `level` of a planet whose
/// faces have `faceSamples` level-0 samples a side. The face's last sample is read from the apron of
/// the last page that owns a cell.
PlanetPageTexel LocatePlanetPageTexel(uint32 faceSamples, const PageStreaming::PageFacePoint& point, uint32 level);

/// The drawn height at `texel` of the page `heights` (kPageSampleCount, apron included, the layers
/// applied): the bilinear blend of its four lattice samples. The one sampler of a planet's CPU height.
float64 SamplePlanetPageTexel(std::span<const float32> heights, const PlanetPageTexel& texel);

} // namespace GameEngine::TerrainECS
