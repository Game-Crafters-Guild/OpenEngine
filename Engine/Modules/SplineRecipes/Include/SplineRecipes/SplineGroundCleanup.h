#pragma once

#include "Components/Spline/SplineExtrude.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "ECS/Entity.h"

#include <optional>

namespace GameEngine::ECS { class World; }

namespace GameEngine::SplineRecipes
{

// The ground authority's removal of the terrain pieces it made on one entity: the pieces it removed (each value
// keeps its piece for the revert) and the provenance marker before and after. No marker after removes the marker too.
struct SplineGroundCleanup
{
    ECS::EntityHandle Entity;
    std::optional<Components::TerrainFlattenEffect> Flatten;
    std::optional<Components::TerrainGroundClaimEffect> Claim;
    std::optional<Components::TerrainModifierVolume> Volume;
    Components::SplineGroundProvisioned MarkerBefore;
    std::optional<Components::SplineGroundProvisioned> MarkerAfter;

    // Removes the named pieces and writes the marker after. Does nothing on an invalid entity.
    void Apply(ECS::World& world) const;
    // Puts the named pieces and the marker before back.
    void Revert(ECS::World& world) const;
};

} // namespace GameEngine::SplineRecipes
