#pragma once

#include "Types/Types.h"

#include <span>

namespace GameEngine::Components
{
struct TerrainSurfaceRule;
}

namespace GameEngine::Mathematics
{
struct WorldPosition;
}

namespace GameEngine::TerrainECS
{

// The measurements of a row of texels, one plane for each measurement: entry i of a plane is that field of
// the TerrainRuleSample of texel i. A plane that no condition reads can be empty.
struct TerrainRuleSampleRow
{
    std::span<const float32> SlopeNormalized;
    std::span<const float32> SlopeDegrees;
    std::span<const float32> HeightMeters;
    std::span<const float32> LatitudeDegrees;
};

// EvaluateTerrainSurfaceRuleWeight over a row of planet texels, eight at a time: `weights[i]` has the
// bits of the call with the sample of texel i and PlanetRuleNoiseSample at `positions[i]`. Each plane
// that a condition of `rule` reads, and `positions` when a condition is Noise, has the row's length.
void EvaluatePlanetSurfaceRuleWeightRow(const Components::TerrainSurfaceRule& rule, const TerrainRuleSampleRow& samples,
                                        std::span<const Mathematics::WorldPosition> positions,
                                        std::span<float32> weights);

} // namespace GameEngine::TerrainECS
