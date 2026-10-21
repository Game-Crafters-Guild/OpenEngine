// The Input script API through its generated table, end to end on the runtime sink: the calls the
// C# Input class makes (Managed/Scripting.Bindings/Generated/Input.g.cs), so the chain a script
// actually gets is what is pinned here rather than the C++ API underneath it.
#include "gtest/gtest.h"

#include "Core/Engine.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Scripting/ScriptingABI.h"
#include "scripting/Input.script.gen.h"

namespace
{
using GameEngine::Input::ActionKind;
using GameEngine::Input::ActionState;
using GameEngine::Input::Key;
using GameEngine::Input::ModifierFlags;
using GameEngine::Input::PointerLockState;

// Ids the way a script makes them (StringId from a name).
constexpr uint64_t kScriptContext = GameEngine::Input::HashInput("Script.Gameplay");
constexpr uint64_t kMoveForward = GameEngine::Input::HashInput("Script.Gameplay.MoveForward");

// Points the scripts' input at a sink this test owns, and puts the engine back afterwards so no
// other suite inherits it.
struct ScopedRuntimeInput
{
    explicit ScopedRuntimeInput(GameEngine::Input::InputSystem& sink)
    {
        GameEngine::EngineCore::GetInstance().SetRuntimeInput(&sink);
    }
    ~ScopedRuntimeInput() { GameEngine::EngineCore::GetInstance().SetRuntimeInput(nullptr); }
};

const GE_Input_Table_v1& InputTable()
{
    const void* table = nullptr;
    uint32_t size = 0;
    EXPECT_EQ(GE_Input_GetInterface(GE_Input_Hash, &table, &size), GE_Result_Ok);
    EXPECT_EQ(size, sizeof(GE_Input_Table_v1));
    return *static_cast<const GE_Input_Table_v1*>(table);
}

} // namespace

TEST(ScriptingAbi_Input, BoundActionIsReadableWithoutPushingAContext)
{
    GameEngine::Input::InputSystem sink{GameEngine::Input::SinkRole::Gameplay};
    ScopedRuntimeInput scoped(sink);
    const GE_Input_Table_v1& input = InputTable();

    ASSERT_EQ(input.RegisterAction(kScriptContext, kMoveForward, ActionKind::Button), GE_Result_Ok);
    ASSERT_EQ(input.BindKey(kScriptContext, kMoveForward, Key::W, 1.0f, ModifierFlags::None, ModifierFlags::None), GE_Result_Ok);

    // The binding claims the key, so the chain stops here and nothing downstream
    // sees the press. The action it was claimed for must therefore be live.
    EXPECT_TRUE(sink.OnKey(GameEngine::Input::kKeyCode_W, GameEngine::Input::kKeyActionPress, /*mods=*/0));
    sink.Update(0.016f);

    ActionState state{};
    ASSERT_EQ(input.GetActionState(kMoveForward, &state), GE_Result_Ok);
    EXPECT_TRUE(state.pressed) << "a claimed key must not read as a dead action";
    EXPECT_TRUE(state.justPressed);

    sink.OnKey(GameEngine::Input::kKeyCode_W, GameEngine::Input::kKeyActionRelease, /*mods=*/0);
    sink.Update(0.016f);

    ASSERT_EQ(input.GetActionState(kMoveForward, &state), GE_Result_Ok);
    EXPECT_FALSE(state.pressed);
    EXPECT_TRUE(state.justReleased);
}

TEST(ScriptingAbi_Input, DisablingTheContextWithdrawsTheClaimAndTheAction)
{
    GameEngine::Input::InputSystem sink{GameEngine::Input::SinkRole::Gameplay};
    ScopedRuntimeInput scoped(sink);
    const GE_Input_Table_v1& input = InputTable();

    ASSERT_EQ(input.RegisterAction(kScriptContext, kMoveForward, ActionKind::Button), GE_Result_Ok);
    ASSERT_EQ(input.BindKey(kScriptContext, kMoveForward, Key::W, 1.0f, ModifierFlags::None, ModifierFlags::None), GE_Result_Ok);
    ASSERT_EQ(input.SetContextEnabled(kScriptContext, /*enabled=*/0), GE_Result_Ok);

    // Both answers are judged on the same enabled contexts: a script that switches
    // its context off stops claiming the key and stops reading the action.
    EXPECT_FALSE(sink.OnKey(GameEngine::Input::kKeyCode_W, GameEngine::Input::kKeyActionPress, /*mods=*/0));
    sink.Update(0.016f);

    ActionState state{};
    ASSERT_EQ(input.GetActionState(kMoveForward, &state), GE_Result_Ok);
    EXPECT_FALSE(state.pressed);
}

