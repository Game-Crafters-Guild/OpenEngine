#pragma once

namespace GameEngine
{
class HierarchyPanel;
class SceneViewPanel;

namespace Editor
{

/// Connects Scene View asset drops to the hierarchy selection; the drop's undo step (one step named for what
/// was placed) goes to the undo service of the viewport's binding.
void WireSceneViewAssetDropUndo(SceneViewPanel* sceneViewPanel, HierarchyPanel* hierarchyPanel);

} // namespace Editor
} // namespace GameEngine
