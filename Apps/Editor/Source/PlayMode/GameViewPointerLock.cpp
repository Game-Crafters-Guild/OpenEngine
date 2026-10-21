#include "PlayMode/GameViewPointerLock.h"

#include "PlayMode/GameViewPointerLockRule.h"

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

void GameViewPointerLock::Update(Input::InputSystem& runtimeInput, Platform::Window* gameViewWindow, bool playing)
{
    GameViewPointerLockInputs inputs;
    inputs.Playing = playing;
    inputs.Requested = runtimeInput.GetPointerLockState() != Input::PointerLockState::Unlocked;
    inputs.GameViewActive = gameViewWindow != nullptr;
    inputs.WindowFocused = gameViewWindow && gameViewWindow->IsFocused();
    inputs.EscapePressed = m_EscapePressed;
    inputs.GameViewPressed = m_GameViewPressed;
    m_EscapePressed = false;
    m_GameViewPressed = false;
    // The rule starts from the lock the platform holds: where it refused one
    // (the browser build) the rule sees it unlocked, so a focus loss is not
    // taken for a hand-back.
    GameViewPointerLockState state;
    state.Locked = m_LockedWindow != nullptr;
    state.HandedBack = m_HandedBack;
    state = NextGameViewPointerLockState(state, inputs);
    m_HandedBack = state.HandedBack;

    Platform::Window* target = state.Locked ? gameViewWindow : nullptr;
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
    Input::IRawInputSink& sink = runtimeInput;
    sink.OnPointerLock(m_LockedWindow != nullptr);
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
