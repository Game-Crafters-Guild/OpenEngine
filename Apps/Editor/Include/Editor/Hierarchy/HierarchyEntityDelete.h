#pragma once

#include "ECS/Entity.h"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

// The entities a Hierarchy delete removes: each selected entity with its whole subtree, each
// entity once, in ascending id order (the order DeleteEntitiesCommand replays). A selected
// entity below another selected one adds nothing; a handle that is invalid or no longer alive
// is skipped.
std::vector<ECS::EntityHandle> CollectHierarchyDeletion(ECS::World& world,
                                                        std::span<const ECS::EntityHandle> selected);

// The undo step's name for deleting `count` entities.
std::string HierarchyDeletionUndoName(std::size_t count);

// Deletes the entities CollectHierarchyDeletion takes for `selected`, for the Hierarchy's Delete key
// and its context menu's Delete. With an undo service the delete is one undo step that runs
// `onDeleted` (deselect) after each delete and `onRevived` (reselect) after each undo; without one
// the delete runs once, then `onDeleted`. Returns false when there is nothing to delete.
bool DeleteHierarchySelection(ECS::World& world, UndoRedoService* undo, EditorChangeNotifications* notifications,
                              std::span<const ECS::EntityHandle> selected, std::function<void()> onDeleted,
                              std::function<void()> onRevived);

} // namespace GameEngine::Editor
