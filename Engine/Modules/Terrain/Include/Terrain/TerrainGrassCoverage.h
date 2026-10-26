#pragma once

#include "Mathematics/Interpolation.h"
#include "Types/Types.h"

#include <algorithm>

namespace GameEngine::Terrain
{

// The grass field coverage on the CPU: the same answer the placement spawns blades by and the ground
// shades with (Includes/terrain_grass_canopy.glsl, GE_SHARED_GRASS_CANOPY). The terrain's colour map
// writes the field's ground by it at bake. TerrainColorMapApplyTests (TheHostCoverageIsTheShadersCoverage)
// runs this mirror against the shader block itself on a sweep, so the two cannot drift.

inline float32 GrassCoverageSmoothStep(float32 edge0, float32 edge1, float32 x)
{
    const float32 t = Math::Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

// The splat feather: the grass layer's normalized weight against the mask threshold, times its
// dominance over the strongest other layer. `weights` are the four normalized channel weights;
// without a splat the terrain is all channel 0 and the feather is wider.
inline float32 GrassSplatCoverage(const float32 (&weights)[4], uint32 grassLayer, float32 maskThreshold,
                                  bool hasSplatmap)
{
    const uint32 layer = std::min(grassLayer, 3u);
    const float32 grassWeight = weights[layer];
    float32 otherMax = 0.0f;
    for (uint32 other = 0; other < 4u; ++other)
        if (other != layer)
            otherMax = std::max(otherMax, weights[other]);

    const float32 threshold = Math::Clamp01(maskThreshold);
    const float32 dominantGrass = grassWeight - otherMax;
    const float32 dominanceFloor = std::min(0.18f, threshold);
    if (hasSplatmap && (grassWeight < threshold || dominantGrass < dominanceFloor))
        return 0.0f;

    const float32 maskFeather = hasSplatmap ? 0.08f : 0.25f;
    const float32 dominanceMin = hasSplatmap ? dominanceFloor : 0.08f;
    const float32 dominanceMax = hasSplatmap ? (dominanceFloor + 0.18f) : 0.28f;
    return GrassCoverageSmoothStep(threshold, std::min(threshold + maskFeather, 1.0f), grassWeight)
        * GrassCoverageSmoothStep(dominanceMin, dominanceMax, dominantGrass);
}

// The field coverage: the splat coverage times a region's density, nothing where a region removes
// the grass (height 0). Both scales are the grass control field's, 1 where no region is authored.
inline float32 GrassFieldCoverage(float32 splatCoverage, float32 heightScale, float32 densityScale)
{
    return heightScale > 0.0f ? splatCoverage * Math::Clamp01(densityScale) : 0.0f;
}

} // namespace GameEngine::Terrain
