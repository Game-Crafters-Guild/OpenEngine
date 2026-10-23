#pragma once

#include "ECS/Components.h"
#include "ECS/HierarchyTagDerivation.h"
#include "ECS/Systems.h"
#include "Types/Types.h"

#include <cstddef>

namespace GameEngine
{
namespace ECS
{

// Derives DisabledInHierarchy — "this entity is off, or an ancestor is" — from
// the authored Disabled tag and the Parent chain. Queries exclude both tags, so
// turning a parent off takes its whole subtree out of every system without any
// of those systems knowing that hierarchies exist.
//
// An entity's own state is immediate: Entity::SetEnabled changes it in one
// archetype move, adding both tags on the way off and removing both on the way
// on unless the parent is still off. This pass owns the derived half, so
// descendants reach their new state on the first update after the change, and
// a world that never runs this system still excludes self-disabled entities —
// just not their children. Binding to a world, it names Parent as the world's
// parent relation (World::SetParentRelation), so switching an entity back on
// while its parent is still off keeps the derived tag instead of dropping it
// until the next pass.
//
// It mutates archetypes, so it runs exclusively within its wave, and it runs at
// all only when a structural change or a Parent edit could have moved the
// answer. With nothing in the world disabled it exits on two archetype-level
// counts.
class DisabledInHierarchySystem : public ISystem
{
  public:
    const char* GetName() const override { return "DisabledInHierarchy"; }
    bool RequiresExclusiveUpdate() const override { return true; }
    void Update(World& world, float32 deltaTime) override;

    // Tags added plus tags removed by the last update that ran, and the number
    // of updates that derived at all — the diagnostics the tests assert on.
    std::size_t GetLastPropagatedCount() const { return m_Derivation.GetLastPropagatedCount(); }
    std::size_t GetDerivationCount() const { return m_Derivation.GetDerivationCount(); }

  private:
    HierarchyTagDerivation<Disabled, DisabledInHierarchy> m_Derivation;
};

} // namespace ECS
} // namespace GameEngine
