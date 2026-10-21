#pragma once

#include "Input/InputScriptApi.h"

// Standard gamepad layout (matches GLFW 3.3+ GLFW_GAMEPAD_* constants).
// Steam Deck / Xbox / Switch Pro via SDL present this mapping through glfwGetGamepadState.

namespace GameEngine::Input
{

constexpr int kGamepadAxisCount = 6;
constexpr int kGamepadButtonCount = 15;
constexpr int kMaxGamepads = 4;

} // namespace GameEngine::Input
