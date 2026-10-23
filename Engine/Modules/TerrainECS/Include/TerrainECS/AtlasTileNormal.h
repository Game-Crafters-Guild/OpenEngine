#pragma once

#include "Types/Types.h"

namespace GameEngine::Terrain
{
class HeightfieldData;
}

namespace GameEngine::TerrainECS
{

struct TerrainTileData;

/// The Full neighbors a tile's normal reads at its edges, so adjacent atlas slots share
/// bit-equal boundary normals. A null neighbor is clamped at that edge.
struct AtlasNormalNeighbors
{
    const Terrain::HeightfieldData* Left = nullptr;
    const Terrain::HeightfieldData* Right = nullptr;
    const Terrain::HeightfieldData* Up = nullptr;
    const Terrain::HeightfieldData* Down = nullptr;
};

/// Brings a Full tile's persistent normal (`tile.Normalmap`) up to date with its heights for a
/// whole atlas slot patch. When the normal is the streaming job's for these very heights
/// (`NormalmapVersion == HeightfieldVersion`) and the job's height scale, `streamedHeightScale`,
/// equals `heightScale`, only its border ring is regenerated: the interior reads no neighbor.
/// Otherwise the whole normal is regenerated from the heights. `tileWorldSize` is the tile's
/// extent in meters.
void RegenerateAtlasTileNormal(TerrainTileData& tile, float32 tileWorldSize, float32 heightScale,
                               float32 streamedHeightScale, const AtlasNormalNeighbors& neighbors);

} // namespace GameEngine::TerrainECS
