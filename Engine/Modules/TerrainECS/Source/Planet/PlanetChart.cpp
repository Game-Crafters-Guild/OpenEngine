#include "TerrainECS/Planet/PlanetChart.h"

#include "Mathematics/CubeFaces.h"
#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>

namespace GameEngine::TerrainECS
{
namespace
{

constexpr float64 kChartMaxSpanMeters = 2048.0;
constexpr float64 kChartMaxAngle = 10.0 * std::numbers::pi / 180.0;
// Points along each side of a chart's page block whose projections bound its plane rectangle: a
// side projects to a gently curved line, so its ends and inner points bound it.
constexpr uint32 kBoundPointsPerSide = 9u;
// Points along each axis of a body's widened rectangle whose pages make the pages it reads: closer
// than the narrowest overlap a read page can have with the rectangle.
constexpr uint32 kReadPointsPerSide = 9u;
// The widening of a body's rectangle, in its pages: more than a sample's direction moves under the
// relief and the plane's distortion together.
constexpr float64 kReadMarginPages = 0.5;
// How far the window reaches from its point along each face axis, in widths of the point's page.
constexpr float64 kWindowReachPages = 0.5;
// How far from a face edge the window samples each side of it, in samples: more than the float32
// error of a direction on a 600 km planet (about 0.1 sample).
constexpr float64 kWindowEdgeOffsetSamples = 1.0;

// The direction of level-0 sample coordinate (sampleX, sampleZ) of `face`, the coordinates possibly
// between samples.
Mathematics::Vector3 SampleDirection(uint8 face, float64 sampleX, float64 sampleZ, uint32 faceSamples)
{
    const float64 last = static_cast<float64>(faceSamples - 1u);
    const float64 equalAngleX = 2.0 * sampleX / last - 1.0;
    const float64 equalAngleZ = 2.0 * sampleZ / last - 1.0;
    return PageStreaming::GnomonicFaceDirection(face, PageStreaming::GnomonicFaceCoordinate(equalAngleX),
                                                PageStreaming::GnomonicFaceCoordinate(equalAngleZ));
}

// The face's X axis: the world axis its sample X runs along.
Mathematics::Vector3 FaceAxisX(uint8 face)
{
    const Mathematics::CubeFaceAxes& axes = Mathematics::kCubeFaceAxes[face];
    Mathematics::Vector3 axis;
    axis[axes.XAxis] = static_cast<float32>(axes.XSign);
    return axis;
}

// The samples page `page` owns along an axis of a face of `faceSamples` samples a side: from its first
// owned sample to the first sample of the next page, clamped to the face's last sample.
void OwnedSampleRange(uint32 page, uint32 faceSamples, float64& low, float64& high)
{
    constexpr float64 kOwned = PageStreaming::kPageOwnedSamples;
    const float64 last = static_cast<float64>(faceSamples - 1u);
    low = std::min(last, page * kOwned);
    high = std::min(last, (page + 1u) * kOwned);
}

// A block of level-0 sample coordinates of one face, ends included.
struct SampleRange
{
    float64 LowX = 0.0;
    float64 HighX = 0.0;
    float64 LowZ = 0.0;
    float64 HighZ = 0.0;
};

// The chart's owned sample range along X and Z.
SampleRange ChartSampleRange(const PlanetChartLayout& layout, const PlanetChart& chart)
{
    SampleRange range;
    float64 unused = 0.0;
    OwnedSampleRange(chart.FirstPageX, layout.FaceSamples, range.LowX, unused);
    OwnedSampleRange(chart.FirstPageX + chart.PagesX - 1u, layout.FaceSamples, unused, range.HighX);
    OwnedSampleRange(chart.FirstPageZ, layout.FaceSamples, range.LowZ, unused);
    OwnedSampleRange(chart.FirstPageZ + chart.PagesZ - 1u, layout.FaceSamples, unused, range.HighZ);
    return range;
}

// The bounding rectangle, in `chart`'s plane, of the boundary of sample block `range` projected along the
// chart's Up. Away from the face's axes the sample grid is sheared in the plane, so a block's rectangle is
// measured from its own boundary, never cut from a larger block's rectangle.
PlanetPageBodyRect ProjectedSampleRect(const PlanetChartLayout& layout, float64 referenceRadius,
                                       const PlanetChart& chart, const SampleRange& range)
{
    float64 minX = std::numeric_limits<float64>::max();
    float64 minZ = std::numeric_limits<float64>::max();
    float64 maxX = std::numeric_limits<float64>::lowest();
    float64 maxZ = std::numeric_limits<float64>::lowest();
    for (uint32 point = 0; point < kBoundPointsPerSide; ++point)
    {
        const float64 along = static_cast<float64>(point) / static_cast<float64>(kBoundPointsPerSide - 1u);
        const float64 sideX = range.LowX + along * (range.HighX - range.LowX);
        const float64 sideZ = range.LowZ + along * (range.HighZ - range.LowZ);
        for (const auto& [sampleX, sampleZ] : {std::pair{sideX, range.LowZ}, std::pair{sideX, range.HighZ},
                                               std::pair{range.LowX, sideZ}, std::pair{range.HighX, sideZ}})
        {
            const Mathematics::Vector3 direction = SampleDirection(chart.Face, sampleX, sampleZ, layout.FaceSamples);
            const float64 toPlane = referenceRadius / Mathematics::Vector3::Dot(direction, chart.Up);
            const float64 x = toPlane * Mathematics::Vector3::Dot(direction, chart.TangentX);
            const float64 z = toPlane * Mathematics::Vector3::Dot(direction, chart.TangentZ);
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minZ = std::min(minZ, z);
            maxZ = std::max(maxZ, z);
        }
    }
    return PlanetPageBodyRect{minX, minZ, maxX - minX, maxZ - minZ};
}

// The level-0 page that owns sample coordinate `sample` along a face axis.
uint32 PageOfSample(const PlanetChartLayout& layout, float64 sample)
{
    return std::min(layout.PagesPerSide - 1u, static_cast<uint32>(std::max(0.0, sample) / PageStreaming::kPageOwnedSamples));
}

// The level-0 page under face point `point`.
PageStreaming::PageAddress PageOfFacePoint(const PlanetChartLayout& layout, const PageStreaming::PageFacePoint& point)
{
    return PageStreaming::PageAddress{point.Face, 0, PageOfSample(layout, point.X), PageOfSample(layout, point.Z)};
}

// The level-0 page under planet-local direction `direction` (any length).
PageStreaming::PageAddress PageUnder(const PlanetChartLayout& layout, const Mathematics::Vector3& direction)
{
    return PageOfFacePoint(layout, PageStreaming::EqualAngleFacePointOf(direction, layout.FaceSamples));
}

// The sample coordinates of the window's square along one face axis: the point and its reach on each
// side, and the coordinates just inside and just outside each face edge the reach crosses.
struct WindowAxis
{
    std::array<float64, 7> Samples{};
    uint32 Count = 0;

