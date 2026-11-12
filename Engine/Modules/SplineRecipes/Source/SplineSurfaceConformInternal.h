#pragma once

#include "Types/Types.h"

namespace GameEngine::Components { struct SplineExtrude; }
namespace GameEngine::ECS { class World; }
namespace GameEngine::Mathematics { struct AABB; }
namespace GameEngine::TerrainECS { class TerrainModifierSystem; }

namespace GameEngine::SplineRecipes
{

// A digest of the terrain set in `reach` (GroundReach): per terrain that meets it, the pick inputs, the heightfield
// and the page source's identity; the planets' layers; with `modifierSystem`, the bakes in the reach. Mesh ground,
// a planet's radius and relief, the pages and the overlay are not in it. A null `modifierSystem` conforms by raycast.
[[nodiscard]] uint64 ConformTerrainSetRevision(ECS::World& world,
                                               const TerrainECS::TerrainModifierSystem* modifierSystem,
                                               const Mathematics::AABB& reach);

// How far from its centerline an extrude reads the ground, in meters: the strip with its shoulders,
// verges and offset, or the fill's search reach (MaxHalfWidth), and kGroundReadSlackMetres.
[[nodiscard]] float32 ExtrudeGroundReach(const Components::SplineExtrude& recipe);

} // namespace GameEngine::SplineRecipes
