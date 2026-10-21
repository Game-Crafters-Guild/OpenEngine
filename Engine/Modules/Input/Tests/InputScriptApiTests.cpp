// The Input script API facade answers what the InputSystem it is handed answers, and the
// layouts the script API description computes for its records are the compiler's.
#include "Input/InputScriptApi.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Mathematics/VectorPrinting.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <fstream>
#include <string>
#include <string_view>

using namespace GameEngine::Input;
using GameEngine::Mathematics::Vector2;

namespace
{

constexpr std::int32_t kKeyA = kKeyCode_A;
constexpr std::int32_t kMouseLeft = kMouseButton_Left;
constexpr ContextId kContext = HashInput("ScriptApi.Context");
constexpr ActionId kJump = HashInput("ScriptApi.Jump");

// The record's entry in the module's committed description, by its id ("Input.ActionState").
nlohmann::json DescribedRecord(std::string_view id)
{
    std::ifstream file(GE_ENGINE_API_DESCRIPTION);
    const nlohmann::json module = nlohmann::json::parse(file);
    for (const nlohmann::json& record : module.at("records"))
        if (record.at("id").get<std::string>() == id)
            return record;
    return {};
}

std::size_t DescribedOffset(const nlohmann::json& record, std::string_view field)
{
    for (const nlohmann::json& entry : record.at("fields"))
        if (entry.at("name").get<std::string>() == field)
            return entry.at("offset").get<std::size_t>();
    return static_cast<std::size_t>(-1);
}

} // namespace

TEST(InputScriptApiTests, KeyAndButtonQueriesAnswerFromTheSystem)
{
    InputSystem input;
    input.OnKey(kKeyA, kKeyActionPress, 0);
    input.OnMouseButton(kMouseLeft, true, 0);
    input.Update(0.016f);
    EXPECT_TRUE(Script::IsKeyDown(input, Key::A));
    EXPECT_TRUE(Script::WasKeyPressed(input, Key::A));
    EXPECT_FALSE(Script::WasKeyReleased(input, Key::A));
    EXPECT_TRUE(Script::IsMouseButtonDown(input, MouseButton::Left));
    EXPECT_TRUE(Script::WasMouseButtonPressed(input, MouseButton::Left));
    EXPECT_FALSE(Script::WasMouseButtonReleased(input, MouseButton::Left));

    input.OnKey(kKeyA, kKeyActionRelease, 0);
    input.OnMouseButton(kMouseLeft, false, 0);
    input.Update(0.016f);
    EXPECT_FALSE(Script::IsKeyDown(input, Key::A));
    EXPECT_FALSE(Script::WasKeyPressed(input, Key::A));
    EXPECT_TRUE(Script::WasKeyReleased(input, Key::A));
    EXPECT_FALSE(Script::IsMouseButtonDown(input, MouseButton::Left));
    EXPECT_TRUE(Script::WasMouseButtonReleased(input, MouseButton::Left));
}

TEST(InputScriptApiTests, PointerQueriesAnswerFromTheSystem)
{
    InputSystem input;
    EXPECT_FALSE(Script::IsPointerInWindow(input));
    EXPECT_FALSE(Script::IsWindowFocused(input));

    input.OnWindowFocus(true);
    input.OnMouseMove({100.0f, 50.0f});
    input.OnMouseMove({104.0f, 47.0f});
    input.Update(0.016f);
    EXPECT_EQ(Script::GetMousePosition(input), Vector2(104.0f, 47.0f));
    EXPECT_EQ(Script::GetMouseDelta(input), Vector2(4.0f, -3.0f));
    EXPECT_TRUE(Script::IsPointerInWindow(input));
    EXPECT_TRUE(Script::IsWindowFocused(input));
}

TEST(InputScriptApiTests, PointerLockFollowsTheRequestAndTheHostsGrant)
{
    InputSystem input{SinkRole::Gameplay};
    IRawInputSink& host = input;

    Script::LockPointer(input);
    EXPECT_EQ(Script::GetPointerLockState(input), PointerLockState::Pending);
    host.OnPointerLock(true);
    EXPECT_EQ(Script::GetPointerLockState(input), PointerLockState::Locked);
    Script::UnlockPointer(input);
    EXPECT_EQ(Script::GetPointerLockState(input), PointerLockState::Unlocked);
}

