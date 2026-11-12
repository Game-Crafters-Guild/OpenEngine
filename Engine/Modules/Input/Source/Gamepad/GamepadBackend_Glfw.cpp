// The gamepad backend on Linux and in the browser. GLFW finds joysticks on its first joystick
// call, on the main thread, so Start does nothing and the first Read pays for discovery.
#include "Input/Gamepad/GamepadBackend.h"
#include "Input/GamepadCodes.h"
#include "Input/GamepadFrame.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <memory>

#include <GLFW/glfw3.h>

namespace GameEngine::Input
{
namespace
{

// GLFW's mapped read and the browser's standard layout read a stick +Y down; the raw read takes a
// device's Y as +Y down too. GamepadAxis reads it +Y up. Only the axes the device reported are
// negated.
void NegateStickY(GamepadFrame::Slot& slot, int axisCount)
{
    for (const GamepadAxis stickY : {GamepadAxis::LeftY, GamepadAxis::RightY})
    {
        const int index = static_cast<int>(stickY);
        if (index < axisCount)
            slot.axes[index] = -slot.axes[index];
    }
}

#ifndef __EMSCRIPTEN__
// GamepadAxis and GamepadButton enumerate GLFW's mapped gamepad layout.
static_assert(kGamepadAxisCount == GLFW_GAMEPAD_AXIS_LAST + 1, "GamepadAxis must match GLFW's mapped axes");
static_assert(kGamepadButtonCount == GLFW_GAMEPAD_BUTTON_LAST + 1, "GamepadButton must match GLFW's mapped buttons");

// The mapped read: GLFW's copy of the community mapping database normalizes the device.
bool ReadMappedJoystick(int joystickId, GamepadFrame::Slot& slot)
{
    GLFWgamepadstate state{};
    if (!glfwGetGamepadState(joystickId, &state))
        return false;
    slot.connected = true;
    for (int i = 0; i < kGamepadAxisCount; ++i)
        slot.axes[i] = state.axes[i];
    NegateStickY(slot, kGamepadAxisCount);
    for (int i = 0; i < kGamepadButtonCount; ++i)
        slot.buttons[i] = state.buttons[i] != 0;
    return true;
}
#else
// Emscripten's GLFW declares glfwGetGamepadState but does not implement it: the browser's pads
// read through the raw joystick API, which the browser presents in the standard layout.
bool ReadMappedJoystick(int, GamepadFrame::Slot&)
{
    return false;
}
#endif

// The raw read, for a joystick with no mapping entry (the Steam Deck's desktop mode, the web).
// Triggers arrive as -1 to 1 and are moved to 0 to 1.
void ReadRawJoystick(int joystickId, GamepadFrame::Slot& slot)
{
    int axisCount = 0;
    const float* axes = glfwGetJoystickAxes(joystickId, &axisCount);
    if (!axes || axisCount < 2)
        return;

    slot.connected = true;
    const int axisCopy = std::min(axisCount, kGamepadAxisCount);
    for (int i = 0; i < axisCopy; ++i)
        slot.axes[i] = axes[i];
    for (const GamepadAxis trigger : {GamepadAxis::LeftTrigger, GamepadAxis::RightTrigger})
    {
        const int index = static_cast<int>(trigger);
        if (index < axisCopy)
            slot.axes[index] = (slot.axes[index] + 1.0f) * 0.5f;
    }
    NegateStickY(slot, axisCopy);

    int buttonCount = 0;
    const unsigned char* buttons = glfwGetJoystickButtons(joystickId, &buttonCount);
    const int buttonCopy = buttons ? std::min(buttonCount, kGamepadButtonCount) : 0;
    for (int i = 0; i < buttonCopy; ++i)
        slot.buttons[i] = buttons[i] != 0;
}

class GlfwGamepadBackend final : public GamepadBackend
{
  public:
    GamepadRuntimeState Start() override
    {
        Logger::Log::Info("GLFW reads the gamepads. It finds them on the first read, on the main thread.");
        return GamepadRuntimeState::Available;
    }

    void Read(GamepadFrame& frame) override
    {
        // Joystick 1 to 4 are slots 0 to 3: GLFW keeps a joystick's id while it stays connected.
        for (int slot = 0; slot < kMaxGamepads; ++slot)
        {
            const int joystickId = GLFW_JOYSTICK_1 + slot;
            if (!glfwJoystickPresent(joystickId))
                continue;
            if (!ReadMappedJoystick(joystickId, frame.slots[slot]))
                ReadRawJoystick(joystickId, frame.slots[slot]);
        }
    }

    void Stop() override {}
};

} // namespace

std::unique_ptr<GamepadBackend> CreatePlatformGamepadBackend()
{
    return std::make_unique<GlfwGamepadBackend>();
}

} // namespace GameEngine::Input
