// The Windows backend's gamepad reading: a GameInput gamepad state stored in the GamepadAxis and
// GamepadButton layout.
#include "Gamepad/GameInputReading.h"
#include "Input/GamepadFrame.h"
#include "Input/InputScriptApi.h"

#include <gtest/gtest.h>

namespace GameEngine::Input
{
namespace
{

float Axis(const GamepadFrame::Slot& slot, GamepadAxis axis)
{
    return slot.axes[static_cast<int>(axis)];
}

// GameInput reads a stick +Y up, as GamepadAxis does: the sticks pass through with their sign.
TEST(GameInputGamepadStateTests, SticksPushedUpReadPositiveYAndTheXAxesAndTriggersPassThrough)
{
    GameInput::v3::GameInputGamepadState state{};
    state.leftThumbstickX = 0.25f;
    state.leftThumbstickY = 1.0f;
    state.rightThumbstickX = -0.75f;
    state.rightThumbstickY = 0.5f;
    state.leftTrigger = 0.4f;
    state.rightTrigger = 1.0f;

    GamepadFrame::Slot slot;
    StoreGamepadState(state, slot);

    EXPECT_TRUE(slot.connected);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::LeftY), 1.0f);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::RightY), 0.5f);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::LeftX), 0.25f);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::RightX), -0.75f);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::LeftTrigger), 0.4f);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::RightTrigger), 1.0f);
}

} // namespace
} // namespace GameEngine::Input
