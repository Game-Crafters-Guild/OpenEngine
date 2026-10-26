#include "PlayMode/GameViewPointerLock.h"

#include "PlayMode/GameViewPointerLockRule.h"
#include "PlayMode/WindowCursorLock.h"

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Platform/Window.h"

namespace GameEngine::Editor
{

GameViewPointerLockState NextGameViewPointerLockState(GameViewPointerLockState state,
                                                      const GameViewPointerLockInputs& inputs)
{
    // Leaving the window while locked is handing the pointer back, as Escape
    // is: coming back to the editor by a click on another panel must not
    // capture the pointer before that click lands.
    if (state.Locked && (inputs.EscapePressed || !inputs.WindowFocused))
        state.HandedBack = true;
    if (inputs.GameViewPressed)
        state.HandedBack = false;
    if (!inputs.Playing || !inputs.Requested)
        state.HandedBack = false;

    state.Locked = inputs.Playing && inputs.Requested && inputs.GameViewActive && inputs.WindowFocused &&
                   !state.HandedBack;
    return state;
}

std::optional<Platform::Window::LockedCursorMotion> ResolveWindowCursorLock(WindowCursorLockRequests requests)
{
    if (requests.Game)
        return Platform::Window::LockedCursorMotion::Raw;
    if (requests.UiDrag)
        return Platform::Window::LockedCursorMotion::Accelerated;
    return std::nullopt;
}

void GameViewPointerLock::Update(Input::InputSystem& runtimeInput, WindowCursorLock* gameViewCursorLock, bool playing)
{
    GameViewPointerLockInputs inputs;
    inputs.Playing = playing;
    inputs.Requested = runtimeInput.GetPointerLockState() != Input::PointerLockState::Unlocked;
    inputs.GameViewActive = gameViewCursorLock != nullptr;
    inputs.WindowFocused = gameViewCursorLock && gameViewCursorLock->IsWindowFocused();
    inputs.EscapePressed = m_EscapePressed;
    inputs.GameViewPressed = m_GameViewPressed;
    m_EscapePressed = false;
    m_GameViewPressed = false;
    // The rule starts from the lock the platform holds: where it refused one
    // (the browser build) the rule sees it unlocked, so a focus loss is not
    // taken for a hand-back.
    GameViewPointerLockState state;
    state.Locked = m_LockedCursor != nullptr;
    state.HandedBack = m_HandedBack;
    state = NextGameViewPointerLockState(state, inputs);
    m_HandedBack = state.HandedBack;

    WindowCursorLock* target = state.Locked ? gameViewCursorLock : nullptr;
    if (target != m_LockedCursor)
    {
        if (m_LockedCursor)
            m_LockedCursor->UnlockForGame();
        m_LockedCursor = nullptr;
        if (target)
        {
            target->LockForGame();
            if (target->IsLocked())
                m_LockedCursor = target;
            else
                target->UnlockForGame();
        }
    }
    Input::IRawInputSink& sink = runtimeInput;
    sink.OnPointerLock(m_LockedCursor != nullptr);
}

void GameViewPointerLock::OnKey(int key, int action)
{
    if (key == Input::kKeyCode_Escape && action == Input::kKeyActionPress)
        m_EscapePressed = true;
}

void GameViewPointerLock::OnGameViewPressed()
{
    m_GameViewPressed = true;
}

void GameViewPointerLock::ForgetCursorLock(const WindowCursorLock* cursorLock)
{
    if (cursorLock && cursorLock == m_LockedCursor)
        m_LockedCursor = nullptr;
}

bool GameViewPointerLock::IsLockedTo(const WindowCursorLock* cursorLock) const
{
    return cursorLock && cursorLock == m_LockedCursor;
}

} // namespace GameEngine::Editor
