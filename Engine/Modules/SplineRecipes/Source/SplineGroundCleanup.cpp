#include "SplineRecipes/SplineGroundCleanup.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

namespace GameEngine::SplineRecipes
{

void SplineGroundCleanup::Apply(ECS::World& world) const
{
    if (!world.IsValid(Entity))
        return;
    if (Flatten && world.GetComponent<Components::TerrainFlattenEffect>(Entity))
        world.RemoveComponentImmediate<Components::TerrainFlattenEffect>(Entity);
    if (Claim && world.GetComponent<Components::TerrainGroundClaimEffect>(Entity))
        world.RemoveComponentImmediate<Components::TerrainGroundClaimEffect>(Entity);
    if (Volume && world.GetComponent<Components::TerrainModifierVolume>(Entity))
        world.RemoveComponentImmediate<Components::TerrainModifierVolume>(Entity);

    if (MarkerAfter)
        world.AddComponentImmediate(Entity, *MarkerAfter);
    else if (world.GetComponent<Components::SplineGroundProvisioned>(Entity))
        world.RemoveComponentImmediate<Components::SplineGroundProvisioned>(Entity);
}

void SplineGroundCleanup::Revert(ECS::World& world) const
{
    if (!world.IsValid(Entity))
        return;
    if (Flatten)
        world.AddComponentImmediate(Entity, *Flatten);
    if (Claim)
        world.AddComponentImmediate(Entity, *Claim);
    if (Volume)
        world.AddComponentImmediate(Entity, *Volume);
    world.AddComponentImmediate(Entity, MarkerBefore);
}

} // namespace GameEngine::SplineRecipes