// A StringId of 0 names no context or action: refused, and nothing is registered under id 0.
TEST(ScriptingAbi_Input, IdZeroIsRefused)
{
    GameEngine::Input::InputSystem sink{GameEngine::Input::SinkRole::Gameplay};
    ScopedRuntimeInput scoped(sink);
    const GE_Input_Table_v1& input = InputTable();
    ActionState state{};

    EXPECT_EQ(input.RegisterAction(0, kMoveForward, ActionKind::Button), GE_Result_InvalidArg);
    EXPECT_EQ(input.RegisterAction(kScriptContext, 0, ActionKind::Button), GE_Result_InvalidArg);
    EXPECT_EQ(input.GetActionState(0, &state), GE_Result_InvalidArg);
    EXPECT_EQ(input.PushContext(0), GE_Result_InvalidArg);
}

// The position is always readable — (0,0) before the pointer first reaches the
// window — and the two states say whether it is one to act on, so a script
// neither unwraps a missing value nor mistakes the origin for a real corner.
TEST(ScriptingAbi_Input, PointerStatesSayWhetherThePositionIsOneToActOn)
{
    GameEngine::Input::InputSystem sink{GameEngine::Input::SinkRole::Gameplay};
    ScopedRuntimeInput scoped(sink);
    const GE_Input_Table_v1& input = InputTable();

    GameEngine::Mathematics::Vector2 position{-7.0f, -7.0f};
    uint8_t inWindow = 42;
    uint8_t focused = 42;
    ASSERT_EQ(input.GetMousePosition(&position), GE_Result_Ok);
    EXPECT_EQ(position.x, 0.0f);
    EXPECT_EQ(position.y, 0.0f);
    ASSERT_EQ(input.IsPointerInWindow(&inWindow), GE_Result_Ok);
    EXPECT_EQ(inWindow, 0);
    ASSERT_EQ(input.IsWindowFocused(&focused), GE_Result_Ok);
    EXPECT_EQ(focused, 0);

    sink.OnMouseMove({12.0f, 34.0f});
    sink.OnWindowFocus(true);
    ASSERT_EQ(input.GetMousePosition(&position), GE_Result_Ok);
    EXPECT_EQ(position.x, 12.0f);
    EXPECT_EQ(position.y, 34.0f);
    ASSERT_EQ(input.IsPointerInWindow(&inWindow), GE_Result_Ok);
    EXPECT_EQ(inWindow, 1);
    ASSERT_EQ(input.IsWindowFocused(&focused), GE_Result_Ok);
    EXPECT_EQ(focused, 1);

    // Leaving and losing focus clear the states; the last position stands.
    sink.OnMouseLeave();
    sink.OnWindowFocus(false);
    ASSERT_EQ(input.GetMousePosition(&position), GE_Result_Ok);
    EXPECT_EQ(position.x, 12.0f);
    EXPECT_EQ(position.y, 34.0f);
    ASSERT_EQ(input.IsPointerInWindow(&inWindow), GE_Result_Ok);
    EXPECT_EQ(inWindow, 0);
    ASSERT_EQ(input.IsWindowFocused(&focused), GE_Result_Ok);
    EXPECT_EQ(focused, 0);

    EXPECT_EQ(input.IsPointerInWindow(nullptr), GE_Result_InvalidArg);
    EXPECT_EQ(input.IsWindowFocused(nullptr), GE_Result_InvalidArg);
}

// A script asks for the pointer lock, reads Pending until the host grants it, and Locked while the
// host holds it; withdrawing the request reads Unlocked.
TEST(ScriptingAbi_Input, PointerLockFollowsTheScriptsRequestAndTheHostsGrant)
{
    GameEngine::Input::InputSystem sink{GameEngine::Input::SinkRole::Gameplay};
    ScopedRuntimeInput scoped(sink);
    const GE_Input_Table_v1& input = InputTable();
    PointerLockState state = PointerLockState::Locked;

    ASSERT_EQ(input.GetPointerLockState(&state), GE_Result_Ok);
    EXPECT_EQ(state, PointerLockState::Unlocked);

    ASSERT_EQ(input.LockPointer(), GE_Result_Ok);
    ASSERT_EQ(input.GetPointerLockState(&state), GE_Result_Ok);
    EXPECT_EQ(state, PointerLockState::Pending);

    static_cast<GameEngine::Input::IRawInputSink&>(sink).OnPointerLock(true);
    ASSERT_EQ(input.GetPointerLockState(&state), GE_Result_Ok);
    EXPECT_EQ(state, PointerLockState::Locked);

    ASSERT_EQ(input.UnlockPointer(), GE_Result_Ok);
    ASSERT_EQ(input.GetPointerLockState(&state), GE_Result_Ok);
    EXPECT_EQ(state, PointerLockState::Unlocked);
}

// A binding built from another Input description is refused, never served.
TEST(ScriptingAbi_Input, TableForAnotherHashIsRefused)
{
    const void* table = &kScriptContext;
    uint32_t size = 7;

    EXPECT_EQ(GE_Input_GetInterface(GE_Input_Hash + 1, &table, &size), GE_Result_InvalidArg);
    EXPECT_EQ(table, nullptr);
    EXPECT_EQ(size, 0u);
}
