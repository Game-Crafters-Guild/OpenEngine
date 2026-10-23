#pragma once

#include "Types/Types.h"

#include <cstddef>

namespace GameEngine::TerrainGrass
{

// One placed blade as the placement compute writes it: the std430 mirror of GrassBladeInstance in
// terrain_grass_place.comp, read by terrain_grass_vertex_modifier.glsl.
struct GrassBladeInstanceGPU
{
    float WorldX = 0.0f;
    float WorldZ = 0.0f;
    float RootY = 0.0f;
    float BladeHeight = 0.0f;
    float BladeWidth = 0.0f;
    float Yaw = 0.0f;
    uint32 RandAndCoverage = 0; // grass_clump.glsl owns the layout (per-blade random, field coverage)
    uint32 BladeShape = 0;      // grass_clump.glsl owns the bit layout (tilt, twist, tuft fan)
    uint32 TerrainIndex = 0;
    float NormalX = 0.0f; // baked terrain normal (atlas terrains only; 0 otherwise)
    float NormalZ = 0.0f;
    uint32 Flags = 0;     // GrassClumpModel.h owns the bit layout (LOD, clump code, min width)
    uint32 RootColor = 0; // the ground colour under the root; grass_clump.glsl owns the packing
};
static_assert(sizeof(GrassBladeInstanceGPU) == 52u);
static_assert(offsetof(GrassBladeInstanceGPU, RootColor) == 48u);

// The stride of the instance pool, in bytes.
inline constexpr uint32 kGrassBladeInstanceBytes = sizeof(GrassBladeInstanceGPU);

} // namespace GameEngine::TerrainGrass
