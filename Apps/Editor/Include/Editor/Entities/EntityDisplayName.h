#pragma once

#include "ECS/ECS.h"

#include <string>
#include <string_view>

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

/// A registered component type name as editor text shows it when no label names the type: the
/// name after its last "::" or "." qualifier, read as words by IdentifierToWords.
/// "GameEngine::Components::AnimatedSprite2D" reads "Animated Sprite 2D" and a script
/// component's "MyGame.Combat.HealthPool" reads "Health Pool".
std::string ComponentTypeNameAsWords(std::string_view registeredTypeName);

} // namespace GameEngine::Editor
