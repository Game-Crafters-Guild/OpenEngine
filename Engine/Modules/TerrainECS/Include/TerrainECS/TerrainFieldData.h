#pragma once

#include "Types/Types.h"

#include <vector>

namespace GameEngine::TerrainECS
{

// The CPU side of a per-terrain map the bake composes and the extraction uploads beside the splat:
// the grass control field (TerrainGrassField.h) and the colour map (TerrainColorMap.h). These are
// the parts the uploads read; each field adds its own.
struct TerrainFieldData
{
    // Width * Height texels of the field's channel count, row-major. Empty means the field's
    // neutral value everywhere, which needs no GPU texture.
    std::vector<uint8> Pixels;
    uint32 Width = 0;
    uint32 Height = 0;
    // Bumped on every content change.
    uint64 Version = 0;
    // True once a compose has run on this lattice: an uncomposed tile reads the coarse field.
    bool Initialized = false;
    // Texels changed since the last upload. Exclusive max.
    bool Dirty = false;
    uint32 DirtyMinX = 0, DirtyMinZ = 0, DirtyMaxX = 0, DirtyMaxZ = 0;

    bool IsActive() const { return !Pixels.empty(); }
};

} // namespace GameEngine::TerrainECS
