#pragma once

#include "ECS/ECS.h"

#include <span>
#include <vector>

namespace GameEngine::ECS
{

class Archetype;

/**
 * The result and the scratch storage of World::CreateBatchFromWorld.
 *
 * After a call, GetTargets()[i] is the entity created for entry i of the
 * call's ClonePlan. Keep one remap across calls to reuse its storage; each
 * call overwrites it.
 */
class EntityRemap
{
  public:
    /// The created entities, one per entry of the last call's plan, in the plan's order.
    std::span<const EntityHandle> GetTargets() const { return m_Targets; }

  private:
    friend class World;

    std::vector<EntityHandle> m_Targets;
    // The created entities grouped by the plan's archetype groups, for one Added batch per group.
    std::vector<EntityHandle> m_GroupedTargets;
    // The target archetype of each plan group.
    std::vector<Archetype*> m_TargetArchetypes;
};

} // namespace GameEngine::ECS
