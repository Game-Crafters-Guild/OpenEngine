#pragma once

#include "Components/Terrain/TerrainGrass.h"
#include "Terrain/TerrainTypes.h"

namespace GameEngine::TerrainECS
{

// The GrassEnabled bits a grass row takes from its authored component (kTerrainGrassBit* in
// TerrainTypes.h): enabled, Splat Root Color, an automatic Root Color, the texture card and the
// blend mode. Extraction writes these first; it then sets kTerrainGrassBitAlphaNeeded from the
// probed texture and clears kTerrainGrassBitEnabled on a row that places nothing. A terrain with no
// TerrainGrass (`grass` null) has none of them.
inline uint32 TerrainGrassAuthoredBits(const Components::TerrainGrass* grass)
{
    if (!grass)
        return 0u;
    return Terrain::kTerrainGrassBitEnabled
        | (grass->UseSplatRootColor ? Terrain::kTerrainGrassBitSplatRootColor : 0u)
        | (grass->RootColor == Components::kTerrainGrassAutoRootColor ? Terrain::kTerrainGrassBitAutoRootColor : 0u)
        | (grass->TextureGrass ? Terrain::kTerrainGrassBitTextureCard : 0u)
        | (grass->RenderMode == Components::TerrainGrassRenderMode::Blend ? Terrain::kTerrainGrassBitBlendMode : 0u);
}

} // namespace GameEngine::TerrainECS
