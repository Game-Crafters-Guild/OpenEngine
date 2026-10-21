#pragma once

#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/Entity.h"

#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// The entity's parent, or {} for a root. A Parent naming a dead entity reads as a root.
ECS::EntityHandle GetEffectiveParent(ECS::World& world, ECS::EntityHandle entity);

// True when `possibleAncestor` is `node` itself or one of its ancestors.
bool IsDescendantOf(ECS::World& world, ECS::EntityHandle node, ECS::EntityHandle possibleAncestor);

// An entity's Parent, HierarchyOrder and Transform at one moment, each with whether the entity
// had it: what a hierarchy move changes and what an undo of the move restores.
struct HierarchyEntityState
{
    ECS::EntityHandle entity{};
    bool hadParent = false;
    Components::Parent parent{};
    bool hadOrder = false;
    Components::HierarchyOrder order{};
    bool hadTransform = false;
    Components::Transform transform{};
};

// The entity's Parent, HierarchyOrder and Transform as a hierarchy move's undo restores them.
HierarchyEntityState CaptureReparentState(ECS::World& world, ECS::EntityHandle entity);

// Passed as `insertIndex` to MoveEntitiesKeepingWorldPose to append after the destination's
// other children.
inline constexpr std::size_t kInsertAtEnd = std::numeric_limits<std::size_t>::max();

// The states of every entity a hierarchy move changed, before and after it, sorted by entity id.
struct HierarchyMoveStates
{
    std::vector<HierarchyEntityState> Before;
    std::vector<HierarchyEntityState> After;
};

// Moves `moved`, in the given order, under `destParent` ({} = the scene root) and
// inserts them at `insertIndex` among destParent's other children in Hierarchy
// order (`kInsertAtEnd`, or any index past the end, appends). Each moved entity keeps its world pose:
// its local Transform is recomputed from its WorldTransform and the destination's;
// when either is missing the local Transform is left as it was. The destination's
// children are renumbered. The caller has ruled out cycles.
HierarchyMoveStates MoveEntitiesKeepingWorldPose(ECS::World& world,
                                                 std::span<const ECS::EntityHandle> moved,
                                                 ECS::EntityHandle destParent,
                                                 std::size_t insertIndex);

} // namespace GameEngine::Editor
