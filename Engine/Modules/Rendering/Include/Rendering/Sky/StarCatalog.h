#pragma once

namespace GameEngine::Rendering
{

/// One night-sky star as sky_stars_billboard.vert reads it (std430, 32 bytes).
struct StarInstanceGPU
{
    /// xyz: unit direction in world space; w: visual (V) magnitude.
    float dirAndMagnitude[4];
    /// xyz: linear Rec.709 colour of the star's blackbody, luminance 1; w: twinkle seed in [0, 1).
    float colorAndSeed[4];
};

} // namespace GameEngine::Rendering
