#pragma once

#include "Mathematics/Vector2.h"
#include "Types/StringId.h"

#include <cstdint>

// The Input module's script API: the declarations marked // @ge-script are what a script in
// any language can call (docs/contributing/script-api-markers.mdx). Native code calls the
// same functions. The types marked here are the module's own: InputSystem uses them too.
namespace GameEngine::Input
{

class InputSystem;

/// A key of the keyboard, by its place on a US layout: Key.Q is the key left of W on every layout.
/// World1 and World2 are the two extra keys some non-US layouts have.
// @ge-script
enum class Key : std::int32_t
{
    LeftShift = 340,
    LeftControl = 341,
    LeftAlt = 342,
    LeftSuper = 343,
    RightShift = 344,
    RightControl = 345,
    RightAlt = 346,
    RightSuper = 347,
    A = 65,
    B = 66,
    C = 67,
    D = 68,
    E = 69,
    F = 70,
    G = 71,
    H = 72,
    I = 73,
    J = 74,
    K = 75,
    L = 76,
    M = 77,
    N = 78,
    O = 79,
    P = 80,
    Q = 81,
    R = 82,
    S = 83,
    T = 84,
    U = 85,
    V = 86,
    W = 87,
    X = 88,
    Y = 89,
    Z = 90,
    Digit0 = 48,
    Digit1 = 49,
    Digit2 = 50,
    Digit3 = 51,
    Digit4 = 52,
    Digit5 = 53,
    Digit6 = 54,
    Digit7 = 55,
    Digit8 = 56,
    Digit9 = 57,
    F1 = 290,
    F2 = 291,
    F3 = 292,
    F4 = 293,
    F5 = 294,
    F6 = 295,
    F7 = 296,
    F8 = 297,
    F9 = 298,
    F10 = 299,
    F11 = 300,
    F12 = 301,
    Up = 265,
    Down = 264,
    Left = 263,
    Right = 262,
    Home = 268,
    End = 269,
    PageUp = 266,
    PageDown = 267,
    Insert = 260,
    Space = 32,
    Escape = 256,
    Enter = 257,
    Tab = 258,
    Backspace = 259,
    Delete = 261,
    CapsLock = 280,
    ScrollLock = 281,
    NumLock = 282,
    PrintScreen = 283,
    Pause = 284,
    Apostrophe = 39,
    Comma = 44,
    Minus = 45,
    Period = 46,
    Slash = 47,
    Semicolon = 59,
    Equal = 61,
    LeftBracket = 91,
    Backslash = 92,
    RightBracket = 93,
    GraveAccent = 96,
    World1 = 161,
    World2 = 162,
    NumPad0 = 320,
    NumPad1 = 321,
    NumPad2 = 322,
    NumPad3 = 323,
    NumPad4 = 324,
    NumPad5 = 325,
    NumPad6 = 326,
    NumPad7 = 327,
    NumPad8 = 328,
    NumPad9 = 329,
    NumPadDecimal = 330,
    NumPadDivide = 331,
    NumPadMultiply = 332,
    NumPadSubtract = 333,
    NumPadAdd = 334,
    NumPadEnter = 335,
    NumPadEqual = 336,
};

/// A button of the mouse. Back and Forward are the two side buttons; Button6 to Button8 are the
/// extra buttons some mice have.
// @ge-script
enum class MouseButton : std::int32_t
{
    Left = 0,
    Right = 1,
    Middle = 2,
    Back = 3,
    Forward = 4,
    Button6 = 5,
    Button7 = 6,
    Button8 = 7,
};

/// What a mouse axis binding reads: the pointer's motion over the frame, or the wheel's.
// @ge-script
enum class MouseAxis : std::int32_t
{
    MotionX = -1,
    MotionY = -2,
    ScrollX = -3,
    ScrollY = -4,
};

/// A button of a gamepad, by its place on an Xbox-style pad: A is the bottom face button.
// @ge-script
enum class GamepadButton : std::int32_t
{
    A = 0,
    B = 1,
    X = 2,
    Y = 3,
    LeftBumper = 4,
    RightBumper = 5,
    Back = 6,
    Start = 7,
    Guide = 8,
    LeftThumb = 9,
    RightThumb = 10,
    DpadUp = 11,
    DpadRight = 12,
    DpadDown = 13,
    DpadLeft = 14,
};

/// A stick or trigger axis of a gamepad. A stick reads -1 to 1, a trigger 0 to 1.
// @ge-script
enum class GamepadAxis : std::int32_t
{
    LeftX = 0,
    LeftY = 1,
    RightX = 2,
    RightY = 3,
    LeftTrigger = 4,
    RightTrigger = 5,
};

/// The modifier keys held with a key. They combine with |, as in a binding's requiredMods and
/// forbiddenMods.
// @ge-script @ge-script-flags
enum class ModifierFlags : std::int32_t
{
    None = 0,
    Shift = 0x0001,
    Control = 0x0002,
    Alt = 0x0004,
    Super = 0x0008,
    CapsLock = 0x0010,
    NumLock = 0x0020,
};

constexpr ModifierFlags operator|(ModifierFlags a, ModifierFlags b)
{
    return static_cast<ModifierFlags>(static_cast<std::int32_t>(a) | static_cast<std::int32_t>(b));
}

constexpr ModifierFlags operator&(ModifierFlags a, ModifierFlags b)
{
    return static_cast<ModifierFlags>(static_cast<std::int32_t>(a) & static_cast<std::int32_t>(b));
}

constexpr ModifierFlags operator~(ModifierFlags a)
{
    return static_cast<ModifierFlags>(~static_cast<std::int32_t>(a));
}

/// The kind of device an action binding reads.
enum class DeviceType : std::uint8_t
{
    Keyboard,
    Mouse,
    Gamepad,
    Unknown
};

/// What an action reports: a button is pressed or released, an axis carries a value.
// @ge-script
enum class ActionKind : std::uint8_t
{
    Button,
    Axis,
};

/// The state of an action for the current frame.
// @ge-script
struct ActionState
{
    bool pressed = false;      // currently held
    bool justPressed = false;  // transitioned to pressed this frame
    bool justReleased = false; // transitioned to released this frame
    float value = 0.0f;        // for axes (e.g. -1..1, scroll delta)
};

/// One input that drives an action, with the modifier keys it needs and the ones it refuses.
/// The device says what the code is: a Key for the keyboard; a MouseButton or a MouseAxis for the
/// mouse; a GamepadButton or a GamepadAxis for a gamepad. The binding counts only while every
/// required modifier is held and no forbidden one, so forbidding Shift on A tells A from Shift+A.
/// Scripts bind through BindKey, BindMouseButton, BindMouseAxis, BindGamepadButton and
/// BindGamepadAxis, which name the device and type the code.
struct ActionBinding
{
    DeviceType device = DeviceType::Keyboard;
    int code = 0;       // a Key, a MouseButton, a MouseAxis, a GamepadButton or a GamepadAxis, by device
    float scale = 1.0f; // for analog axes
    ModifierFlags requiredMods = ModifierFlags::None;
    ModifierFlags forbiddenMods = ModifierFlags::None;
};

/// Where the game's pointer lock stands.
// @ge-script
enum class PointerLockState : std::uint8_t
{
    // The game does not ask for the lock.
    Unlocked,
    // The game asks and the host has not granted it.
    Pending,
    // The host holds the pointer locked for the game.
    Locked,
};

} // namespace GameEngine::Input

