#pragma once

#include "ECS/Entity.h"

#include <string>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

class EditorChangeNotifications;
class UndoRedoService;

// Why `name` cannot name an entity, written for the user; empty when it can. Entity names need
// not be unique: two entities may share one, as two copies of a prop do.
std::string ValidateEntityName(const std::string& name);

// Writes `name` into the entity's Name component as one undo step, "Rename Entity", and announces
// the change so the Hierarchy and the Inspector show it. False when the name is refused or the
// entity is gone.
bool CommitEntityRename(ECS::World& world, ECS::EntityHandle entity, const std::string& name, UndoRedoService* undo,
                        EditorChangeNotifications* notifications);

} // namespace GameEngine::Editor