    void Build(float64 point, float64 reach, uint32 faceSamples)
    {
        Samples[Count++] = point - reach;
        Samples[Count++] = point;
        Samples[Count++] = point + reach;
        for (const float64 edge : {0.0, static_cast<float64>(faceSamples - 1u)})
            if (point - reach < edge && edge < point + reach)
            {
                Samples[Count++] = edge - kWindowEdgeOffsetSamples;
                Samples[Count++] = edge + kWindowEdgeOffsetSamples;
            }
    }
};

// The bounds, in one face's sample space, of the window samples that lie on that face.
struct FaceSampleBox
{
    bool Used = false;
    float64 MinX = 0.0;
    float64 MaxX = 0.0;
    float64 MinZ = 0.0;
    float64 MaxZ = 0.0;

    void Add(float64 x, float64 z)
    {
        MinX = Used ? std::min(MinX, x) : x;
        MaxX = Used ? std::max(MaxX, x) : x;
        MinZ = Used ? std::min(MinZ, z) : z;
        MaxZ = Used ? std::max(MaxZ, z) : z;
        Used = true;
    }
};

void AppendOnce(const PageStreaming::PageAddress& page, std::vector<PageStreaming::PageAddress>& out)
{
    if (std::find(out.begin(), out.end(), page) == out.end())
        out.push_back(page);
}

// The planet-local point of plane point (x, z) of `chart` on a planet of radius `referenceRadius`.
Mathematics::Vector3 PlanePoint(const PlanetChart& chart, float64 referenceRadius, float64 x, float64 z)
{
    return chart.Up * static_cast<float32>(referenceRadius) + chart.TangentX * static_cast<float32>(x) +
           chart.TangentZ * static_cast<float32>(z);
}

} // namespace

PlanetChartLayout ComputePlanetChartLayout(float64 referenceRadius, uint32 faceSamples)
{
    PlanetChartLayout layout;
    layout.FaceSamples = faceSamples;
    layout.PagesPerSide = (faceSamples + PageStreaming::kPageOwnedSamples - 2u) / PageStreaming::kPageOwnedSamples;
    const float64 pageAngle =
        0.5 * std::numbers::pi * PageStreaming::kPageOwnedSamples / static_cast<float64>(faceSamples - 1u);
    const float64 maxAngle = std::min(kChartMaxSpanMeters / referenceRadius, kChartMaxAngle);
    while ((1u << (layout.Shift + 1u)) <= layout.PagesPerSide &&
           pageAngle * static_cast<float64>(1u << (layout.Shift + 1u)) <= maxAngle)
        ++layout.Shift;
    layout.ChartsPerSide = (layout.PagesPerSide + (1u << layout.Shift) - 1u) >> layout.Shift;
    return layout;
}

PlanetChart MakePlanetChartOfPage(const PlanetChartLayout& layout, float64 referenceRadius, uint8 face, uint32 pageX,
                                  uint32 pageZ)
{
    PlanetChart chart;
    chart.Face = face;
    chart.X = pageX >> layout.Shift;
    chart.Z = pageZ >> layout.Shift;
    chart.FirstPageX = chart.X << layout.Shift;
    chart.FirstPageZ = chart.Z << layout.Shift;
    chart.PagesX = std::min(layout.PagesPerSide, chart.FirstPageX + (1u << layout.Shift)) - chart.FirstPageX;
    chart.PagesZ = std::min(layout.PagesPerSide, chart.FirstPageZ + (1u << layout.Shift)) - chart.FirstPageZ;

    const SampleRange range = ChartSampleRange(layout, chart);
    chart.Up = SampleDirection(face, 0.5 * (range.LowX + range.HighX), 0.5 * (range.LowZ + range.HighZ),
                               layout.FaceSamples);
    const Mathematics::Vector3 axisX = FaceAxisX(face);
    chart.TangentX = (axisX - chart.Up * Mathematics::Vector3::Dot(axisX, chart.Up)).Normalize();
    chart.TangentZ = Mathematics::Vector3::Cross(chart.TangentX, chart.Up);

    const PlanetPageBodyRect rect = ProjectedSampleRect(layout, referenceRadius, chart, range);
    chart.PlaneMinX = rect.MinX;
    chart.PlaneMinZ = rect.MinZ;
    chart.PlaneSizeX = rect.SizeX;
    chart.PlaneSizeZ = rect.SizeZ;
    return chart;
}

PlanetPageBodyRect PlanetPageBodyRectOf(const PlanetChartLayout& layout, float64 referenceRadius,
                                        const PlanetChart& chart, uint32 pageX, uint32 pageZ)
{
    SampleRange page;
    OwnedSampleRange(pageX, layout.FaceSamples, page.LowX, page.HighX);
    OwnedSampleRange(pageZ, layout.FaceSamples, page.LowZ, page.HighZ);
    return ProjectedSampleRect(layout, referenceRadius, chart, page);
}

void AppendPlanetWindowPages(const PlanetChartLayout& layout, const Mathematics::Vector3& point,
                             std::vector<PageStreaming::PageAddress>& out)
{
    if (!(Mathematics::Vector3::Dot(point, point) > 0.0f))
        return;
    const PageStreaming::PageFacePoint facePoint = PageStreaming::EqualAngleFacePointOf(point, layout.FaceSamples);
    const PageStreaming::PageAddress own = PageOfFacePoint(layout, facePoint);
    float64 low = 0.0;
    float64 high = 0.0;
    OwnedSampleRange(own.X, layout.FaceSamples, low, high);
    WindowAxis axisX;
    axisX.Build(facePoint.X, kWindowReachPages * (high - low), layout.FaceSamples);
    OwnedSampleRange(own.Z, layout.FaceSamples, low, high);
    WindowAxis axisZ;
    axisZ.Build(facePoint.Z, kWindowReachPages * (high - low), layout.FaceSamples);

    // The reached square is sampled in the point's face sample space. Past a cube edge a sample's
    // direction lies on the next face, where the square is sheared: the corners of each face's part
    // bound it in that face's own sample space.
    std::array<FaceSampleBox, Mathematics::kCubeFaceCount> boxes{};
    for (uint32 indexZ = 0; indexZ < axisZ.Count; ++indexZ)
        for (uint32 indexX = 0; indexX < axisX.Count; ++indexX)
        {
            const Mathematics::Vector3 direction =
                SampleDirection(own.Face, axisX.Samples[indexX], axisZ.Samples[indexZ], layout.FaceSamples);
            const PageStreaming::PageFacePoint reached =
                PageStreaming::EqualAngleFacePointOf(direction, layout.FaceSamples);
            boxes[reached.Face].Add(reached.X, reached.Z);
        }
    AppendOnce(own, out);
    for (uint32 step = 0; step < Mathematics::kCubeFaceCount; ++step)
    {
        const uint8 face = static_cast<uint8>((own.Face + step) % Mathematics::kCubeFaceCount);
        const FaceSampleBox& box = boxes[face];
        if (!box.Used)
            continue;
        for (uint32 pageZ = PageOfSample(layout, box.MinZ); pageZ <= PageOfSample(layout, box.MaxZ); ++pageZ)
            for (uint32 pageX = PageOfSample(layout, box.MinX); pageX <= PageOfSample(layout, box.MaxX); ++pageX)
                AppendOnce(PageStreaming::PageAddress{face, 0, pageX, pageZ}, out);
    }
}

void AppendPlanetPageBodyReads(const PlanetChartLayout& layout, float64 referenceRadius, uint8 face, uint32 pageX,
                               uint32 pageZ, std::vector<PageStreaming::PageAddress>& out)
{
    const PlanetChart chart = MakePlanetChartOfPage(layout, referenceRadius, face, pageX, pageZ);
    const PlanetPageBodyRect rect = PlanetPageBodyRectOf(layout, referenceRadius, chart, pageX, pageZ);
    const float64 marginX = kReadMarginPages * chart.PlaneSizeX / chart.PagesX;
    const float64 marginZ = kReadMarginPages * chart.PlaneSizeZ / chart.PagesZ;
    for (uint32 row = 0; row < kReadPointsPerSide; ++row)
        for (uint32 column = 0; column < kReadPointsPerSide; ++column)
        {
            const float64 alongX = static_cast<float64>(column) / (kReadPointsPerSide - 1u);
            const float64 alongZ = static_cast<float64>(row) / (kReadPointsPerSide - 1u);
            const float64 x = rect.MinX - marginX + alongX * (rect.SizeX + 2.0 * marginX);
            const float64 z = rect.MinZ - marginZ + alongZ * (rect.SizeZ + 2.0 * marginZ);
            AppendOnce(PageUnder(layout, PlanePoint(chart, referenceRadius, x, z)), out);
        }
}

PlanetPageTexel LocatePlanetPageTexel(uint32 faceSamples, const PageStreaming::PageFacePoint& point, uint32 level)
{
    constexpr uint32 kOwned = PageStreaming::kPageOwnedSamples;
    assert(faceSamples >= 2u && level < 32u);
    const uint32 cells = (faceSamples - 1u) >> level;
    const uint32 lastPage = std::max(1u, (cells + kOwned - 1u) / kOwned) - 1u;
    const float64 scale = std::ldexp(1.0, -static_cast<int32>(level));
    const float64 x = std::max(0.0, point.X * scale);
    const float64 z = std::max(0.0, point.Z * scale);
    const uint32 pageX = std::min(lastPage, static_cast<uint32>(x / kOwned));
    const uint32 pageZ = std::min(lastPage, static_cast<uint32>(z / kOwned));
    return PlanetPageTexel{PageStreaming::PageAddress{point.Face, static_cast<uint8>(level), pageX, pageZ},
                           x - static_cast<float64>(pageX * kOwned) + PageStreaming::kPageApronSamples,
                           z - static_cast<float64>(pageZ * kOwned) + PageStreaming::kPageApronSamples};
}

float64 SamplePlanetPageTexel(std::span<const float32> heights, const PlanetPageTexel& texel)
{
    constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
    assert(heights.size() == PageStreaming::kPageSampleCount);
    const uint32 lowX = std::min(kStride - 2u, static_cast<uint32>(texel.X));
    const uint32 lowZ = std::min(kStride - 2u, static_cast<uint32>(texel.Z));
    // lowX and lowZ are at most the texel, so a fraction is never negative; the top bound holds the
    // lattice's last cell.
    const float64 fractionX = std::min(texel.X - lowX, 1.0);
    const float64 fractionZ = std::min(texel.Z - lowZ, 1.0);
    const float32* row0 = heights.data() + static_cast<std::size_t>(lowZ) * kStride + lowX;
    const float32* row1 = row0 + kStride;
    const float64 nearRow = row0[0] + fractionX * (row0[1] - row0[0]);
    const float64 farRow = row1[0] + fractionX * (row1[1] - row1[0]);
    return nearRow + fractionZ * (farRow - nearRow);
}

} // namespace GameEngine::TerrainECS
