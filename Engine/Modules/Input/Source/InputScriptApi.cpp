#include "Input/InputScriptApi.h"

#include "Input/InputSystem.h"

namespace GameEngine::Input::Script
{

bool IsKeyDown(InputSystem& input, Key key)
{
    return input.IsKeyDown(static_cast<int>(key));
}

bool WasKeyPressed(InputSystem& input, Key key)
{
    return input.WasKeyPressed(static_cast<int>(key));
}

bool WasKeyReleased(InputSystem& input, Key key)
{
    return input.WasKeyReleased(static_cast<int>(key));
}

Mathematics::Vector2 GetMousePosition(InputSystem& input)
{
    return input.GetMousePosition();
}

bool IsPointerInWindow(InputSystem& input)
{
    return input.IsPointerInWindow();
}

bool IsWindowFocused(InputSystem& input)
{
    return input.IsWindowFocused();
}

bool IsMouseButtonDown(InputSystem& input, MouseButton button)
{
    return input.IsMouseButtonDown(static_cast<int>(button));
}

bool WasMouseButtonPressed(InputSystem& input, MouseButton button)
{
    return input.WasMouseButtonPressed(static_cast<int>(button));
}

bool WasMouseButtonReleased(InputSystem& input, MouseButton button)
{
    return input.WasMouseButtonReleased(static_cast<int>(button));
}

Mathematics::Vector2 GetMouseDelta(InputSystem& input)
{
    return input.GetMouseDelta();
}

void LockPointer(InputSystem& input)
{
    input.LockPointer();
}

void UnlockPointer(InputSystem& input)
{
    input.UnlockPointer();
}

PointerLockState GetPointerLockState(InputSystem& input)
{
    return input.GetPointerLockState();
}

ActionState GetActionState(InputSystem& input, StringId action)
{
    return input.GetActionState(action);
}

void RegisterAction(InputSystem& input, StringId context, StringId action, ActionKind kind)
{
    ActionDesc desc{};
    desc.id = action;
    desc.isAxis = kind == ActionKind::Axis;
    input.RegisterAction(context, desc);
}

void RemoveAction(InputSystem& input, StringId context, StringId action)
{
    input.RemoveAction(context, action);
}

void ClearBindings(InputSystem& input, StringId context, StringId action)
{
    input.ClearBindings(context, action);
}

namespace
{

void BindCode(InputSystem& input, StringId context, StringId action, DeviceType device, int code, float scale,
              ModifierFlags requiredMods, ModifierFlags forbiddenMods)
{
    ActionBinding binding;
    binding.device = device;
    binding.code = code;
    binding.scale = scale;
    binding.requiredMods = requiredMods;
    binding.forbiddenMods = forbiddenMods;
    input.BindKey(context, action, binding);
}

} // namespace

void BindKey(InputSystem& input, StringId context, StringId action, Key key, float scale, ModifierFlags requiredMods,
             ModifierFlags forbiddenMods)
{
    BindCode(input, context, action, DeviceType::Keyboard, static_cast<int>(key), scale, requiredMods, forbiddenMods);
}

void BindMouseButton(InputSystem& input, StringId context, StringId action, MouseButton button, float scale,
                     ModifierFlags requiredMods, ModifierFlags forbiddenMods)
{
    BindCode(input, context, action, DeviceType::Mouse, static_cast<int>(button), scale, requiredMods, forbiddenMods);
}

void BindMouseAxis(InputSystem& input, StringId context, StringId action, MouseAxis axis, float scale)
{
    BindCode(input, context, action, DeviceType::Mouse, static_cast<int>(axis), scale, ModifierFlags::None,
             ModifierFlags::None);
}

void BindGamepadButton(InputSystem& input, StringId context, StringId action, GamepadButton button)
{
    BindCode(input, context, action, DeviceType::Gamepad, static_cast<int>(button), 1.0f, ModifierFlags::None,
             ModifierFlags::None);
}

void BindGamepadAxis(InputSystem& input, StringId context, StringId action, GamepadAxis axis, float scale)
{
    BindCode(input, context, action, DeviceType::Gamepad, static_cast<int>(axis), scale, ModifierFlags::None,
             ModifierFlags::None);
}

void PushContext(InputSystem& input, StringId context)
{
    input.PushContext(context);
}

void PopContext(InputSystem& input, StringId context)
{
    input.PopContext(context);
}

void SetContextEnabled(InputSystem& input, StringId context, bool enabled)
{
    input.SetContextEnabled(context, enabled);
}

} // namespace GameEngine::Input::Script
