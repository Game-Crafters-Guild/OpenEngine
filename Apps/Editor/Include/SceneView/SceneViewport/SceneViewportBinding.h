#pragma once

#include <functional>

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;
} // namespace GameEngine::Editor

namespace GameEngine
{
class SceneViewController;
class SceneViewportHost;

/// The services one Scene View viewport edits through. The world is the controller's world.
/// A viewport on a second world binds its own controller, undo service and dirty callback,
/// so its edits never reach the scene's world or its undo history. The host gets the pane's actions that span
/// panes; a pane with no host drives its camera and tools only.
struct SceneViewportBinding
{
    SceneViewController* Controller = nullptr;
    Editor::UndoRedoService* Undo = nullptr;
    Editor::EditorChangeNotifications* Notifications = nullptr;
    std::function<void()> OnDirty;
    SceneViewportHost* Host = nullptr;
};

} // namespace GameEngine
