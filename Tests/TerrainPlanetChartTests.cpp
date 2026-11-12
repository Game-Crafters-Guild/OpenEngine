// The collider charts of a planet (design page-streaming-planets 3.10): the page-to-chart map, the
// chart's angular span against its cap, its frame, and the cover of two neighbor charts' planes.

#include "TerrainECS/Planet/PlanetChart.h"

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;

namespace
{

// The 2 km component default and a 600 km planet at a 0.5 m level-0 texel (14,720 pages a side).
constexpr float64 kSmallRadius = 2000.0;
constexpr uint32 kSmallFaceSamples = 6337u;
constexpr float64 kLargeRadius = 600000.0;
constexpr uint32 kLargeFaceSamples = 14720u * PageStreaming::kPageOwnedSamples + 1u;

constexpr float64 kChartCapMeters = 2048.0;
constexpr float64 kChartCapAngle = 10.0 * std::numbers::pi / 180.0;
// float32 frame vectors: unit length, orthogonality and determinant hold to this.
constexpr float32 kFrameTolerance = 1.0e-5f;
// The plane cover is computed through float32 frame vectors: a point lies inside a rectangle when it
// is no further out than this, in meters.
constexpr float64 kCoverToleranceMeters = 1.0e-3;
constexpr uint32 kBoundaryPoints = 33u;

// The angle one level-0 page spans along a face's side.
float64 PageAngle(uint32 faceSamples)
{
    return 0.5 * std::numbers::pi * PageStreaming::kPageOwnedSamples / static_cast<float64>(faceSamples - 1u);
}

// The direction of level-0 sample coordinate (sampleX, sampleZ) of `face`, between samples allowed.
Mathematics::Vector3 DirectionAt(uint8 face, float64 sampleX, float64 sampleZ, uint32 faceSamples)
{
    const float64 last = static_cast<float64>(faceSamples - 1u);
    return PageStreaming::GnomonicFaceDirection(face, PageStreaming::GnomonicFaceCoordinate(2.0 * sampleX / last - 1.0),
                                                PageStreaming::GnomonicFaceCoordinate(2.0 * sampleZ / last - 1.0));
}

// The chart that owns level-0 sample point `point` (a canonical face point).
PlanetChart ChartOfPoint(const PlanetChartLayout& layout, float64 radius, const PageStreaming::PageFacePoint& point)
{
    constexpr float64 kOwned = PageStreaming::kPageOwnedSamples;
    const uint32 last = layout.PagesPerSide - 1u;
    return MakePlanetChartOfPage(layout, radius, point.Face, std::min(last, static_cast<uint32>(point.X / kOwned)),
                                 std::min(last, static_cast<uint32>(point.Z / kOwned)));
}

// How far direction `direction` projected onto `chart`'s plane lies outside the chart's rectangle, in
// meters; zero or less inside.
float64 DistanceOutsidePlane(const PlanetChart& chart, float64 radius, const Mathematics::Vector3& direction)
{
    const float64 toPlane = radius / Mathematics::Vector3::Dot(direction, chart.Up);
    const float64 x = toPlane * Mathematics::Vector3::Dot(direction, chart.TangentX);
    const float64 z = toPlane * Mathematics::Vector3::Dot(direction, chart.TangentZ);
    return std::max({chart.PlaneMinX - x, x - (chart.PlaneMinX + chart.PlaneSizeX), chart.PlaneMinZ - z,
                     z - (chart.PlaneMinZ + chart.PlaneSizeZ)});
}

// About this many pages a side, the last page included, keep the check fast on a large planet.
constexpr uint32 kCheckedPagesPerSide = 24u;

void ExpectChartIsAShiftOfItsPages(const PlanetChartLayout& layout, float64 radius)
{
    const uint32 step = std::max(1u, layout.PagesPerSide / kCheckedPagesPerSide) | 1u;
    std::vector<uint32> pages;
    for (uint32 page = 0; page < layout.PagesPerSide; page += step)
        pages.push_back(page);
    pages.push_back(layout.PagesPerSide - 1u);
    for (uint8 face = 0; face < 6u; ++face)
        for (const uint32 pageZ : pages)
            for (const uint32 pageX : pages)
            {
                const PlanetChart chart = MakePlanetChartOfPage(layout, radius, face, pageX, pageZ);
                ASSERT_EQ(chart.Face, face);
                ASSERT_EQ(chart.X, pageX >> layout.Shift);
                ASSERT_EQ(chart.Z, pageZ >> layout.Shift);
                ASSERT_EQ(chart.FirstPageX, chart.X << layout.Shift);
                ASSERT_EQ(chart.FirstPageZ, chart.Z << layout.Shift);
                ASSERT_EQ(chart.PagesX, std::min(1u << layout.Shift, layout.PagesPerSide - chart.FirstPageX));
                ASSERT_EQ(chart.PagesZ, std::min(1u << layout.Shift, layout.PagesPerSide - chart.FirstPageZ));
            }
}

void ExpectChartSpanIsTheLargestWithinTheCap(const PlanetChartLayout& layout, float64 radius, uint32 faceSamples)
{
    const float64 cap = std::min(kChartCapMeters / radius, kChartCapAngle);
    const float64 span = PageAngle(faceSamples) * static_cast<float64>(1u << layout.Shift);
    EXPECT_LE(span, cap);
    const bool nextFitsTheFace = (2u << layout.Shift) <= layout.PagesPerSide;
    EXPECT_TRUE(!nextFitsTheFace || 2.0 * span > cap) << "a chart twice as wide still fits the cap";
    EXPECT_EQ(layout.ChartsPerSide, (layout.PagesPerSide + (1u << layout.Shift) - 1u) >> layout.Shift);
    // The plane holds the chart's arc on the reference sphere: at most the gnomonic stretch of the cap.
    const PlanetChart chart = MakePlanetChartOfPage(layout, radius, 2u, 0u, 0u);
    const float64 planeCap = 2.0 * radius * std::tan(cap) * 1.01;
    EXPECT_LE(chart.PlaneSizeX, planeCap);
    EXPECT_LE(chart.PlaneSizeZ, planeCap);
}

void ExpectFramesAreRotationsUpAtTheChartCenter(const PlanetChartLayout& layout, float64 radius)
{
    constexpr float64 kOwned = PageStreaming::kPageOwnedSamples;
    const float64 last = static_cast<float64>(layout.FaceSamples - 1u);
    const uint32 lastChart = layout.ChartsPerSide - 1u;
    for (uint8 face = 0; face < 6u; ++face)
        for (const uint32 chartZ : {0u, lastChart / 2u, lastChart})
            for (const uint32 chartX : {0u, lastChart / 3u, lastChart})
            {
                const PlanetChart chart =
                    MakePlanetChartOfPage(layout, radius, face, chartX << layout.Shift, chartZ << layout.Shift);
                const float64 centerX =
                    0.5 * (chart.FirstPageX * kOwned + std::min(last, (chart.FirstPageX + chart.PagesX) * kOwned));
                const float64 centerZ =
                    0.5 * (chart.FirstPageZ * kOwned + std::min(last, (chart.FirstPageZ + chart.PagesZ) * kOwned));
                const Mathematics::Vector3 center = DirectionAt(face, centerX, centerZ, layout.FaceSamples);
                EXPECT_NEAR((chart.Up - center).Length(), 0.0f, kFrameTolerance);
                EXPECT_NEAR(chart.TangentX.Length(), 1.0f, kFrameTolerance);
                EXPECT_NEAR(chart.Up.Length(), 1.0f, kFrameTolerance);
                EXPECT_NEAR(chart.TangentZ.Length(), 1.0f, kFrameTolerance);
                EXPECT_NEAR(Mathematics::Vector3::Dot(chart.TangentX, chart.Up), 0.0f, kFrameTolerance);
                EXPECT_NEAR(Mathematics::Vector3::Dot(chart.TangentZ, chart.Up), 0.0f, kFrameTolerance);
                EXPECT_NEAR(Mathematics::Vector3::Dot(chart.TangentX, chart.TangentZ), 0.0f, kFrameTolerance);
                // det(TangentX, Up, TangentZ) = (TangentX x Up) . TangentZ.
                const float32 determinant =
                    Mathematics::Vector3::Dot(Mathematics::Vector3::Cross(chart.TangentX, chart.Up), chart.TangentZ);
                EXPECT_NEAR(determinant, 1.0f, kFrameTolerance)
                    << "face " << int(face) << " chart " << chartX << "," << chartZ;
                // TangentX runs along the face's sample X.
                const Mathematics::Vector3 alongX = DirectionAt(face, centerX + 1.0, centerZ, layout.FaceSamples);
                EXPECT_GT(Mathematics::Vector3::Dot(alongX - center, chart.TangentX), 0.0f);
            }
}

// Every direction on the line of `face` from sample point (lowX, lowZ) to (highX, highZ) lies inside
// both charts' plane rectangles.
void ExpectLineCoveredByBoth(float64 radius, const PlanetChart& first, const PlanetChart& second, uint8 face,
                             float64 lowX, float64 lowZ, float64 highX, float64 highZ, uint32 faceSamples)
{
    for (uint32 point = 0; point < kBoundaryPoints; ++point)
    {
        const float64 t = static_cast<float64>(point) / static_cast<float64>(kBoundaryPoints - 1u);
        const Mathematics::Vector3 direction =
            DirectionAt(face, lowX + t * (highX - lowX), lowZ + t * (highZ - lowZ), faceSamples);
        EXPECT_LE(DistanceOutsidePlane(first, radius, direction), kCoverToleranceMeters)
            << "face " << int(first.Face) << " chart " << first.X << "," << first.Z << " point " << point;
        EXPECT_LE(DistanceOutsidePlane(second, radius, direction), kCoverToleranceMeters)
            << "face " << int(second.Face) << " chart " << second.X << "," << second.Z << " point " << point;
    }
}

void ExpectNeighborChartsLeaveNoGap(const PlanetChartLayout& layout, float64 radius)
{
    constexpr float64 kOwned = PageStreaming::kPageOwnedSamples;
    const float64 last = static_cast<float64>(layout.FaceSamples - 1u);
    const uint32 chartPages = 1u << layout.Shift;
    for (uint8 face = 0; face < 6u; ++face)
    {
        // Two charts on one face that share the line at sample X = boundaryX.
        const uint32 chartZ = layout.ChartsPerSide / 2u;
        const PlanetChart left = MakePlanetChartOfPage(layout, radius, face, 0u, chartZ * chartPages);
        const PlanetChart right = MakePlanetChartOfPage(layout, radius, face, chartPages, chartZ * chartPages);
        const float64 boundaryX = chartPages * kOwned;
        const float64 lowZ = left.FirstPageZ * kOwned;
        const float64 highZ = std::min(last, (left.FirstPageZ + left.PagesZ) * kOwned);
        ExpectLineCoveredByBoth(radius, left, right, face, boundaryX, lowZ, boundaryX, highZ, layout.FaceSamples);

        // The face's last chart column and the chart past the cube edge on the next face.
        const PlanetChart edge = MakePlanetChartOfPage(layout, radius, face, layout.PagesPerSide - 1u, chartZ * chartPages);
        const PageStreaming::PageFacePoint pastEdge = PageStreaming::EqualAngleFacePointOf(
            DirectionAt(face, last + 0.5, 0.5 * (lowZ + highZ), layout.FaceSamples), layout.FaceSamples);
        ASSERT_NE(pastEdge.Face, face);
        const PlanetChart across = ChartOfPoint(layout, radius, pastEdge);
        // The part of the cube edge both charts hold: the edge chart's span, clipped to the samples the
        // chart across holds along the edge.
        const float64 acrossLowX = across.FirstPageX * kOwned;
        const float64 acrossHighX = std::min(last, (across.FirstPageX + across.PagesX) * kOwned);
        const float64 acrossLowZ = across.FirstPageZ * kOwned;
        const float64 acrossHighZ = std::min(last, (across.FirstPageZ + across.PagesZ) * kOwned);
        uint32 checked = 0;
        for (uint32 point = 0; point < kBoundaryPoints; ++point)
        {
            const float64 t = static_cast<float64>(point) / static_cast<float64>(kBoundaryPoints - 1u);
            const Mathematics::Vector3 direction = DirectionAt(face, last, lowZ + t * (highZ - lowZ), layout.FaceSamples);
            // The edge point half a sample onto the face across, to find whether the chart across holds it.
            const Mathematics::Vector3 nudged = DirectionAt(face, last + 0.5, lowZ + t * (highZ - lowZ), layout.FaceSamples);
            const PageStreaming::PageFacePoint onAcross = PageStreaming::EqualAngleFacePointOf(nudged, layout.FaceSamples);
            if (onAcross.Face != across.Face || onAcross.X < acrossLowX || onAcross.X > acrossHighX ||
                onAcross.Z < acrossLowZ || onAcross.Z > acrossHighZ)
                continue;
            ++checked;
            EXPECT_LE(DistanceOutsidePlane(edge, radius, direction), kCoverToleranceMeters)
                << "face " << int(face) << " edge point " << point;
            EXPECT_LE(DistanceOutsidePlane(across, radius, direction), kCoverToleranceMeters)
                << "face " << int(across.Face) << " across point " << point;
        }
        EXPECT_GT(checked, kBoundaryPoints / 4u) << "the two charts share little of the cube edge on face " << int(face);
    }
}

void ExpectBodyRectsHoldTheirPageCenters(const PlanetChartLayout& layout, float64 radius)
{
    constexpr float64 kOwned = PageStreaming::kPageOwnedSamples;
    const float64 last = static_cast<float64>(layout.FaceSamples - 1u);
    const uint32 step = std::max(1u, layout.PagesPerSide / kCheckedPagesPerSide) | 1u;
    for (uint8 face = 0; face < 6u; ++face)
        for (uint32 pageZ = 0; pageZ < layout.PagesPerSide; pageZ += step)
            for (uint32 pageX = 0; pageX < layout.PagesPerSide; pageX += step)
            {
                const PlanetChart chart = MakePlanetChartOfPage(layout, radius, face, pageX, pageZ);
                const PlanetPageBodyRect rect = PlanetPageBodyRectOf(layout, radius, chart, pageX, pageZ);
                // The middle of the samples the page owns: the last page of a face owns fewer.
                const float64 sampleX = 0.5 * (std::min(last, pageX * kOwned) + std::min(last, (pageX + 1u) * kOwned));
                const float64 sampleZ = 0.5 * (std::min(last, pageZ * kOwned) + std::min(last, (pageZ + 1u) * kOwned));
                const Mathematics::Vector3 direction = DirectionAt(face, sampleX, sampleZ, layout.FaceSamples);
                const float64 toPlane = radius / Mathematics::Vector3::Dot(direction, chart.Up);
                const float64 x = toPlane * Mathematics::Vector3::Dot(direction, chart.TangentX);
                const float64 z = toPlane * Mathematics::Vector3::Dot(direction, chart.TangentZ);
                EXPECT_TRUE(x > rect.MinX && x < rect.MinX + rect.SizeX && z > rect.MinZ && z < rect.MinZ + rect.SizeZ)
                    << "radius " << radius << " m, " << layout.PagesPerSide << " pages a side, face " << int(face)
                    << " page (" << pageX << ", " << pageZ << "): center (" << x << ", " << z
                    << ") outside the body rect at (" << rect.MinX << ", " << rect.MinZ << ") size (" << rect.SizeX
                    << ", " << rect.SizeZ << ") of the chart rect at (" << chart.PlaneMinX << ", " << chart.PlaneMinZ
                    << ") size (" << chart.PlaneSizeX << ", " << chart.PlaneSizeZ << ")";
            }
}

// The pages a body reaches from points near the face's edges and corners: the pages under the point
// moved by up to this part of its page's owned width along either face axis or both.
constexpr float64 kWindowReach = 0.45;
constexpr float64 kWindowFractions[] = {0.1, 0.3, 0.7, 0.9};
// The moves checked along each axis on each side of the point, evenly spaced up to the reach.
constexpr int32 kWindowMoves = 3;
// Away from cube edges a window holds four pages; past an edge the sheared page grid adds one or two.
constexpr size_t kWindowMostPages = 6u;

// The level-0 page under face point `point`, the last page of a face owning fewer samples.
PageStreaming::PageAddress PageOfPoint(const PlanetChartLayout& layout, const PageStreaming::PageFacePoint& point)
{
    constexpr float64 kOwned = PageStreaming::kPageOwnedSamples;
    const uint32 last = layout.PagesPerSide - 1u;
    return PageStreaming::PageAddress{point.Face, 0, std::min(last, static_cast<uint32>(point.X / kOwned)),
                                      std::min(last, static_cast<uint32>(point.Z / kOwned))};
}

// Every point of the edge and corner pages of each face (the first two, the middle and the last two
// pages a side) at kWindowFractions of the page's owned samples: the window holds each page the point
// reaches by kWindowReach of its page along the face's axes, on the next face past a cube edge.
void ExpectWindowsHoldThePagesAPointReaches(const PlanetChartLayout& layout, float64 radius)
{
    constexpr float64 kOwned = PageStreaming::kPageOwnedSamples;
    const float64 lastSample = static_cast<float64>(layout.FaceSamples - 1u);
    const uint32 lastPage = layout.PagesPerSide - 1u;
    const uint32 pages[] = {0u, 1u, layout.PagesPerSide / 2u, lastPage - 1u, lastPage};
    uint32 points = 0;
    uint32 misses = 0;
    size_t largestWindow = 0;
    std::vector<PageStreaming::PageAddress> window;
    for (uint8 face = 0; face < 6u; ++face)
        for (const uint32 pageZ : pages)
            for (const uint32 pageX : pages)
                for (const float64 fractionZ : kWindowFractions)
                    for (const float64 fractionX : kWindowFractions)
                    {
                        const float64 lowX = std::min(lastSample, pageX * kOwned);
                        const float64 widthX = std::min(lastSample, (pageX + 1u) * kOwned) - lowX;
                        const float64 lowZ = std::min(lastSample, pageZ * kOwned);
                        const float64 widthZ = std::min(lastSample, (pageZ + 1u) * kOwned) - lowZ;
                        const float64 sampleX = lowX + fractionX * widthX;
                        const float64 sampleZ = lowZ + fractionZ * widthZ;
                        window.clear();
                        AppendPlanetWindowPages(layout, DirectionAt(face, sampleX, sampleZ, layout.FaceSamples),
                                                window);
                        ++points;
                        largestWindow = std::max(largestWindow, window.size());
                        for (int32 moveZ = -kWindowMoves; moveZ <= kWindowMoves; ++moveZ)
                            for (int32 moveX = -kWindowMoves; moveX <= kWindowMoves; ++moveX)
                            {
                                const float64 reachX = kWindowReach * widthX * moveX / kWindowMoves;
                                const float64 reachZ = kWindowReach * widthZ * moveZ / kWindowMoves;
                                const Mathematics::Vector3 moved =
                                    DirectionAt(face, sampleX + reachX, sampleZ + reachZ, layout.FaceSamples);
                                const PageStreaming::PageAddress reached =
                                    PageOfPoint(layout, PageStreaming::EqualAngleFacePointOf(moved, layout.FaceSamples));
                                if (std::find(window.begin(), window.end(), reached) != window.end())
                                    continue;
                                if (++misses <= 4u)
                                    ADD_FAILURE() << "radius " << radius << " m, face " << int(face) << " page ("
                                                  << pageX << ", " << pageZ << ") at (" << fractionX << ", "
                                                  << fractionZ << ") moved (" << moveX << ", " << moveZ
                                                  << "): the window has no face " << int(reached.Face) << " page ("
                                                  << reached.X << ", " << reached.Z << ")";
                            }
                    }
    EXPECT_EQ(points, 2400u);
    EXPECT_EQ(misses, 0u) << "radius " << radius << " m: of " << points << " points";
    EXPECT_LE(largestWindow, kWindowMostPages) << "radius " << radius << " m";
}

} // namespace

