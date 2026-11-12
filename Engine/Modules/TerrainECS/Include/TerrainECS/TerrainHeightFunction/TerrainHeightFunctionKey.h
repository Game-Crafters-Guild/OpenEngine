#pragma once

#include "TerrainECS/TerrainHeightFunction/TerrainHeightFunctionContext.h"
#include "Types/StringId.h"
#include "Types/Types.h"

namespace GameEngine::TerrainECS
{

/// What a built terrain height function is built for: the name's id (HashStringId), the generation
/// of the name's registration (0 when no module registers it), the seed and the footprint.
struct TerrainHeightFunctionKey
{
    StringId Name = 0;
    uint64 Generation = 0;
    TerrainHeightFunctionContext Context;

    bool operator==(const TerrainHeightFunctionKey&) const = default;
};

} // namespace GameEngine::TerrainECS
