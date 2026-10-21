#pragma once

#include "ECS/ECS.h"

#include <string>

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Editor
{

/// The entity's name as editor text shows it: its Name component; when it has none or the
/// name is empty, the UntitledName a component of it registers (EditorComponentTraits), else
/// its id label (EntityIdLabel).
std::string EntityDisplayName(const ECS::World& world, ECS::EntityHandle entity);

/// The entity's id as editor text shows it: "Entity <id>".
std::string EntityIdLabel(ECS::EntityHandle entity);

} // namespace GameEngine::Editor