TEST(PlanetChart, APageMapsToItsChartByShifts)
{
    ExpectChartIsAShiftOfItsPages(ComputePlanetChartLayout(kSmallRadius, kSmallFaceSamples), kSmallRadius);
    ExpectChartIsAShiftOfItsPages(ComputePlanetChartLayout(kLargeRadius, kLargeFaceSamples), kLargeRadius);
}

TEST(PlanetChart, TheTwoKilometerDefaultHasFiftyPagesInFourByFourCharts)
{
    const PlanetChartLayout layout = ComputePlanetChartLayout(kSmallRadius, kSmallFaceSamples);
    EXPECT_EQ(layout.PagesPerSide, 50u);
    EXPECT_EQ(layout.Shift, 2u);
    EXPECT_EQ(layout.ChartsPerSide, 13u);
}

TEST(PlanetChart, AChartSpansTheLargestPowerOfTwoPagesWithinTheCap)
{
    ExpectChartSpanIsTheLargestWithinTheCap(ComputePlanetChartLayout(kSmallRadius, kSmallFaceSamples), kSmallRadius,
                                            kSmallFaceSamples);
    ExpectChartSpanIsTheLargestWithinTheCap(ComputePlanetChartLayout(kLargeRadius, kLargeFaceSamples), kLargeRadius,
                                            kLargeFaceSamples);
}

