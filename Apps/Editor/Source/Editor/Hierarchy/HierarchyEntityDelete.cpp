#include "Editor/Hierarchy/HierarchyEntityDelete.h"

#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/DeleteEntitiesSelectionCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "ECS/World.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <utility>

namespace GameEngine::Editor
{

std::vector<ECS::EntityHandle> CollectHierarchyDeletion(ECS::World& world,
                                                        std::span<const ECS::EntityHandle> selected)
{
    std::unordered_set<std::uint64_t> seen;
    std::vector<ECS::EntityHandle> entities;
    for (const ECS::EntityHandle root : selected)
    {
        if (!root.IsValid() || !world.IsValid(root))
            continue;
        for (const ECS::EntityHandle entity : DeleteEntitiesCommand::CollectSubtree(world, root))
        {
            if (entity.IsValid() && world.IsValid(entity) && seen.insert(static_cast<std::uint64_t>(entity.id)).second)
                entities.push_back(entity);
        }
    }
    std::sort(entities.begin(), entities.end(),
              [](const ECS::EntityHandle& a, const ECS::EntityHandle& b) { return a.id < b.id; });
    return entities;
}

std::string HierarchyDeletionUndoName(std::size_t count)
{
    return count > 1 ? "Delete Entities" : "Delete Entity";
}

bool DeleteHierarchySelection(ECS::World& world, UndoRedoService* undo, EditorChangeNotifications* notifications,
                              std::span<const ECS::EntityHandle> selected, std::function<void()> onDeleted,
                              std::function<void()> onRevived)
{
    std::vector<ECS::EntityHandle> entities = CollectHierarchyDeletion(world, selected);
    if (entities.empty())
        return false;

    std::string undoName = HierarchyDeletionUndoName(entities.size());
    if (!undo)
    {
        DeleteEntitiesCommand command(std::move(undoName), &world, notifications, std::move(entities));
        command.Redo();
        if (onDeleted)
            onDeleted();
        return true;
    }

    undo->Execute(std::make_unique<DeleteEntitiesSelectionCommand>(std::move(undoName), &world, notifications,
                                                                   std::move(entities), std::move(onDeleted),
                                                                   std::move(onRevived)));
    return true;
}

} // namespace GameEngine::Editor
