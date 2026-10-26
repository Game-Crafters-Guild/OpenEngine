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

/// The Sky Environment's "Seen by" row: two picture tiles, Camera (the default) and Eye, that set the star
/// observer as one undo step. The presets never change it, so every look works with either.
void AddSkyStarSeenByRow(UIElement* parent, ECS::World* world, ECS::EntityHandle sky,
                         Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo);

} // namespace GameEngine
