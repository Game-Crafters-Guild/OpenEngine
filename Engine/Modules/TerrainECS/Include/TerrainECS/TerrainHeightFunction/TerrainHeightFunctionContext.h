#pragma once

#include "Types/Types.h"

namespace GameEngine::TerrainECS
{

/// What a terrain height function is built for: the terrain's seed and its footprint in meters.
struct TerrainHeightFunctionContext
{
    uint64 Seed = 0;
    float32 SizeX = 0.0f;
    float32 SizeZ = 0.0f;

    bool operator==(const TerrainHeightFunctionContext&) const = default;
};

} // namespace GameEngine::TerrainECS
