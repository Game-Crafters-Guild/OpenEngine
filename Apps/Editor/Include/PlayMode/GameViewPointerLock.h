#pragma once

namespace GameEngine::Input
{
class InputSystem;
}
namespace GameEngine::Platform
{
class Window;
}

namespace GameEngine::Editor
{

/// Grants the game's pointer lock request (Input::InputSystem::LockPointer)
/// inside the editor, and applies the outcome to the platform window. The game
/// reads the outcome from Input::InputSystem::GetPointerLockState.
class GameViewPointerLock
{
  public:
    /// Once a frame, after the session's input state for the frame is settled.
    /// gameViewWindow is the window whose Game View is the active tab, or null.
    void Update(Input::InputSystem& runtimeInput, Platform::Window* gameViewWindow, bool playing);

    /// Every key an editor window receives, before it is routed. Never consumes.
    void OnKey(int key, int action);

    /// A press on the Game View's viewport.
    void OnGameViewPressed();

    /// The window is about to be destroyed; it is never touched again.
    void ForgetWindow(const Platform::Window* window);

    /// Whether the pointer is locked to this window now.
    bool IsLockedTo(const Platform::Window* window) const;

  private:
    // Null while unlocked: the rule's Locked state is m_LockedWindow != nullptr.
    Platform::Window* m_LockedWindow = nullptr;
    // The pointer was handed back to the editor while the game still asks.
    bool m_HandedBack = false;
    // Events since the last Update, read by the next one.
    bool m_EscapePressed = false;
    bool m_GameViewPressed = false;
};

} // namespace GameEngine::Editor
