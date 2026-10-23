#include "TerrainECS/CoarseTileStandIn.h"

#include "TerrainECS/TerrainService.h"

#include <algorithm>

namespace GameEngine::TerrainECS
{

bool CoarseTileStandIn::Matches(uint32 tileRes, float32 tileWorldSize, float32 heightScale,
                                uint64 heightfieldVersion) const
{
    return Heights.GetWidth() == tileRes && Heights.GetHeight() == tileRes &&
           Normalmap.size() == static_cast<size_t>(tileRes) * tileRes * 4u && TileWorldSize == tileWorldSize &&
           HeightScale == heightScale && HeightfieldVersion == heightfieldVersion;
}

void BuildCoarseTileStandIn(const Terrain::HeightfieldData& coarse, uint32 tileRes, float32 tileWorldSize,
                            float32 heightScale, CoarseTileStandIn& out)
{
    out = CoarseTileStandIn{};
    const uint32 coarseRes = coarse.GetWidth();
    if (coarseRes < 2 || coarse.GetHeight() != coarseRes || tileRes < 2)
        return;
    std::vector<float32> upsampled;
    UpsampleHeightfieldBilinear(coarse.GetRawSamples(), coarseRes, upsampled, tileRes);
    out.Heights.Resize(tileRes, tileRes, 0.0f);
    std::copy(upsampled.begin(), upsampled.end(), out.Heights.GetMutableSamples());
    uint32 normalW = 0;
    uint32 normalH = 0;
    GenerateNormalmapFromHeightfield(out.Heights, tileWorldSize, tileWorldSize, heightScale, out.Normalmap, normalW,
                                     normalH);
    out.TileWorldSize = tileWorldSize;
    out.HeightScale = heightScale;
}

} // namespace GameEngine::TerrainECS
