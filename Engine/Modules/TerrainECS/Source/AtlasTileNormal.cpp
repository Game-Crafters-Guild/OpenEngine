#include "TerrainECS/AtlasTileNormal.h"

#include "TerrainECS/TerrainService.h"
#include "Core/CpuProfiler.h"

namespace GameEngine::TerrainECS
{

void RegenerateAtlasTileNormal(TerrainTileData& tile, float32 tileWorldSize, float32 heightScale,
                               float32 streamedHeightScale, const AtlasNormalNeighbors& neighbors)
{
    const uint32 res = tile.Heightfield.GetWidth();
    const bool normalSeeded = tile.Normalmap.size() == static_cast<size_t>(res) * res * 4u &&
                              tile.NormalmapWidth == res && tile.NormalmapHeight == res;
    const Terrain::HeightfieldData* nL = neighbors.Left;
    const Terrain::HeightfieldData* nR = neighbors.Right;
    const Terrain::HeightfieldData* nU = neighbors.Up;
    const Terrain::HeightfieldData* nD = neighbors.Down;
    if (!normalSeeded || tile.NormalmapVersion != tile.HeightfieldVersion || streamedHeightScale != heightScale)
    {
        GE_CPU_PROFILE_SCOPE("Terrain.NormalRegen.Whole");
        GenerateNormalmapFromHeightfield(tile.Heightfield, tileWorldSize, tileWorldSize, heightScale, tile.Normalmap,
                                         tile.NormalmapWidth, tile.NormalmapHeight, nL, nR, nU, nD);
        return;
    }

    // The streaming job's normal is current for these heights: only its edge texels read the
    // neighbors. The ring is the top and bottom rows, then the left and right columns between them.
    GE_CPU_PROFILE_SCOPE("Terrain.NormalRegen.Border");
    const int32 last = static_cast<int32>(res) - 1;
    GenerateNormalmapRegionFromHeightfield(tile.Heightfield, tileWorldSize, tileWorldSize, heightScale,
                                           tile.Normalmap, 0, 0, last, 0, nL, nR, nU, nD);
    GenerateNormalmapRegionFromHeightfield(tile.Heightfield, tileWorldSize, tileWorldSize, heightScale,
                                           tile.Normalmap, 0, last, last, last, nL, nR, nU, nD);
    GenerateNormalmapRegionFromHeightfield(tile.Heightfield, tileWorldSize, tileWorldSize, heightScale,
                                           tile.Normalmap, 0, 1, 0, last - 1, nL, nR, nU, nD);
    GenerateNormalmapRegionFromHeightfield(tile.Heightfield, tileWorldSize, tileWorldSize, heightScale,
                                           tile.Normalmap, last, 1, last, last - 1, nL, nR, nU, nD);
}

} // namespace GameEngine::TerrainECS
