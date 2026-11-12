#pragma once

#include "Types/Types.h"

namespace GameEngine::Mathematics
{
struct Vector3;
}

namespace GameEngine::TerrainECS
{
class TerrainService;
struct TiledTerrainBase;
}

namespace GameEngine::Terrain
{
class HeightfieldData;
}

namespace GameEngine::Components
{
struct Terrain;
}

namespace GameEngine::TerrainTests
{

/// The relief of an untiled test terrain: fills `heightfield` with the fixture noise and seeds the same
/// samples as a decoded heightmap. A terrain that UseNoiseBaseHeightmap sets up rebakes any region back
/// to this relief. The Generated noise effect reaches tiled terrains only.
void SeedNoiseBaseHeightmap(TerrainECS::TerrainService& service, Terrain::HeightfieldData& heightfield);

/// Sets the base of `terrain` to the heightmap SeedNoiseBaseHeightmap seeds.
void UseNoiseBaseHeightmap(Components::Terrain& terrain);

/// The base of a tiled test terrain under a Generated noise effect: the default stack, as the bake reads it.
TerrainECS::TiledTerrainBase GeneratedNoiseBase();

/// The height, in meters, planet `planet`'s layers add in unit direction `direction`: the level-0 term
/// SurfaceHeightQuery's spherical arm adds to the drawn pages. Zero when the planet has no layer.
float32 PlanetLayerHeight(const TerrainECS::TerrainService& service, uint32 planet,
                          const Mathematics::Vector3& direction);

} // namespace GameEngine::TerrainTests
