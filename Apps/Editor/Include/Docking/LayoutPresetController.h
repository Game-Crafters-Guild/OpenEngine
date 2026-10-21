#pragma once

namespace GameEngine
{
class DockNode;
class DockingManager;
class UIManager;

namespace Editor
{

/// Coordinates layout-preset edits with the main dockspace and toolbar.
/// Preset UI/model policy stays out of EditorApplication and DockingManager.
class LayoutPresetController
{
  public:
    static bool Apply(DockingManager& docking,
                      UIManager* mainUi,
                      const DockNode* preset,
                      bool rebuildDockspace);
    static bool IsCurrentLayoutDirty(DockingManager& docking,
                                     UIManager* mainUi,
                                     const DockNode* preset);
    static void SetActiveToolbarIndex(UIManager* mainUi, int activeIndex);
    /// Copies the main dockspace's splitter positions into the docking model,
    /// so a layout read from the model matches what the user sees.
    static void SyncSplitRatios(UIManager* mainUi);
};

} // namespace Editor
} // namespace GameEngine
