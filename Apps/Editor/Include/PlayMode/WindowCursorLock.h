#pragma once

#include "Platform/Window.h"

#include <optional>

namespace GameEngine::Editor
{

/// The one owner of an editor window's cursor lock. The game's lock and a UI
/// drag's lock ask here, never the window directly; the window holds the lock
/// that ResolveWindowCursorLock picks from both.
class WindowCursorLock
{
  public:
    explicit WindowCursorLock(Platform::Window& window);

    /// The game holds the pointer in this window (GameViewPointerLock).
    void LockForGame();
    void UnlockForGame();

    /// The window's UI holds a value drag (UIManager::LockCapturedPointer).
    void LockForUiDrag();
    void UnlockForUiDrag();

    /// Whether the window holds a lock now. The browser build never does.
    bool IsLocked() const;

    /// Whether the window has the keyboard focus.
    bool IsWindowFocused() const;

  private:
    // Applies the resolved lock to the window when it differs from the applied one.
    void Apply();

    Platform::Window& m_Window;
    bool m_GameRequested = false;
    bool m_UiDragRequested = false;
    // The motion the window holds, or none while it is unlocked.
    std::optional<Platform::Window::LockedCursorMotion> m_Applied;
};

} // namespace GameEngine::Editor
