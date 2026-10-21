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

// The Sky Environment's "Star look" row: one button per look (Vivid, Restrained, Physical, Classic)
// that sets the star dials on the sky and the night tint on every global Post Process Volume as undo
// steps. The look the star dials match is shown selected; when they match none, the row says Custom.
// The row follows the sky's changes (a dial dragged, an undo) while it lives.
void AddSkyStarLookRow(UIElement* parent, ECS::World* world, ECS::EntityHandle sky,
                       Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo);

} // namespace GameEngine
