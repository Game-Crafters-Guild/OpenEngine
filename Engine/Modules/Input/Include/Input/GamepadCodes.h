#pragma once

#include "Input/InputScriptApi.h"

// Standard gamepad layout (matches GLFW 3.3+ GLFW_GAMEPAD_* constants).
// On Windows the GameInput backend maps its gamepad state to this layout, and on macOS the Game
// Controller backend maps the extended gamepad profile. On Linux glfwGetGamepadState gives this
// layout through SDL's controller mappings.

namespace GameEngine::Input
{

constexpr int kGamepadAxisCount = 6;
constexpr int kGamepadButtonCount = 15;
constexpr int kMaxGamepads = 4;

} // namespace GameEngine::Input
