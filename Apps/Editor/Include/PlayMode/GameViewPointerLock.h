#pragma once

namespace GameEngine::Input
{
class InputSystem;
}

namespace GameEngine::Editor
{
class WindowCursorLock;

/// Grants the game's pointer lock request (Input::InputSystem::LockPointer)
/// inside the editor, and applies the outcome to the window's cursor lock. The game
/// reads the outcome from Input::InputSystem::GetPointerLockState.
class GameViewPointerLock
{
  public:
    /// Once a frame, after the session's input state for the frame is settled.
    /// gameViewCursorLock is the cursor lock of the window whose Game View is the active tab, or null.
    void Update(Input::InputSystem& runtimeInput, WindowCursorLock* gameViewCursorLock, bool playing);

    /// Every key an editor window receives, before it is routed. Never consumes.
    void OnKey(int key, int action);

    /// A press on the Game View's viewport.
    void OnGameViewPressed();

    /// The window of this cursor lock is about to be destroyed; the lock is never touched again.
    void ForgetCursorLock(const WindowCursorLock* cursorLock);

    /// Whether the game holds the pointer in this cursor lock's window now.
    bool IsLockedTo(const WindowCursorLock* cursorLock) const;

  private:
    // Null while unlocked: the rule's Locked state is m_LockedCursor != nullptr.
    WindowCursorLock* m_LockedCursor = nullptr;
    // The pointer was handed back to the editor while the game still asks.
    bool m_HandedBack = false;
    // Events since the last Update, read by the next one.
    bool m_EscapePressed = false;
    bool m_GameViewPressed = false;
};

} // namespace GameEngine::Editor