TEST(PlanetChart, TheFrameIsARotationWithUpAtTheChartCenter)
{
    ExpectFramesAreRotationsUpAtTheChartCenter(ComputePlanetChartLayout(kSmallRadius, kSmallFaceSamples), kSmallRadius);
    ExpectFramesAreRotationsUpAtTheChartCenter(ComputePlanetChartLayout(kLargeRadius, kLargeFaceSamples), kLargeRadius);
}

TEST(PlanetChart, NeighborChartPlanesLeaveNoGapAlongTheirSharedBoundary)
{
    ExpectNeighborChartsLeaveNoGap(ComputePlanetChartLayout(kSmallRadius, kSmallFaceSamples), kSmallRadius);
    ExpectNeighborChartsLeaveNoGap(ComputePlanetChartLayout(kLargeRadius, kLargeFaceSamples), kLargeRadius);
}

TEST(PlanetChart, ABodyRectHoldsTheCenterOfItsPage)
{
    ExpectBodyRectsHoldTheirPageCenters(ComputePlanetChartLayout(kSmallRadius, kSmallFaceSamples), kSmallRadius);
    ExpectBodyRectsHoldTheirPageCenters(ComputePlanetChartLayout(kLargeRadius, kLargeFaceSamples), kLargeRadius);
}