namespace GameEngine::Input::Script
{

/// True while the key is held down.
// @ge-script @ge-context @ge-engine-thread
bool IsKeyDown(InputSystem& input, Key key);

/// True in the frame the key went down.
// @ge-script @ge-context @ge-engine-thread
bool WasKeyPressed(InputSystem& input, Key key);

/// True in the frame the key came up.
// @ge-script @ge-context @ge-engine-thread
bool WasKeyReleased(InputSystem& input, Key key);

/// Where the last move put the pointer, in pixels of the play surface, with the origin at the
/// top-left corner and +Y down. It is (0, 0) before the first move. It stays where it is when
/// the pointer leaves the window or the window loses focus: read IsPointerInWindow and
/// IsWindowFocused before you act on it.
// @ge-script @ge-context @ge-engine-thread
Mathematics::Vector2 GetMousePosition(InputSystem& input);

/// True from the first move into the window until the pointer leaves it.
// @ge-script @ge-context @ge-engine-thread
bool IsPointerInWindow(InputSystem& input);

/// True while the window that feeds the game has focus.
// @ge-script @ge-context @ge-engine-thread
bool IsWindowFocused(InputSystem& input);

/// True while the mouse button is held down.
// @ge-script @ge-context @ge-engine-thread
bool IsMouseButtonDown(InputSystem& input, MouseButton button);

/// True in the frame the mouse button went down, also when it came up again in the same frame.
// @ge-script @ge-context @ge-engine-thread
bool WasMouseButtonPressed(InputSystem& input, MouseButton button);

/// True in the frame the mouse button came up, also when it went down in the same frame.
// @ge-script @ge-context @ge-engine-thread
bool WasMouseButtonReleased(InputSystem& input, MouseButton button);

/// How far the pointer moved over the last frame. Unlocked, the units are the pixels of
/// GetMousePosition. Locked, the delta keeps counting however far the mouse travels, in the
/// window's cursor units: raw device counts where the platform gives raw motion, otherwise
/// cursor pixels.
// @ge-script @ge-context @ge-engine-thread
Mathematics::Vector2 GetMouseDelta(InputSystem& input);

/// Asks the host to hide the cursor and hold the pointer in the window, so the mouse steers
/// the game. The request stays until UnlockPointer or the end of the session.
// @ge-script @ge-context @ge-engine-thread
void LockPointer(InputSystem& input);

/// Withdraws the pointer lock request.
// @ge-script @ge-context @ge-engine-thread
void UnlockPointer(InputSystem& input);

/// Where the pointer lock stands: Unlocked while the game does not ask, Pending while it
/// asks and the host has not granted the lock, Locked while the host holds the pointer.
// @ge-script @ge-context @ge-engine-thread
PointerLockState GetPointerLockState(InputSystem& input);

/// The state of an action this frame, from the enabled contexts that hold it.
// @ge-script @ge-context @ge-engine-thread
ActionState GetActionState(InputSystem& input, StringId action);

/// Adds an action to a context. An action of an enabled context is live at once.
// @ge-script @ge-context @ge-engine-thread
void RegisterAction(InputSystem& input, StringId context, StringId action, ActionKind kind);

/// Removes an action and its bindings from a context.
// @ge-script @ge-context @ge-engine-thread
void RemoveAction(InputSystem& input, StringId context, StringId action);

/// Removes every binding of an action.
// @ge-script @ge-context @ge-engine-thread
void ClearBindings(InputSystem& input, StringId context, StringId action);

/// Binds a key to an action. On an axis action the held key adds the scale to the action's value,
/// so A at -1 and D at +1 make one axis. The binding counts while every required modifier is held
/// and no forbidden one: forbid Control on S to tell S from Control+S.
// @ge-script @ge-context @ge-engine-thread
void BindKey(InputSystem& input, StringId context, StringId action, Key key, float scale = 1.0f,
             ModifierFlags requiredMods = ModifierFlags::None, ModifierFlags forbiddenMods = ModifierFlags::None);

/// Binds a mouse button to an action, with the scale and the modifiers as BindKey takes them.
// @ge-script @ge-context @ge-engine-thread
void BindMouseButton(InputSystem& input, StringId context, StringId action, MouseButton button, float scale = 1.0f,
                     ModifierFlags requiredMods = ModifierFlags::None, ModifierFlags forbiddenMods = ModifierFlags::None);

/// Binds the pointer's motion or the wheel to an axis action: the action's value is the movement
/// over the frame times the scale.
// @ge-script @ge-context @ge-engine-thread
void BindMouseAxis(InputSystem& input, StringId context, StringId action, MouseAxis axis, float scale = 1.0f);

/// Binds a gamepad button to a button action. An axis action reads a gamepad's axes only: bind a
/// stick or trigger with BindGamepadAxis.
// @ge-script @ge-context @ge-engine-thread
void BindGamepadButton(InputSystem& input, StringId context, StringId action, GamepadButton button);

/// Binds a gamepad stick or trigger to an axis action: the action's value is the axis times the
/// scale.
// @ge-script @ge-context @ge-engine-thread
void BindGamepadAxis(InputSystem& input, StringId context, StringId action, GamepadAxis axis, float scale = 1.0f);

/// Puts a context on top of the context stack, which gives its actions priority.
// @ge-script @ge-context @ge-engine-thread
void PushContext(InputSystem& input, StringId context);

/// Removes a context from the context stack.
// @ge-script @ge-context @ge-engine-thread
void PopContext(InputSystem& input, StringId context);

/// Turns a context's actions on or off.
// @ge-script @ge-context @ge-engine-thread
void SetContextEnabled(InputSystem& input, StringId context, bool enabled);

} // namespace GameEngine::Input::Script
