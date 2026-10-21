#pragma once

#include "ECS/ECS.h"

#include <span>
#include <string>

namespace GameEngine
{
struct SearchIcon;

namespace ECS
{
class World;
}

namespace Editor
{

/// The CSS class of a component's icon in the inspector: the class its registered
/// EditorComponentTraits name, else the built-in class of an engine component (a light's
/// follows its type when `world` holds `entity`), else "inspector-section-icon-default".
std::string ComponentIconClass(ECS::World* world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId);

/// The icon of an Add Component picker entry that adds `typeIds`: the ComponentIconClass of the
/// last of them that has an icon of its own, else the generic glyph of the entry's `category`. The
/// class alone draws it (inspector.css), so a component whose traits register an icon class shows
/// that icon in the picker as in its inspector header.
SearchIcon ComponentPickerIcon(std::span<const ECS::ComponentTypeId> typeIds, const std::string& category);

} // namespace Editor
} // namespace GameEngine