TEST(PlanetChart, TheWindowHoldsThePageUnderThePointAndItsNeighborsOnTheNearerSides)
{
    // Sample 3,176 of the 2 km face lies in page 24, past its middle on both axes: the window takes
    // pages 24 and 25 on each axis.
    const PlanetChartLayout layout = ComputePlanetChartLayout(kSmallRadius, kSmallFaceSamples);
    constexpr uint8 kFace = 2u;
    constexpr float64 kSample = 3176.0;
    std::vector<PageStreaming::PageAddress> window;
    AppendPlanetWindowPages(layout, DirectionAt(kFace, kSample, kSample, kSmallFaceSamples), window);
    const std::vector<PageStreaming::PageAddress> expected{
        {kFace, 0, 24, 24}, {kFace, 0, 25, 24}, {kFace, 0, 24, 25}, {kFace, 0, 25, 25}};
    ASSERT_EQ(window.size(), expected.size());
    EXPECT_EQ(window.front(), expected.front()) << "the page under the point comes first";
    for (const PageStreaming::PageAddress& page : expected)
        EXPECT_NE(std::find(window.begin(), window.end(), page), window.end())
            << "page (" << page.X << ", " << page.Z << ") is not in the window";
}

TEST(PlanetChart, TheWindowHoldsThePagesAPointReachesByLessThanHalfAPageNearFaceEdgesAndCorners)
{
    ExpectWindowsHoldThePagesAPointReaches(ComputePlanetChartLayout(kSmallRadius, kSmallFaceSamples), kSmallRadius);
    ExpectWindowsHoldThePagesAPointReaches(ComputePlanetChartLayout(kLargeRadius, kLargeFaceSamples), kLargeRadius);
}
