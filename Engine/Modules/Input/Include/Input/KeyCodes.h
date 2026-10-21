#pragma once

#include "Input/InputScriptApi.h"

// Shared key-code and modifier helpers for the input system and UI.
//
// These helpers operate on raw platform modifier bits and key codes. For now we
// assume GLFW-compatible values (e.g. Ctrl = 0x0002, Super/Command = 0x0008,
// ASCII letter key codes for A/C/V/X), but the rest of the engine should treat
// them as opaque input-layer details.
//
// NOTE: Modifier mask constants (kModShift, kModControl, kModAlt, kModSuper,
// kModCapsLock, kModNumLock) are defined in InputSystem.h as the ModifierMask
// enum. Include that header if you need modifier masks.

namespace GameEngine {
namespace Input {

// ---------------------------------------------------------------------------
// Modifier masks - defined in InputSystem.h as ModifierMask enum
// Use: #include "Input/InputSystem.h" for kModShift, kModControl, etc.
// ---------------------------------------------------------------------------

// Returns true if the given modifier mask has the "primary" shortcut modifier
// pressed. On Windows/Linux this is Ctrl; on macOS this is Command (Super).
inline bool IsPrimaryShortcutModifier(int mods)
{
    constexpr int kModControl = 0x0002;
    constexpr int kModSuper   = 0x0008;
    return (mods & (kModControl | kModSuper)) != 0;
}

// ---------------------------------------------------------------------------
// Modifier keys (physical key codes)
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_LeftShift    = static_cast<int>(Key::LeftShift); // GLFW_KEY_LEFT_SHIFT
inline constexpr int kKeyCode_LeftControl  = static_cast<int>(Key::LeftControl); // GLFW_KEY_LEFT_CONTROL
inline constexpr int kKeyCode_LeftAlt      = static_cast<int>(Key::LeftAlt); // GLFW_KEY_LEFT_ALT
inline constexpr int kKeyCode_LeftSuper    = static_cast<int>(Key::LeftSuper); // GLFW_KEY_LEFT_SUPER
inline constexpr int kKeyCode_RightShift   = static_cast<int>(Key::RightShift); // GLFW_KEY_RIGHT_SHIFT
inline constexpr int kKeyCode_RightControl = static_cast<int>(Key::RightControl); // GLFW_KEY_RIGHT_CONTROL
inline constexpr int kKeyCode_RightAlt     = static_cast<int>(Key::RightAlt); // GLFW_KEY_RIGHT_ALT
inline constexpr int kKeyCode_RightSuper   = static_cast<int>(Key::RightSuper); // GLFW_KEY_RIGHT_SUPER

// ---------------------------------------------------------------------------
// Letter keys (A-Z)
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_A = static_cast<int>(Key::A);
inline constexpr int kKeyCode_B = static_cast<int>(Key::B);
inline constexpr int kKeyCode_C = static_cast<int>(Key::C);
inline constexpr int kKeyCode_D = static_cast<int>(Key::D);
inline constexpr int kKeyCode_E = static_cast<int>(Key::E);
inline constexpr int kKeyCode_F = static_cast<int>(Key::F);
inline constexpr int kKeyCode_G = static_cast<int>(Key::G);
inline constexpr int kKeyCode_H = static_cast<int>(Key::H);
inline constexpr int kKeyCode_I = static_cast<int>(Key::I);
inline constexpr int kKeyCode_J = static_cast<int>(Key::J);
inline constexpr int kKeyCode_K = static_cast<int>(Key::K);
inline constexpr int kKeyCode_L = static_cast<int>(Key::L);
inline constexpr int kKeyCode_M = static_cast<int>(Key::M);
inline constexpr int kKeyCode_N = static_cast<int>(Key::N);
inline constexpr int kKeyCode_O = static_cast<int>(Key::O);
inline constexpr int kKeyCode_P = static_cast<int>(Key::P);
inline constexpr int kKeyCode_Q = static_cast<int>(Key::Q);
inline constexpr int kKeyCode_R = static_cast<int>(Key::R);
inline constexpr int kKeyCode_S = static_cast<int>(Key::S);
inline constexpr int kKeyCode_T = static_cast<int>(Key::T);
inline constexpr int kKeyCode_U = static_cast<int>(Key::U);
inline constexpr int kKeyCode_V = static_cast<int>(Key::V);
inline constexpr int kKeyCode_W = static_cast<int>(Key::W);
inline constexpr int kKeyCode_X = static_cast<int>(Key::X);
inline constexpr int kKeyCode_Y = static_cast<int>(Key::Y);
inline constexpr int kKeyCode_Z = static_cast<int>(Key::Z);

// ---------------------------------------------------------------------------
// Digit keys (0-9, top row)
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_0 = static_cast<int>(Key::Digit0);
inline constexpr int kKeyCode_1 = static_cast<int>(Key::Digit1);
inline constexpr int kKeyCode_2 = static_cast<int>(Key::Digit2);
inline constexpr int kKeyCode_3 = static_cast<int>(Key::Digit3);
inline constexpr int kKeyCode_4 = static_cast<int>(Key::Digit4);
inline constexpr int kKeyCode_5 = static_cast<int>(Key::Digit5);
inline constexpr int kKeyCode_6 = static_cast<int>(Key::Digit6);
inline constexpr int kKeyCode_7 = static_cast<int>(Key::Digit7);
inline constexpr int kKeyCode_8 = static_cast<int>(Key::Digit8);
inline constexpr int kKeyCode_9 = static_cast<int>(Key::Digit9);

// ---------------------------------------------------------------------------
// Function keys (F1-F12)
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_F1  = static_cast<int>(Key::F1);
inline constexpr int kKeyCode_F2  = static_cast<int>(Key::F2);
inline constexpr int kKeyCode_F3  = static_cast<int>(Key::F3);
inline constexpr int kKeyCode_F4  = static_cast<int>(Key::F4);
inline constexpr int kKeyCode_F5  = static_cast<int>(Key::F5);
inline constexpr int kKeyCode_F6  = static_cast<int>(Key::F6);
inline constexpr int kKeyCode_F7  = static_cast<int>(Key::F7);
inline constexpr int kKeyCode_F8  = static_cast<int>(Key::F8);
inline constexpr int kKeyCode_F9  = static_cast<int>(Key::F9);
inline constexpr int kKeyCode_F10 = static_cast<int>(Key::F10);
inline constexpr int kKeyCode_F11 = static_cast<int>(Key::F11);
inline constexpr int kKeyCode_F12 = static_cast<int>(Key::F12);

// ---------------------------------------------------------------------------
// Navigation keys
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_Up       = static_cast<int>(Key::Up); // GLFW_KEY_UP
inline constexpr int kKeyCode_Down     = static_cast<int>(Key::Down); // GLFW_KEY_DOWN
inline constexpr int kKeyCode_Left     = static_cast<int>(Key::Left); // GLFW_KEY_LEFT
inline constexpr int kKeyCode_Right    = static_cast<int>(Key::Right); // GLFW_KEY_RIGHT
inline constexpr int kKeyCode_Home     = static_cast<int>(Key::Home); // GLFW_KEY_HOME
inline constexpr int kKeyCode_End      = static_cast<int>(Key::End); // GLFW_KEY_END
inline constexpr int kKeyCode_PageUp   = static_cast<int>(Key::PageUp); // GLFW_KEY_PAGE_UP
inline constexpr int kKeyCode_PageDown = static_cast<int>(Key::PageDown); // GLFW_KEY_PAGE_DOWN
inline constexpr int kKeyCode_Insert   = static_cast<int>(Key::Insert); // GLFW_KEY_INSERT

// ---------------------------------------------------------------------------
// Editing / control keys
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_Space       = static_cast<int>(Key::Space);  // GLFW_KEY_SPACE
inline constexpr int kKeyCode_Escape      = static_cast<int>(Key::Escape); // GLFW_KEY_ESCAPE
inline constexpr int kKeyCode_Enter       = static_cast<int>(Key::Enter); // GLFW_KEY_ENTER
inline constexpr int kKeyCode_Tab         = static_cast<int>(Key::Tab); // GLFW_KEY_TAB
inline constexpr int kKeyCode_Backspace   = static_cast<int>(Key::Backspace); // GLFW_KEY_BACKSPACE
inline constexpr int kKeyCode_Delete      = static_cast<int>(Key::Delete); // GLFW_KEY_DELETE
inline constexpr int kKeyCode_CapsLock    = static_cast<int>(Key::CapsLock); // GLFW_KEY_CAPS_LOCK
inline constexpr int kKeyCode_ScrollLock  = static_cast<int>(Key::ScrollLock); // GLFW_KEY_SCROLL_LOCK
inline constexpr int kKeyCode_NumLock     = static_cast<int>(Key::NumLock); // GLFW_KEY_NUM_LOCK
inline constexpr int kKeyCode_PrintScreen = static_cast<int>(Key::PrintScreen); // GLFW_KEY_PRINT_SCREEN
inline constexpr int kKeyCode_Pause       = static_cast<int>(Key::Pause); // GLFW_KEY_PAUSE

// ---------------------------------------------------------------------------
// Punctuation / symbol keys
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_Apostrophe   = static_cast<int>(Key::Apostrophe);  // GLFW_KEY_APOSTROPHE (')
inline constexpr int kKeyCode_Comma        = static_cast<int>(Key::Comma);  // GLFW_KEY_COMMA (,)
inline constexpr int kKeyCode_Minus        = static_cast<int>(Key::Minus);  // GLFW_KEY_MINUS (-)
inline constexpr int kKeyCode_Period       = static_cast<int>(Key::Period);  // GLFW_KEY_PERIOD (.)
inline constexpr int kKeyCode_Slash        = static_cast<int>(Key::Slash);  // GLFW_KEY_SLASH (/)
inline constexpr int kKeyCode_Semicolon    = static_cast<int>(Key::Semicolon);  // GLFW_KEY_SEMICOLON (;)
inline constexpr int kKeyCode_Equal        = static_cast<int>(Key::Equal);  // GLFW_KEY_EQUAL (=)
inline constexpr int kKeyCode_LeftBracket  = static_cast<int>(Key::LeftBracket);  // GLFW_KEY_LEFT_BRACKET ([)
inline constexpr int kKeyCode_Backslash    = static_cast<int>(Key::Backslash);  // GLFW_KEY_BACKSLASH (\)
inline constexpr int kKeyCode_RightBracket = static_cast<int>(Key::RightBracket);  // GLFW_KEY_RIGHT_BRACKET (])
inline constexpr int kKeyCode_GraveAccent  = static_cast<int>(Key::GraveAccent);  // GLFW_KEY_GRAVE_ACCENT (`)

// The two extra printing keys non-US layouts carry beyond the ASCII block
// above (GLFW names them for the position, not the character, because which
// character they produce is the layout's business). They sit outside the
// contiguous ASCII range, so every classifier has to name them.
inline constexpr int kKeyCode_World1 = static_cast<int>(Key::World1); // GLFW_KEY_WORLD_1
inline constexpr int kKeyCode_World2 = static_cast<int>(Key::World2); // GLFW_KEY_WORLD_2

// ---------------------------------------------------------------------------
// Numpad keys
// ---------------------------------------------------------------------------
inline constexpr int kKeyCode_NumPad0        = static_cast<int>(Key::NumPad0); // GLFW_KEY_KP_0
inline constexpr int kKeyCode_NumPad1        = static_cast<int>(Key::NumPad1); // GLFW_KEY_KP_1
inline constexpr int kKeyCode_NumPad2        = static_cast<int>(Key::NumPad2); // GLFW_KEY_KP_2
inline constexpr int kKeyCode_NumPad3        = static_cast<int>(Key::NumPad3); // GLFW_KEY_KP_3
inline constexpr int kKeyCode_NumPad4        = static_cast<int>(Key::NumPad4); // GLFW_KEY_KP_4
inline constexpr int kKeyCode_NumPad5        = static_cast<int>(Key::NumPad5); // GLFW_KEY_KP_5
inline constexpr int kKeyCode_NumPad6        = static_cast<int>(Key::NumPad6); // GLFW_KEY_KP_6
inline constexpr int kKeyCode_NumPad7        = static_cast<int>(Key::NumPad7); // GLFW_KEY_KP_7
inline constexpr int kKeyCode_NumPad8        = static_cast<int>(Key::NumPad8); // GLFW_KEY_KP_8
inline constexpr int kKeyCode_NumPad9        = static_cast<int>(Key::NumPad9); // GLFW_KEY_KP_9
inline constexpr int kKeyCode_NumPadDecimal  = static_cast<int>(Key::NumPadDecimal); // GLFW_KEY_KP_DECIMAL
inline constexpr int kKeyCode_NumPadDivide   = static_cast<int>(Key::NumPadDivide); // GLFW_KEY_KP_DIVIDE
inline constexpr int kKeyCode_NumPadMultiply = static_cast<int>(Key::NumPadMultiply); // GLFW_KEY_KP_MULTIPLY
inline constexpr int kKeyCode_NumPadSubtract = static_cast<int>(Key::NumPadSubtract); // GLFW_KEY_KP_SUBTRACT
inline constexpr int kKeyCode_NumPadAdd      = static_cast<int>(Key::NumPadAdd); // GLFW_KEY_KP_ADD
inline constexpr int kKeyCode_NumPadEnter    = static_cast<int>(Key::NumPadEnter); // GLFW_KEY_KP_ENTER
inline constexpr int kKeyCode_NumPadEqual    = static_cast<int>(Key::NumPadEqual); // GLFW_KEY_KP_EQUAL

// ---------------------------------------------------------------------------
// Key classification
// ---------------------------------------------------------------------------

// A modifier's own key-down, as opposed to the modifier bits carried by some
// other key's event.
inline constexpr bool IsModifierKey(int key)
{
    return key == kKeyCode_LeftShift || key == kKeyCode_RightShift ||
           key == kKeyCode_LeftControl || key == kKeyCode_RightControl ||
           key == kKeyCode_LeftAlt || key == kKeyCode_RightAlt ||
           key == kKeyCode_LeftSuper || key == kKeyCode_RightSuper;
}

inline constexpr bool IsFunctionKey(int key)
{
    return key >= kKeyCode_F1 && key <= kKeyCode_F12;
}

// Keys the platform also delivers as a character event. A text control claims
// such a keystroke because the character it is about to receive is the thing it
// acts on — the key event and its character are one keystroke, and consuming
// only the character would leave the key itself free to reach gameplay. The
// ASCII-valued GLFW codes from space to backtick cover letters, digits and
// punctuation; the keypad block adds its digits and operators; World1/World2
// are the same thing for the keys only non-US layouts have, and missing them
// would leak that keystroke to gameplay while the field still edited on it.
inline constexpr bool ProducesTextInput(int key)
{
    if (key >= kKeyCode_Space && key <= kKeyCode_GraveAccent)
        return true;
    if (key == kKeyCode_World1 || key == kKeyCode_World2)
        return true;
    return key >= kKeyCode_NumPad0 && key <= kKeyCode_NumPadEqual && key != kKeyCode_NumPadEnter;
}

// The keystroke-level form of the above: true when this key event, under these
// modifiers, is about to arrive as a character event — the condition under
// which a focused text control claims the key. A primary-modifier chord never
// composes. Alt is the platform split: Alt composes characters on macOS
// (Option+F types 'ƒ'), so the keystroke is text and stays with the control;
// on Windows and Linux Alt+letter is a chord that composes nothing, so it must
// bubble to the binding that implements it.
inline bool ComposesTextInput(int key, int mods)
{
    if (!ProducesTextInput(key) || IsPrimaryShortcutModifier(mods))
        return false;
#if defined(__APPLE__)
    return true;
#else
    constexpr int kModAlt = 0x0004;
    return (mods & kModAlt) == 0;
#endif
}

// ---------------------------------------------------------------------------
// Mouse buttons
// ---------------------------------------------------------------------------
inline constexpr int kMouseButton_Left   = static_cast<int>(MouseButton::Left); // GLFW_MOUSE_BUTTON_LEFT
inline constexpr int kMouseButton_Right  = static_cast<int>(MouseButton::Right); // GLFW_MOUSE_BUTTON_RIGHT
inline constexpr int kMouseButton_Middle = static_cast<int>(MouseButton::Middle); // GLFW_MOUSE_BUTTON_MIDDLE

} // namespace Input
} // namespace GameEngine

