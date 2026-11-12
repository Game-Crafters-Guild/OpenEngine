#pragma once

// GameSDK/TerrainHeightFunction.h: give a planar terrain base heights computed by game code.
// A terrain whose Base Source is Function and whose Height Function is `Name` draws this function
// for its Base Seed. The user page docs/terrain/height-functions.mdx states the rules.

#include "ECS/Reflection.h"
#include "TerrainECS/TerrainHeightFunction/ITerrainHeightFunction.h"

/// Registers the height function `Type` under `Name` when its module loads. `Type` derives from
/// TerrainECS::ITerrainHeightFunction and has a constructor that takes a TerrainHeightFunctionContext.
#define GE_REGISTER_TERRAIN_HEIGHT_FUNCTION(Type, Name)                                                    \
    namespace                                                                                              \
    {                                                                                                      \
    [[maybe_unused]] const bool GE_PP_CONCAT(ge_terrain_height_function_, __LINE__) =                      \
        ::GameEngine::TerrainECS::RegisterTerrainHeightFunction(                                           \
            Name, &::GameEngine::TerrainECS::MakeTerrainHeightFunction<Type>);                             \
    }
