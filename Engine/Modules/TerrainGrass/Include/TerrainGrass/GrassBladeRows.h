#pragma once

#include "Types/Types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace GameEngine::TerrainGrass
{

// The tip droop's share of the bend in the blade's angle law: the spine's angle from the terrain
// normal at height fraction t is tilt + bend (t^2 + kGrassBladeDroopRatio t^4). Mirror of
// kGrassBladeDroopRatio in terrain_grass_vertex_modifier.glsl.
inline constexpr float32 kGrassBladeDroopRatio = 0.6f;

namespace Detail
{
// How densely rows are spaced at height fraction t: the square root of the spine's curvature there
// per unit of bend, the derivative of the angle law's t^2 + kGrassBladeDroopRatio t^4.
inline float32 GrassBladeRowDensity(float32 t)
{
    return std::sqrt(2.0f * t + 4.0f * kGrassBladeDroopRatio * t * t * t);
}
} // namespace Detail

// The height fraction of row `row` of a blade strip of `segments` segments, 0 at the root and 1 at
// the tip.
//
// The rows are placed so each straight segment strays from the curved spine by the same amount. A
// segment of height dt strays by about its curvature times dt^2, and the curvature along the angle
// law above grows as 2t + 4 kGrassBladeDroopRatio t^3, from nothing at the root to its most at the
// tip, where the droop bends the leaf. So the spacing goes as one over the square root of that,
// and the rows crowd toward the tip, where the eye sees the bend. The tilt bends nothing, and the
// bend and the wind scale the curvature as a whole, so the spacing holds for every blade. Mesh build
// only: it costs nothing at draw time.
inline float32 GrassBladeRowHeight(uint32 row, uint32 segments)
{
    if (segments == 0u || row >= segments)
        return 1.0f;
    if (row == 0u)
        return 0.0f;

    // The cumulative row measure, the integral of the density from the root to t, by the trapezoid
    // rule on a fine fixed grid, then inverted at the row's share of the whole.
    constexpr uint32 kSteps = 512u;
    float32 cumulative[kSteps + 1u];
    cumulative[0] = 0.0f;
    for (uint32 i = 1u; i <= kSteps; ++i)
    {
        const float32 t0 = static_cast<float32>(i - 1u) / static_cast<float32>(kSteps);
        const float32 t1 = static_cast<float32>(i) / static_cast<float32>(kSteps);
        cumulative[i] = cumulative[i - 1u]
            + 0.5f * (Detail::GrassBladeRowDensity(t0) + Detail::GrassBladeRowDensity(t1)) * (t1 - t0);
    }
    const float32 target =
        cumulative[kSteps] * static_cast<float32>(row) / static_cast<float32>(segments);
    const float32* upper = std::lower_bound(cumulative, cumulative + kSteps + 1u, target);
    const uint32 i = static_cast<uint32>(std::max<std::ptrdiff_t>(upper - cumulative, 1));
    const float32 span = cumulative[i] - cumulative[i - 1u];
    const float32 within = span > 0.0f ? (target - cumulative[i - 1u]) / span : 0.0f;
    return (static_cast<float32>(i - 1u) + within) / static_cast<float32>(kSteps);
}

} // namespace GameEngine::TerrainGrass
