#pragma once

#include "ECS/Entity.h"

#include <functional>
#include <vector>

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

/// The Day Cycle window's dock id.
inline constexpr const char* kDayCycleWindowPanelId = "DayCycle";

/// Registers the Day Cycle window with the editor's panel registry over the editor's own services:
/// the undo history, the change notifications, the selection and whether the editor plays. A project
/// switch (EditorChangeNotifications::NotifyProjectSwitched) closes the profiles' working copies, so
/// the window never shows or saves the previous project's profile.
void RegisterDayCycleWindow(UndoRedoService* undo, EditorChangeNotifications& notifications,
                            std::function<std::vector<ECS::EntityHandle>()> selection,
                            std::function<bool()> isPlaying);

} // namespace GameEngine::Editor
