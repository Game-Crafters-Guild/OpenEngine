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

// Runs `edit` on `entity` as one undo step named `label`, for a write that does not know in
// advance which of the entity's components it adds or changes (a component set by name, which
// can bring a required companion with it). The step holds every component of the entity whose
// bytes differ after the edit, including one the edit added or removed: undo puts back the
// bytes from before (removing what the edit added), redo the bytes from after, and each
// announces the components it wrote as an UndoRedo change. The edit's own writes are the caller's
// to announce.
//
// A refused or throwing edit restores the starting components. A successful edit records only
// changed components; an unchanged edit or a null `undo` records no step.
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
