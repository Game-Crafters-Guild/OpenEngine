#pragma once

#include "Input/GamepadFrame.h"

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gameinput.h>

namespace GameEngine::Input
{

/// Writes a GameInput gamepad reading into `slot` in the GamepadButton and GamepadAxis layout and
/// marks the slot connected.
void StoreGamepadState(const GameInput::v3::GameInputGamepadState& state, GamepadFrame::Slot& slot);

/// A switch position as the mapping database's hat mask: up 1, right 2, down 4, left 8.
std::uint8_t ToHatMask(GameInput::v3::GameInputSwitchPosition position);

} // namespace GameEngine::Input
