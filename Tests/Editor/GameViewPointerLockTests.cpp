// When the editor grants the game's pointer lock: the rule the Game View applies
// each frame, driven through every event the user meets.
#include <gtest/gtest.h>

#include "PlayMode/GameViewPointerLockRule.h"

using namespace GameEngine::Editor;

namespace
{
// Play running in the focused Game View, the game asking.
GameViewPointerLockInputs Playing()
{
    GameViewPointerLockInputs inputs;
    inputs.Playing = true;
    inputs.Requested = true;
    inputs.GameViewActive = true;
    inputs.WindowFocused = true;
    return inputs;
}

GameViewPointerLockState Step(GameViewPointerLockState state, const GameViewPointerLockInputs& inputs)
{
    return NextGameViewPointerLockState(state, inputs);
}
} // namespace

TEST(GameViewPointerLockTests, GrantedOnlyWhilePlayRunsInTheFocusedGameViewAndTheGameAsks)
{
    EXPECT_TRUE(Step({}, Playing()).Locked);

    GameViewPointerLockInputs inputs = Playing();
    inputs.Requested = false;
    EXPECT_FALSE(Step({}, inputs).Locked);
    inputs = Playing();
    inputs.Playing = false;
    EXPECT_FALSE(Step({}, inputs).Locked);
    inputs = Playing();
    inputs.GameViewActive = false;
    EXPECT_FALSE(Step({}, inputs).Locked);
    inputs = Playing();
    inputs.WindowFocused = false;
    EXPECT_FALSE(Step({}, inputs).Locked);
}

TEST(GameViewPointerLockTests, EscapeHandsThePointerBackUntilAPressOnTheGameView)
{
    GameViewPointerLockState state = Step({}, Playing());
    GameViewPointerLockInputs escape = Playing();
    escape.EscapePressed = true;
    state = Step(state, escape);
    EXPECT_FALSE(state.Locked);
    EXPECT_TRUE(state.HandedBack);

    // Frames pass and the game still asks: the editor keeps the pointer.
    state = Step(state, Playing());
    EXPECT_FALSE(state.Locked);

    GameViewPointerLockInputs press = Playing();
    press.GameViewPressed = true;
    state = Step(state, press);
    EXPECT_TRUE(state.Locked);
    EXPECT_FALSE(state.HandedBack);
}

// Escape while the editor already has the pointer is the editor's key alone.
TEST(GameViewPointerLockTests, EscapeWhileUnlockedHandsNothingBack)
{
    GameViewPointerLockInputs escape = Playing();
    escape.Requested = false;
    escape.EscapePressed = true;
    GameViewPointerLockState state = Step({}, escape);
    EXPECT_FALSE(state.HandedBack);
    EXPECT_TRUE(Step(state, Playing()).Locked);
}

// Alt-tab away and click another panel to come back: the pointer stays the
// editor's until the Game View is pressed.
TEST(GameViewPointerLockTests, LosingFocusWhileLockedHandsThePointerBack)
{
    GameViewPointerLockState state = Step({}, Playing());
    GameViewPointerLockInputs unfocused = Playing();
    unfocused.WindowFocused = false;
    state = Step(state, unfocused);
    EXPECT_FALSE(state.Locked);
    EXPECT_TRUE(state.HandedBack);

    state = Step(state, Playing());
    EXPECT_FALSE(state.Locked) << "regaining focus alone does not lock";

    GameViewPointerLockInputs press = Playing();
    press.GameViewPressed = true;
    EXPECT_TRUE(Step(state, press).Locked);
}

// Focus that was never locked has nothing to hand back: the grant waits for it.
TEST(GameViewPointerLockTests, AnUnfocusedStartLocksOnceTheWindowIsFocused)
{
    GameViewPointerLockInputs unfocused = Playing();
    unfocused.WindowFocused = false;
    GameViewPointerLockState state = Step({}, unfocused);
    EXPECT_FALSE(state.HandedBack);
    EXPECT_TRUE(Step(state, Playing()).Locked);
}

// A pause, a stop or a withdrawn request ends the hand-back: resuming play or a
// new request is a return to the game.
TEST(GameViewPointerLockTests, PauseStopOrAWithdrawnRequestEndTheHandBack)
{
    GameViewPointerLockInputs escape = Playing();
    escape.EscapePressed = true;
    const GameViewPointerLockState handedBack = Step(Step({}, Playing()), escape);
    ASSERT_TRUE(handedBack.HandedBack);

    GameViewPointerLockInputs paused = Playing();
    paused.Playing = false;
    GameViewPointerLockState state = Step(handedBack, paused);
    EXPECT_FALSE(state.Locked);
    EXPECT_FALSE(state.HandedBack);
    EXPECT_TRUE(Step(state, Playing()).Locked);

    GameViewPointerLockInputs withdrawn = Playing();
    withdrawn.Requested = false;
    state = Step(handedBack, withdrawn);
    EXPECT_FALSE(state.Locked);
    EXPECT_TRUE(Step(state, Playing()).Locked);
}
