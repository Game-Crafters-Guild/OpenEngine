#pragma once

#include "UI/UIManager.h"
#include "Platform/Window.h"

namespace GameEngine
{

/// Sets up the UIManager's cursor callbacks for this window: the OS cursor
/// follows the hovered UI element's CSS cursor property, and a captured
/// pointer's lock (UIManager::LockCapturedPointer) hides and holds the cursor.
void SetupUICursorCallback(UIManager* ui, Platform::Window* window);

} // namespace GameEngine