TEST(InputScriptApiTests, ActionAuthoringDrivesTheActionState)
{
    InputSystem input;
    Script::RegisterAction(input, kContext, kJump, ActionKind::Button);
    Script::BindKey(input, kContext, kJump, Key::A);
    Script::PushContext(input, kContext);
    input.OnKey(kKeyA, kKeyActionPress, 0);
    input.Update(0.016f);
    EXPECT_TRUE(Script::GetActionState(input, kJump).pressed);
    EXPECT_TRUE(Script::GetActionState(input, kJump).justPressed);

    Script::SetContextEnabled(input, kContext, false);
    EXPECT_FALSE(Script::GetActionState(input, kJump).pressed);
    Script::SetContextEnabled(input, kContext, true);
    EXPECT_TRUE(Script::GetActionState(input, kJump).pressed);

    Script::ClearBindings(input, kContext, kJump);
    input.OnKey(kKeyA, kKeyActionRelease, 0);
    input.OnKey(kKeyA, kKeyActionPress, 0);
    input.Update(0.016f);
    EXPECT_FALSE(Script::GetActionState(input, kJump).pressed);

    Script::PopContext(input, kContext);
    Script::RemoveAction(input, kContext, kJump);
    EXPECT_FALSE(Script::GetActionState(input, kJump).pressed);
}

// Each typed binding reaches its device: a key with a forbidden modifier, a mouse button, and the
// pointer's motion as an axis times its scale.
TEST(InputScriptApiTests, TypedBindingsReadTheirDevice)
{
    InputSystem input;
    const ActionId fire = HashInput("ScriptApi.Fire");
    const ActionId look = HashInput("ScriptApi.Look");
    Script::RegisterAction(input, kContext, kJump, ActionKind::Button);
    Script::RegisterAction(input, kContext, fire, ActionKind::Button);
    Script::RegisterAction(input, kContext, look, ActionKind::Axis);
    Script::BindKey(input, kContext, kJump, Key::A, 1.0f, ModifierFlags::None, ModifierFlags::Control);
    Script::BindMouseButton(input, kContext, fire, MouseButton::Right);
    Script::BindMouseAxis(input, kContext, look, MouseAxis::MotionX, 2.0f);

    input.OnKey(kKeyA, kKeyActionPress, kModControl);
    input.Update(0.016f);
    EXPECT_FALSE(Script::GetActionState(input, kJump).pressed) << "Control is forbidden on the key";

    input.OnMouseButton(kMouseButton_Right, true, 0);
    input.OnMouseMove({100.0f, 50.0f});
    input.Update(0.016f);
    input.OnMouseMove({104.0f, 47.0f});
    input.Update(0.016f);
    EXPECT_TRUE(Script::GetActionState(input, fire).pressed);
    EXPECT_FLOAT_EQ(Script::GetActionState(input, look).value, 8.0f);
}

// Two keys make one axis: each held key adds its scale to the axis action's value.
TEST(InputScriptApiTests, KeysWithOppositeScalesMakeOneAxis)
{
    InputSystem input;
    const ActionId move = HashInput("ScriptApi.MoveX");
    Script::RegisterAction(input, kContext, move, ActionKind::Axis);
    Script::BindKey(input, kContext, move, Key::A, -1.0f);
    Script::BindKey(input, kContext, move, Key::D, 1.0f);

    input.OnKey(kKeyCode_A, kKeyActionPress, 0);
    input.Update(0.016f);
    EXPECT_FLOAT_EQ(Script::GetActionState(input, move).value, -1.0f);

    input.OnKey(kKeyCode_A, kKeyActionRelease, 0);
    input.OnKey(kKeyCode_D, kKeyActionPress, 0);
    input.Update(0.016f);
    EXPECT_FLOAT_EQ(Script::GetActionState(input, move).value, 1.0f);
}

TEST(InputScriptApiTests, DescribedRecordLayoutsAreTheCompilers)
{
    const nlohmann::json state = DescribedRecord("Input.ActionState");
    ASSERT_FALSE(state.is_null());
    EXPECT_EQ(state.at("size").get<std::size_t>(), sizeof(ActionState));
    EXPECT_EQ(state.at("align").get<std::size_t>(), alignof(ActionState));
    EXPECT_EQ(DescribedOffset(state, "pressed"), offsetof(ActionState, pressed));
    EXPECT_EQ(DescribedOffset(state, "justPressed"), offsetof(ActionState, justPressed));
    EXPECT_EQ(DescribedOffset(state, "justReleased"), offsetof(ActionState, justReleased));
    EXPECT_EQ(DescribedOffset(state, "value"), offsetof(ActionState, value));
}
