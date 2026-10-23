#pragma once

#include "ECS/Entity.h"
#include "UI/Controls/EnumField.h"

#include <vector>

namespace GameEngine
{
class UIElement;
namespace Components { enum class ShadowCasting : uint8; }
namespace ECS { class World; }
namespace Editor
{
class EditorChangeNotifications;
class UndoRedoService;
} // namespace Editor

namespace InspectorDrag
{

/// The Mesh Renderer's Shadow Casting row: a dropdown of Off, On and Shadows Only. A choice sets
/// the mode on the entity `e` and on each other selected entity, as one undo step.
EnumField<Components::ShadowCasting>* AddShadowCastingRow(UIElement* parent, ECS::World* w, ECS::EntityHandle e,
                                                          Editor::EditorChangeNotifications* n,
                                                          Editor::UndoRedoService* undo,
                                                          const std::vector<ECS::EntityHandle>& additionalEntities);

} // namespace InspectorDrag
} // namespace GameEngine
