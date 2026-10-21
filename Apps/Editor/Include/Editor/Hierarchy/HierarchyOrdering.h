#pragma once

#include "Components/Hierarchy.h"
#include "ECS/ECS.h"

#include <span>
#include <vector>

namespace GameEngine {
namespace Editor {

// The HierarchyOrder a new root entity needs to appear at the bottom of the
// hierarchy: one past the largest order currently in the world. Siblings sort
// by HierarchyOrder and a missing one reads as 0 (the top), so every place that
// creates a root for the user (create menu, asset drops, download placeholders)
// assigns this instead of leaving the new entity at the top.
Components::HierarchyOrder NextHierarchyOrderAtBottom(ECS::World* world);

// Append every entity in `roots` that has no HierarchyOrder yet, in the given
// order, after the current maximum. Entities that already carry an order (an
// existing entity a drop targeted, a placeholder the factory ordered) keep it.
// Scans the world once for the whole batch.
void AppendUnorderedRootsToHierarchyEnd(ECS::World& world, std::span<const ECS::EntityHandle> roots);

// Sorts `entities` into the order the Hierarchy lists siblings in: by
// HierarchyOrder (a missing one reads as 0), ties broken by entity id.
void SortByHierarchyOrder(ECS::World& world, std::vector<ECS::EntityHandle>& entities);

// True when `a` is listed above `b` in the Hierarchy tree: it comes first among
// the siblings where their ancestor chains part, or it is an ancestor of `b`.
bool PrecedesInHierarchyTree(ECS::World& world, ECS::EntityHandle a, ECS::EntityHandle b);

// The children of `parent` ({} = the scene roots) in Hierarchy sibling order.
// A Parent naming a dead entity counts as a root. Scans the whole world.
std::vector<ECS::EntityHandle> ChildrenInHierarchyOrder(ECS::World& world, ECS::EntityHandle parent);

// Writes HierarchyOrder 0, 10, 20, ... onto `siblings` in the given order.
void WriteSiblingOrder(ECS::World& world, std::span<const ECS::EntityHandle> siblings);

} // namespace Editor
} // namespace GameEngine
