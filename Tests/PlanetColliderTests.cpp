#include <gtest/gtest.h>

#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "TerrainECS/Planet/PlanetChart.h"
#include "Planet/PlanetPageBodyFill.h"
#include "TerrainECS/SurfaceHeightQuery.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Oracles for the planet page body fill (design page-streaming-planets-r4-colliders 6): each sample of
// a body, placed in its chart frame, lies on the surface the pages draw.

using namespace GameEngine;
using namespace GameEngine::TerrainECS;

namespace
{

// The 2 km default planet: radius and level-0 samples a face side.
constexpr float64 kRadius = 2000.0;
constexpr uint32 kFaceSamples = 6337;
// A body one page off its chart's center, on face +Z, away from the face edges.
constexpr PageStreaming::PageAddress kBodyPage{4, 0, 24, 25};
// The drawn surface may differ from a body sample by this much, in meters.
constexpr float64 kSurfaceTolerance = 0.01;

// Page heights linear in the face lattice coordinates: the bilinear page read reproduces them exactly,
// so the drawn surface is known at any direction.
struct LatticeHeights
{
    float64 Base = 0.0;
    float64 PerX = 0.0;
    float64 PerZ = 0.0;

    float64 At(float64 x, float64 z) const
    {
        constexpr float64 kCenterSample = 0.5 * (kFaceSamples - 1);
        return Base + PerX * (x - kCenterSample) + PerZ * (z - kCenterSample);
    }
};

PlanetPageBodyInput MakeBodyInput(const LatticeHeights& heights)
{
    PlanetPageBodyInput input;
    input.Layout = ComputePlanetChartLayout(kRadius, kFaceSamples);
    input.ReferenceRadius = kRadius;
    input.Page = kBodyPage;
    std::vector<PageStreaming::PageAddress> reads;
    AppendPlanetPageBodyReads(input.Layout, kRadius, kBodyPage.Face, kBodyPage.X, kBodyPage.Z, reads);
    constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
    constexpr uint32 kOwned = PageStreaming::kPageOwnedSamples;
    constexpr uint32 kApron = PageStreaming::kPageApronSamples;
    for (const PageStreaming::PageAddress& address : reads)
    {
        PlanetPageBodyPage& page = input.Pages.emplace_back();
        page.Address = address;
        page.Heights.resize(PageStreaming::kPageSampleCount);
        for (uint32 z = 0; z < kStride; ++z)
            for (uint32 x = 0; x < kStride; ++x)
            {
                const float64 faceX = static_cast<float64>(address.X * kOwned + x) - kApron;
                const float64 faceZ = static_cast<float64>(address.Z * kOwned + z) - kApron;
                page.Heights[static_cast<size_t>(z) * kStride + x] = static_cast<float32>(heights.At(faceX, faceZ));
            }
    }
    return input;
}

// A planet-local point in float64.
struct WidePoint
{
    float64 X = 0.0;
    float64 Y = 0.0;
    float64 Z = 0.0;

    float64 Length() const { return std::sqrt(X * X + Y * Y + Z * Z); }
};

// A body sample's planet-local point: the plane point of grid sample (column, row) moved `height`
// meters along the chart's Up.
WidePoint BodySamplePoint(const PlanetChart& chart, const PlanetPageBodyRect& rect, uint32 column, uint32 row,
                          float64 height)
{
    constexpr float64 kLast = kFinestPageGridSamples - 1u;
    const float64 x = rect.MinX + rect.SizeX * column / kLast;
    const float64 z = rect.MinZ + rect.SizeZ * row / kLast;
    const float64 along = kRadius + height;
    return {chart.Up.x * along + chart.TangentX.x * x + chart.TangentZ.x * z,
            chart.Up.y * along + chart.TangentX.y * x + chart.TangentZ.y * z,
            chart.Up.z * along + chart.TangentX.z * x + chart.TangentZ.z * z};
}

std::vector<float32> FillBody(const PlanetPageBodyInput& input)
{
    std::vector<float32> samples(static_cast<size_t>(kFinestPageGridSamples) * kFinestPageGridSamples);
    EXPECT_TRUE(FillPlanetPageBody(input, samples));
    return samples;
}

bool IsBodyPage(const PlanetPageBodyPage& page)
{
    return page.Address == kBodyPage;
}

} // namespace

