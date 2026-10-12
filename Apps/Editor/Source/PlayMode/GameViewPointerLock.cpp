#include "PlayMode/GameViewPointerLock.h"

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Platform/Window.h"

namespace GameEngine::Editor
{

void GameViewPointerLock::Update(Input::InputSystem& runtimeInput, Platform::Window* gameViewWindow, bool playing)
{
    const bool requested = playing && runtimeInput.IsPointerLockRequested();
    // A withdrawn request ends the hand-back: the next request is a new one.
    if (!requested)
        m_HandedBack = false;

    Platform::Window* target = nullptr;
    if (requested && !m_HandedBack && gameViewWindow && gameViewWindow->IsFocused())
        target = gameViewWindow;

    if (target != m_LockedWindow)
    {
        if (m_LockedWindow)
            m_LockedWindow->SetCursorLocked(false);
        m_LockedWindow = nullptr;
        if (target)
        {
            target->SetCursorLocked(true);
            if (target->IsCursorLocked())
                m_LockedWindow = target;
        }
    }
    runtimeInput.SetPointerLocked(m_LockedWindow != nullptr);
}

void GameViewPointerLock::OnKey(int key, int action)
{
    if (m_LockedWindow && key == Input::kKeyCode_Escape && action == Input::kKeyActionPress)
        m_HandedBack = true;
}

void GameViewPointerLock::OnGameViewPressed()
{
    m_HandedBack = false;
}

void GameViewPointerLock::ForgetWindow(const Platform::Window* window)
{
    if (window && window == m_LockedWindow)
        m_LockedWindow = nullptr;
}

bool GameViewPointerLock::IsLockedTo(const Platform::Window* window) const
{
    return window && window == m_LockedWindow;
}

} // namespace GameEngine::Editor
