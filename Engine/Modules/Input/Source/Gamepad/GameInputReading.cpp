#include "Gamepad/GameInputReading.h"

#include "Input/GamepadCodes.h"

namespace GameEngine::Input
{
namespace
{

struct ButtonMapping
{
    GameInput::v3::GameInputGamepadButtons Source;
    GamepadButton Target;
};

// GameInput's gamepad buttons in the GamepadButton layout. The Guide button is a system button in
// GameInput, not a gamepad button, so it never reads down.
constexpr ButtonMapping kButtonMap[] = {
    {GameInput::v3::GameInputGamepadA, GamepadButton::A},
    {GameInput::v3::GameInputGamepadB, GamepadButton::B},
    {GameInput::v3::GameInputGamepadX, GamepadButton::X},
    {GameInput::v3::GameInputGamepadY, GamepadButton::Y},
    {GameInput::v3::GameInputGamepadLeftShoulder, GamepadButton::LeftBumper},
    {GameInput::v3::GameInputGamepadRightShoulder, GamepadButton::RightBumper},
    {GameInput::v3::GameInputGamepadView, GamepadButton::Back},
    {GameInput::v3::GameInputGamepadMenu, GamepadButton::Start},
    {GameInput::v3::GameInputGamepadLeftThumbstick, GamepadButton::LeftThumb},
    {GameInput::v3::GameInputGamepadRightThumbstick, GamepadButton::RightThumb},
    {GameInput::v3::GameInputGamepadDPadUp, GamepadButton::DpadUp},
    {GameInput::v3::GameInputGamepadDPadRight, GamepadButton::DpadRight},
    {GameInput::v3::GameInputGamepadDPadDown, GamepadButton::DpadDown},
    {GameInput::v3::GameInputGamepadDPadLeft, GamepadButton::DpadLeft},
};

} // namespace

void StoreGamepadState(const GameInput::v3::GameInputGamepadState& state, GamepadFrame::Slot& slot)
{
    slot.connected = true;
    // GameInput reads a stick +Y up, as GamepadAxis does.
    slot.axes[static_cast<int>(GamepadAxis::LeftX)] = state.leftThumbstickX;
    slot.axes[static_cast<int>(GamepadAxis::LeftY)] = state.leftThumbstickY;
    slot.axes[static_cast<int>(GamepadAxis::RightX)] = state.rightThumbstickX;
    slot.axes[static_cast<int>(GamepadAxis::RightY)] = state.rightThumbstickY;
    slot.axes[static_cast<int>(GamepadAxis::LeftTrigger)] = state.leftTrigger;
    slot.axes[static_cast<int>(GamepadAxis::RightTrigger)] = state.rightTrigger;
    for (const ButtonMapping& mapping : kButtonMap)
        slot.buttons[static_cast<int>(mapping.Target)] = (state.buttons & mapping.Source) != 0;
}

std::uint8_t ToHatMask(GameInput::v3::GameInputSwitchPosition position)
{
    constexpr std::uint8_t kUp = 1;
    constexpr std::uint8_t kRight = 2;
    constexpr std::uint8_t kDown = 4;
    constexpr std::uint8_t kLeft = 8;
    switch (position)
    {
    case GameInput::v3::GameInputSwitchUp:
        return kUp;
    case GameInput::v3::GameInputSwitchUpRight:
        return kUp | kRight;
    case GameInput::v3::GameInputSwitchRight:
        return kRight;
    case GameInput::v3::GameInputSwitchDownRight:
        return kDown | kRight;
    case GameInput::v3::GameInputSwitchDown:
        return kDown;
    case GameInput::v3::GameInputSwitchDownLeft:
        return kDown | kLeft;
    case GameInput::v3::GameInputSwitchLeft:
        return kLeft;
    case GameInput::v3::GameInputSwitchUpLeft:
        return kUp | kLeft;
    case GameInput::v3::GameInputSwitchCenter:
        break;
    }
    return 0;
}

} // namespace GameEngine::Input
