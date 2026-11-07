#pragma once

#include "ECS/ECS.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

/// Records changed, added, or removed component bytes; refused or throwing edits restore the starting components.
/// Settle the previous action before opening an enclosing snapshot or calling this function.
/// Enclosing snapshots remain active during `edit`. The caller announces its own writes.
bool CommitEntityComponentsEdit(ECS::World& world, ECS::EntityHandle entity, UndoRedoService* undo,
                                EditorChangeNotifications* notifications, const std::string& label,
                                const std::function<bool()>& edit);

// Every component an entity carries, with its bytes, in type id order.
using EntityComponentBytes = std::vector<std::pair<ECS::ComponentTypeId, std::vector<std::uint8_t>>>;

// The components `entity` carries now, which RestoreEntityComponents gives back.
EntityComponentBytes CaptureEntityComponents(const ECS::World& world, ECS::EntityHandle entity);

// Gives `entity` the components of `captured` with their bytes and removes each component that
// `captured` does not hold. Announces each component it wrote as an UndoRedo change.
void RestoreEntityComponents(ECS::World& world, ECS::EntityHandle entity, const EntityComponentBytes& captured,
                             EditorChangeNotifications* notifications);

} // namespace GameEngine::Editor
