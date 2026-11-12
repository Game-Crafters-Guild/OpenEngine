#pragma once

#include "ECS/ECS.h"

#include <string>

namespace GameEngine
{
namespace ECS
{
class World;
} // namespace ECS

namespace Components
{
struct Terrain;
struct TerrainPlanetRelief;
} // namespace Components

namespace TerrainECS
{

/// What ProvisionTerrainEntity did.
struct TerrainProvisioningResult
{
    /// Empty when the terrain was provisioned; otherwise why it was refused and how to fix it, and the entity is untouched.
    std::string Error;
    /// The scene's default "Terrain Surface Rules" volume, when this call spawned it; invalid otherwise.
    ECS::EntityHandle SurfaceRulesVolume;
};

/// Makes `entity` a terrain carrying `config`: the one implementation every creation path shares
/// (the editor's create menu and create_terrain IPC, the web library's scene.terrain.create).
///
/// - Adds Terrain (or overwrites it with `config`) and a default TerrainGrass when absent.
/// - A spherical terrain gets `relief` as its TerrainPlanetRelief; a planar one loses any it had.
/// - A planet below the per-tile cap gets its heightfield now, so it renders at once. The
///   TerrainExtractionSystem provisions every other terrain on the next tick: a planar terrain's
///   tiles, pages and page colliders, and a larger planet's tiles.
/// - A planar terrain in a scene with no global surface rules gets the default rules volume
///   (MakeDefaultTerrainSurfaceRules): an ordinary entity, deletable, never re-created.
///
/// Refuses a planar terrain that PlanarTerrainSizeError refuses, with its message.
/// The caller adds the entity's Name and Transform; the editor orders the volume in its hierarchy.
TerrainProvisioningResult ProvisionTerrainEntity(ECS::World& world, ECS::EntityHandle entity,
                                                 const Components::Terrain& config,
                                                 const Components::TerrainPlanetRelief& relief);

/// Places the default rows again on every rules effect still marked ProvisionedDefaults, from the
/// joined height range of the scene's planar terrains (TerrainService::GetHeightRangeMeters). That
/// range is known only after a terrain's bake or its tiles' arrival, so a heightmap assigned after
/// creation moves the snow row to its summits. Writes only an effect whose rows change and returns
/// how many it wrote. The TerrainExtractionSystem calls it each tick.
uint32 PlaceProvisionedSurfaceRules(ECS::World& world);

/// Spawns "Terrain Noise": an ordinary Global volume with a TerrainNoiseEffect in Generated mode at the
/// Rolling preset, the relief of every Flat planar terrain. Returns the entity, invalid if none was made.
ECS::EntityHandle SpawnGeneratedNoiseVolume(ECS::World& world);

} // namespace TerrainECS
} // namespace GameEngine
