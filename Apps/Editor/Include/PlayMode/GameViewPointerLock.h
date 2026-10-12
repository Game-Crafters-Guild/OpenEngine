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

/// Grants the game's pointer lock request (Input::InputSystem::RequestPointerLock)
/// inside the editor. The pointer is locked to the window whose Game View is the
/// active tab while play runs unpaused, that window has focus and the game asks.
/// Escape hands the pointer back to the editor without taking the key from the
/// game, and a press on the Game View returns it to the game. The game reads the
/// outcome from Input::InputSystem::IsPointerLocked.
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
    Platform::Window* m_LockedWindow = nullptr;
    // Escape gave the pointer to the editor; the game's request waits for a press
    // on the Game View.
    bool m_HandedBack = false;
};

} // namespace GameEngine::Editor
