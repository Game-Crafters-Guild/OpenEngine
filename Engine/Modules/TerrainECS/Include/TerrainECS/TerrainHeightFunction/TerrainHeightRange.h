#pragma once

#include "Types/Types.h"

namespace GameEngine::TerrainECS
{

/// The lowest and the highest height of a terrain height function over a region, in meters.
struct TerrainHeightRange
{
    float32 Min = 0.0f;
    float32 Max = 0.0f;
};

} // namespace GameEngine::TerrainECS
