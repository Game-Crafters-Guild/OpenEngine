#pragma once

#include "Types/Types.h"

// The texel arithmetic of the colour map compose (TerrainColorMap.cpp), for the compose and its tests.

namespace GameEngine::TerrainECS
{

// Composite one premultiplied writer over a premultiplied texel: rgb' = colour + rgb x (1 - weight),
// a' = weight + a x (1 - weight), with colour already times the weight. No division, so a texel
// nothing has written (0, 0, 0, 0) composes to a finite result.
void CompositePremultipliedOver(float32 (&texel)[4], const float32 (&premultipliedColor)[3], float32 weight);

// The sRGB byte of a linear value in [0, 1]: what the RGBA8_SRGB image decodes back to linear.
uint8 EncodeLinearToSrgbByte(float32 linear);

} // namespace GameEngine::TerrainECS
