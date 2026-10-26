#pragma once

#include "Platform/Window.h"

#include <optional>

namespace GameEngine::Editor
{

/// What the grant rule reads in a frame.
struct GameViewPointerLockInputs
{
    /// Play runs and is not paused.
    bool Playing = false;
    /// The game asks for the lock (Input::InputSystem::LockPointer).
    bool Requested = false;
    /// A window shows the Game View as its active tab.
    bool GameViewActive = false;
    /// That window has focus.
    bool WindowFocused = false;
    /// Escape was pressed in an editor window since the last frame.
    bool EscapePressed = false;
    /// The Game View's viewport was pressed since the last frame.
    bool GameViewPressed = false;
};

/// The grant rule's state, carried from frame to frame.
struct GameViewPointerLockState
{
    /// The pointer is locked to the Game View's window.
    bool Locked = false;
    /// The editor holds the pointer while the game still asks: Escape, or the
    /// window losing focus while locked, gave it back, and only a press on the
    /// Game View returns it to the game.
    bool HandedBack = false;
};

/// One frame of the grant rule. The pointer is locked while play runs unpaused,
/// the game asks, the Game View is the active tab of a focused window and the
/// pointer was not handed back. Escape or a focus loss while locked hands it
/// back; a press on the Game View returns it. A withdrawn request, a pause or a
/// stop ends the hand-back, so a new request, or resuming play, is a return to
/// the game.
GameViewPointerLockState NextGameViewPointerLockState(GameViewPointerLockState state,
                                                      const GameViewPointerLockInputs& inputs);

/// What asks for an editor window's cursor lock in a frame.
struct WindowCursorLockRequests
{
    /// The game holds the pointer in this window (GameViewPointerLock granted it).
    bool Game = false;
    /// The window's UI holds a value drag (UIManager::LockCapturedPointer).
    bool UiDrag = false;
};

/// The lock an editor window holds: the game's Raw motion first, then a UI drag's
/// Accelerated motion, else none. So a UI release while the game holds the lock keeps it.
std::optional<Platform::Window::LockedCursorMotion> ResolveWindowCursorLock(WindowCursorLockRequests requests);

} // namespace GameEngine::Editor
