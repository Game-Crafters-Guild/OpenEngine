#pragma once

#include "Mathematics/Vector4.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::Ocean
{

/// CPU geometry of the camera-following ocean ring mesh: one dense 4 x 4 tile
/// square (LOD 0) and seven 12-tile rings, each twice the size of the last.
struct OceanRingMesh
{
    /// One per vertex: xy = ring-local X and Z in root-tile units, z = LOD index,
    /// w = footprint multiplier (larger on the outer horizon skirt).
    std::vector<Mathematics::Vector4> Vertices;
    std::vector<uint32> Indices;
};

/// Builds the ring mesh with `cellsPerTile` cells per tile side (even, see
/// ResolveOceanRingCellsPerTile). The vertex stage places it (ocean_ring_layout.glsl).
OceanRingMesh BuildOceanRingMesh(uint32 cellsPerTile);

} // namespace GameEngine::Ocean
