#pragma once

#include "ECS/ECS.h"

#include <cstdint>
#include <vector>

namespace GameEngine::ECS
{

class Archetype;

/**
 * What World::CreateBatchFromWorld copies, worked out once for a list of
 * source entities by World::PlanClone: the entities grouped by archetype, and
 * every entity field of their rows with the list position of the entity it
 * names. A clone through a plan reads the source rows and does no lookup per
 * field.
 *
 * A source entity may appear more than once in the list: its k-th occurrence
 * belongs to copy k, and an entity field of a copy-k row names the copy-k
 * entry of the entity it names.
 *
 * A plan describes its entities as they were when it was built: their
 * archetypes and the entities their fields name. Build a new plan after
 * changing either; a clone through a plan whose entity died or changed
 * archetype is refused.
 */
class ClonePlan
{
  private:
    friend class World;

    static constexpr std::uint32_t kNoPosition = UINT32_MAX;

    // The list positions of one source archetype. The group's targets are
    // EntityRemap's grouped targets [FirstTarget, FirstTarget + Count).
    struct Group
    {
        const Archetype* Source = nullptr; // null for an entity with no components
        std::uint32_t FirstTarget = 0;
        std::uint32_t Count = 0;
    };
    // One EntityHandle in the row at Position: in component TypeId at byte
    // Offset, naming the entity at TargetPosition, or kNoPosition for an
    // entity outside the list (the copy gets the invalid handle).
    struct FieldTarget
    {
        ComponentTypeId TypeId = 0;
        std::uint32_t Position = 0;
        std::uint32_t Offset = 0;
        std::uint32_t TargetPosition = kNoPosition;
    };

    std::uint64_t m_SourceWorldId = 0;
    std::vector<EntityHandle> m_Sources;
    // The sources in EntityRemap's grouped order: m_GroupedSources[i] is the source of grouped target i.
    std::vector<EntityHandle> m_GroupedSources;
    std::vector<std::uint32_t> m_GroupOfPosition;
    // Where each position's target lands in EntityRemap's grouped targets.
    std::vector<std::uint32_t> m_GroupedIndexOfPosition;
    std::vector<Group> m_Groups;
    std::vector<FieldTarget> m_FieldTargets; // in position order
};

} // namespace GameEngine::ECS
