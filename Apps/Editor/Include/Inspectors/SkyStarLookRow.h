#pragma once

#include "ECS/ECS.h"

namespace GameEngine
{
class UIElement;
namespace Editor
{
class EditorChangeNotifications;
class UndoRedoService;
} // namespace Editor

/// The Sky Environment's star "Preset" row: a list with a picture, the name and one line per look
/// (Vivid, Restrained, Classic, Physical). Picking one sets the star dials on the sky and the night
/// tint on every global Post Process Volume as one undo step. The field shows the look the star dials
/// and the night tint match, or Custom when they match none; Custom is a state, never an entry in the
/// list. A caption under the list names the cause when it is not in the row: no global volume, or
/// a night tint that alone makes the field read Custom. The row follows the sky's and the volumes' changes (a dial dragged, an undo) while it lives.
void AddSkyStarLookRow(UIElement* parent, ECS::World* world, ECS::EntityHandle sky,
                       Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo);

} // namespace GameEngine
