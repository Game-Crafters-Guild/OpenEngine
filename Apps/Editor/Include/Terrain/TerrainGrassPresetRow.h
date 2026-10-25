#pragma once

#include "ECS/ECS.h"

#include <functional>

namespace GameEngine
{
class UIElement;
namespace Editor
{
class EditorChangeNotifications;
class UndoRedoService;
} // namespace Editor

/// The Terrain Grass "Preset" row: a list with a picture, the name and one line per kind of field
/// (Meadow, Lawn, Steppe). Picking one writes its dials (TerrainGrass::ApplyTerrainGrassPreset) as
/// one undo step, then calls `requestRefresh` so the dial rows below show the preset's values. The
/// field shows the preset the grass is on, or Custom once a dial differs from it
/// (TerrainGrass::ReconcileTerrainGrassPreset); Custom is a state, never an entry in the list. The
/// row follows the grass's changes (a dial dragged, an undo) while it lives.
void AddTerrainGrassPresetRow(UIElement* parent, ECS::World* world, ECS::EntityHandle terrain,
                              Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo,
                              std::function<void()> requestRefresh);

} // namespace GameEngine
