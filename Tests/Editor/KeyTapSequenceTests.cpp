#include <gtest/gtest.h>

#include "DebugServer/KeyTapSequence.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"

using GameEngine::Editor::KeyTapSequence;
using namespace GameEngine::Input;

// send_key with a count of 3 gives the game three presses: each frame's input reads one tap.
TEST(KeyTapSequence, EachTapIsItsOwnPressInTheFrameInputReads)
{
    InputSystem input;
    KeyTapSequence taps(3);
    int frames = 0;
    int pressEvents = 0;
    int framesWithAPress = 0;
    const KeyTapSequence::SendToWindow sendToInput = [&](int action)
    {
        pressEvents += action == kKeyActionPress ? 1 : 0;
        input.OnKey(kKeyCode_Tab, action, 0);
        return true;
    };
    KeyTapSequence::Step step = KeyTapSequence::Step::Pending;
    while (step == KeyTapSequence::Step::Pending)
    {
        step = taps.Poll(sendToInput);
        input.Update(0.0f);
        framesWithAPress += input.WasKeyPressed(kKeyCode_Tab) ? 1 : 0;
        ++frames;
    }
    EXPECT_EQ(step, KeyTapSequence::Step::Done);
    EXPECT_EQ(pressEvents, 3);
    EXPECT_EQ(framesWithAPress, 3);
    EXPECT_EQ(frames, taps.FrameCount());
    EXPECT_FALSE(input.IsKeyDown(kKeyCode_Tab));
}

// A window that closes during the taps ends the sequence on that poll, with no more sends.
TEST(KeyTapSequence, AWindowThatClosesEndsTheSequence)
{
    KeyTapSequence taps(3);
    int sends = 0;
    const KeyTapSequence::SendToWindow closesAfterTwoSends = [&](int) { return ++sends <= 2; };
    EXPECT_EQ(taps.Poll(closesAfterTwoSends), KeyTapSequence::Step::Pending);
    EXPECT_EQ(taps.Poll(closesAfterTwoSends), KeyTapSequence::Step::Pending);
    EXPECT_EQ(taps.Poll(closesAfterTwoSends), KeyTapSequence::Step::WindowClosed);
    EXPECT_EQ(sends, 3);
}

TEST(KeyTapSequence, TheCountIsClampedToOneThroughTheMaximum)
{
    EXPECT_EQ(KeyTapSequence(0).TapCount(), 1);
    EXPECT_EQ(KeyTapSequence(KeyTapSequence::kMaxTapCount + 1).TapCount(), KeyTapSequence::kMaxTapCount);
}
