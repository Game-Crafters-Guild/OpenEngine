#pragma once

namespace GameEngine
{
class GameViewPanel;
class SceneViewPanel;
class UIManager;

namespace Editor
{

/// The Scene View and Game View panels mounted in a window's dock, for the callers that
/// ask every frame (frame routing, controller sync, overlay declaration, binding).
///
/// Each call finds the window's dock Mount from its root and reads the panel live through
/// the Mount: a tab activation swaps the Mount's target. Returns nullptr when the window
/// has no UI, the Mount is absent, or its active target is another panel. UI thread only.
class MountedViewPanels final
{
  public:
    MountedViewPanels() = delete;

    static SceneViewPanel* SceneView(UIManager* ui);
    static GameViewPanel* GameView(UIManager* ui);
};

} // namespace Editor
} // namespace GameEngine
