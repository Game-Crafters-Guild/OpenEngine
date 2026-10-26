#include "PlayMode/WindowCursorLock.h"

#include "PlayMode/GameViewPointerLockRule.h"

namespace GameEngine::Editor
{

WindowCursorLock::WindowCursorLock(Platform::Window& window) : m_Window(window) {}

void WindowCursorLock::LockForGame()
{
    m_GameRequested = true;
    Apply();
}

void WindowCursorLock::UnlockForGame()
{
    m_GameRequested = false;
    Apply();
}

void WindowCursorLock::LockForUiDrag()
{
    m_UiDragRequested = true;
    Apply();
}

void WindowCursorLock::UnlockForUiDrag()
{
    m_UiDragRequested = false;
    Apply();
}

bool WindowCursorLock::IsLocked() const
{
    return m_Window.IsCursorLocked();
}

bool WindowCursorLock::IsWindowFocused() const
{
    return m_Window.IsFocused();
}

void WindowCursorLock::Apply()
{
    const std::optional<Platform::Window::LockedCursorMotion> lock =
        ResolveWindowCursorLock({.Game = m_GameRequested, .UiDrag = m_UiDragRequested});
    if (lock == m_Applied)
        return;
    if (!lock)
    {
        m_Window.UnlockCursor();
        m_Applied = std::nullopt;
        return;
    }
    m_Window.LockCursor(*lock);
    // Where the platform refuses the lock, the next request tries again.
    m_Applied = m_Window.IsCursorLocked() ? lock : std::nullopt;
}

} // namespace GameEngine::Editor