// A planet of constant height H draws a sphere of radius R + H: every body sample lies on it.
TEST(PlanetPageBody, SamplesLieOnTheSphereOfAConstantHeight)
{
    constexpr float64 kHeight = 35.0;
    const PlanetPageBodyInput input = MakeBodyInput(LatticeHeights{kHeight, 0.0, 0.0});
    const std::vector<float32> samples = FillBody(input);
    const PlanetChart chart = MakePlanetChartOfPage(input.Layout, kRadius, kBodyPage.Face, kBodyPage.X, kBodyPage.Z);
    const PlanetPageBodyRect rect = PlanetPageBodyRectOf(input.Layout, kRadius, chart, kBodyPage.X, kBodyPage.Z);
    float64 worst = 0.0;
    for (uint32 row = 0; row < kFinestPageGridSamples; ++row)
        for (uint32 column = 0; column < kFinestPageGridSamples; ++column)
        {
            const float64 height = samples[static_cast<size_t>(row) * kFinestPageGridSamples + column];
            const float64 length = BodySamplePoint(chart, rect, column, row, height).Length();
            worst = std::max(worst, std::abs(length - kRadius - kHeight));
        }
    EXPECT_LT(worst, kSurfaceTolerance) << "worst distance from the sphere of radius R + H, meters";
}

// On a sloped surface, each body sample lies on the drawn surface at its own direction within 1 cm: the
// height at a plane point is read where the sample's line meets the surface, not under the plane point.
TEST(PlanetPageBody, SamplesLieOnTheDrawnSurfaceWithinOneCentimeter)
{
    const LatticeHeights heights{40.0, 0.12, -0.08};
    const PlanetPageBodyInput input = MakeBodyInput(heights);
    const std::vector<float32> samples = FillBody(input);
    const PlanetChart chart = MakePlanetChartOfPage(input.Layout, kRadius, kBodyPage.Face, kBodyPage.X, kBodyPage.Z);
    const PlanetPageBodyRect rect = PlanetPageBodyRectOf(input.Layout, kRadius, chart, kBodyPage.X, kBodyPage.Z);
    float64 worst = 0.0;
    for (uint32 row = 0; row < kFinestPageGridSamples; ++row)
        for (uint32 column = 0; column < kFinestPageGridSamples; ++column)
        {
            const float64 height = samples[static_cast<size_t>(row) * kFinestPageGridSamples + column];
            const WidePoint point = BodySamplePoint(chart, rect, column, row, height);
            const float64 length = point.Length();
            const Mathematics::Vector3 direction(static_cast<float32>(point.X / length),
                                                 static_cast<float32>(point.Y / length),
                                                 static_cast<float32>(point.Z / length));
            const PageStreaming::PageFacePoint facePoint = PageStreaming::EqualAngleFacePointOf(direction, kFaceSamples);
            ASSERT_EQ(facePoint.Face, kBodyPage.Face);
            worst = std::max(worst, std::abs(length - kRadius - heights.At(facePoint.X, facePoint.Z)));
        }
    EXPECT_LT(worst, kSurfaceTolerance) << "worst distance from the drawn surface, meters";
}

// A body whose reads lack a page it needs is not filled.
TEST(PlanetPageBody, FillRefusesAMissingPage)
{
    PlanetPageBodyInput input = MakeBodyInput(LatticeHeights{10.0, 0.0, 0.0});
    std::erase_if(input.Pages, IsBodyPage);
    std::vector<float32> samples(static_cast<size_t>(kFinestPageGridSamples) * kFinestPageGridSamples);
    EXPECT_FALSE(FillPlanetPageBody(input, samples));
}
