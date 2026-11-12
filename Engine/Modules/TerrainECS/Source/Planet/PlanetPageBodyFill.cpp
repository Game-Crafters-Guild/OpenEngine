#include "Planet/PlanetPageBodyFill.h"

#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "TerrainECS/SurfaceHeightQuery.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace GameEngine::TerrainECS
{
namespace
{

// Steps of the fixed point that finds where a plane sample's line meets the surface: the height moves
// by millimeters after the first step on relief a body can stand on.
constexpr uint32 kSurfaceSteps = 3u;

struct Vector3Wide
{
    float64 X = 0.0;
    float64 Y = 0.0;
    float64 Z = 0.0;
};

Vector3Wide Widen(const Mathematics::Vector3& v)
{
    return {v.x, v.y, v.z};
}

// The drawn surface's height (meters above the reference radius) in direction `direction`, or false
// when the page under it is not in `input`.
bool SurfaceHeight(const PlanetPageBodyInput& input, const Mathematics::Vector3& direction, float64& height)
{
    const PageStreaming::PageFacePoint point = PageStreaming::EqualAngleFacePointOf(direction, input.Layout.FaceSamples);
    const PlanetPageTexel texel = LocatePlanetPageTexel(input.Layout.FaceSamples, point, 0u);
    const auto page = std::find_if(input.Pages.begin(), input.Pages.end(), [&texel](const PlanetPageBodyPage& candidate) {
        return candidate.Address == texel.Address;
    });
    if (page == input.Pages.end())
        return false;
    height = SamplePlanetPageTexel(page->Heights, texel);
    return true;
}

} // namespace

bool FillPlanetPageBody(const PlanetPageBodyInput& input, std::span<float32> samples)
{
    constexpr uint32 kGrid = kFinestPageGridSamples;
    assert(samples.size() == static_cast<std::size_t>(kGrid) * kGrid);
    const PlanetChart chart =
        MakePlanetChartOfPage(input.Layout, input.ReferenceRadius, input.Page.Face, input.Page.X, input.Page.Z);
    const PlanetPageBodyRect rect =
        PlanetPageBodyRectOf(input.Layout, input.ReferenceRadius, chart, input.Page.X, input.Page.Z);
    const Vector3Wide up = Widen(chart.Up);
    const Vector3Wide tangentX = Widen(chart.TangentX);
    const Vector3Wide tangentZ = Widen(chart.TangentZ);
    const float64 radius = input.ReferenceRadius;

    for (uint32 row = 0; row < kGrid; ++row)
    {
        const float64 z = rect.MinZ + rect.SizeZ * row / (kGrid - 1u);
        for (uint32 column = 0; column < kGrid; ++column)
        {
            const float64 x = rect.MinX + rect.SizeX * column / (kGrid - 1u);
            const float64 planar = x * x + z * z;
            float64 y = 0.0;
            for (uint32 step = 0; step < kSurfaceSteps; ++step)
            {
                const float64 along = radius + y;
                const float64 px = up.X * along + tangentX.X * x + tangentZ.X * z;
                const float64 py = up.Y * along + tangentX.Y * x + tangentZ.Y * z;
                const float64 pz = up.Z * along + tangentX.Z * x + tangentZ.Z * z;
                const float64 inverseLength = 1.0 / std::sqrt(px * px + py * py + pz * pz);
                const Mathematics::Vector3 direction(static_cast<float32>(px * inverseLength),
                                                     static_cast<float32>(py * inverseLength),
                                                     static_cast<float32>(pz * inverseLength));
                float64 height = 0.0;
                if (!SurfaceHeight(input, direction, height))
                    return false;
                const float64 surface = radius + height;
                y = std::sqrt(std::max(0.0, surface * surface - planar)) - radius;
            }
            samples[static_cast<std::size_t>(row) * kGrid + column] = static_cast<float32>(y);
        }
    }
    return true;
}

} // namespace GameEngine::TerrainECS
