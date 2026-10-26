#include "TerrainECS/TerrainHeightSource.h"

namespace GameEngine::TerrainECS
{

bool TerrainHeightIsPaged(const TerrainHeightSourceInputs& inputs)
{
    if (inputs.ModifiersPending || !inputs.PagesOnLattice || !inputs.GpuReadsPages)
        return false;
    return inputs.GeneratedBase || inputs.CookedStoreOpen;
}

bool TerrainHeightHasCpuPages(const TerrainHeightSourceInputs& inputs)
{
    return inputs.PagesOnLattice && (inputs.GeneratedBase || inputs.CookedStoreOpen);
}

} // namespace GameEngine::TerrainECS
