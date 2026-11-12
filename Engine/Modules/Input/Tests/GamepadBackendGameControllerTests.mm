// The macOS backend through the poller. The tests post the framework's snapshot controllers
// (+controllerWithExtendedGamepad) as connect and disconnect notifications on the main thread.
#include "Input/Gamepad/GamepadBackend.h"
#include "Input/Gamepad/GamepadPoller.h"
#include "Input/GamepadFrame.h"
#include "Input/InputScriptApi.h"
#include "JobSystem/WorkStealingThreadPool.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::Input;

namespace
{

constexpr auto kStartTimeout = std::chrono::seconds(10);

struct ButtonElement
{
    GamepadButton Button;
    GCControllerButtonInput* (*Element)(GCExtendedGamepad*);
};

// Every button the profile has as its own element. The D-pad's buttons follow its axes.
const ButtonElement kButtonElements[] = {
    {GamepadButton::A, [](GCExtendedGamepad* pad) { return pad.buttonA; }},
    {GamepadButton::B, [](GCExtendedGamepad* pad) { return pad.buttonB; }},
    {GamepadButton::X, [](GCExtendedGamepad* pad) { return pad.buttonX; }},
    {GamepadButton::Y, [](GCExtendedGamepad* pad) { return pad.buttonY; }},
    {GamepadButton::LeftBumper, [](GCExtendedGamepad* pad) { return pad.leftShoulder; }},
    {GamepadButton::RightBumper, [](GCExtendedGamepad* pad) { return pad.rightShoulder; }},
    {GamepadButton::Back, [](GCExtendedGamepad* pad) { return pad.buttonOptions; }},
    {GamepadButton::Start, [](GCExtendedGamepad* pad) { return pad.buttonMenu; }},
    {GamepadButton::Guide, [](GCExtendedGamepad* pad) { return pad.buttonHome; }},
    {GamepadButton::LeftThumb, [](GCExtendedGamepad* pad) { return pad.leftThumbstickButton; }},
    {GamepadButton::RightThumb, [](GCExtendedGamepad* pad) { return pad.rightThumbstickButton; }},
};

struct DpadDirection
{
    GamepadButton Button;
    float X;
    float Y;
};

const DpadDirection kDpadDirections[] = {
    {GamepadButton::DpadUp, 0.0f, 1.0f},
    {GamepadButton::DpadRight, 1.0f, 0.0f},
    {GamepadButton::DpadDown, 0.0f, -1.0f},
    {GamepadButton::DpadLeft, -1.0f, 0.0f},
};

void PostControllerStatus(GCController* controller, bool connected)
{
    @autoreleasepool
    {
        NSString* name = connected ? GCControllerDidConnectNotification : GCControllerDidDisconnectNotification;
        [[NSNotificationCenter defaultCenter] postNotificationName:name object:controller];
    }
}

// A held button, as the pressed buttons of one slot: one bit per GamepadButton.
unsigned PressedMask(const GamepadFrame::Slot& slot)
{
    unsigned mask = 0;
    for (int button = 0; button < kGamepadButtonCount; ++button)
        mask |= slot.buttons[button] ? 1u << button : 0u;
    return mask;
}

unsigned ButtonBit(GamepadButton button)
{
    return 1u << static_cast<int>(button);
}

class GameControllerBackendTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // The main thread runs no run loop while Start runs: Start must not wait for the main thread.
        m_Poller = std::make_unique<GamepadPoller>(m_Pool, CreatePlatformGamepadBackend());
        const auto deadline = std::chrono::steady_clock::now() + kStartTimeout;
        while (GamepadPoller::GetRuntimeState() == GamepadRuntimeState::Starting &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_EQ(GamepadPoller::GetRuntimeState(), GamepadRuntimeState::Available);
        // A pad attached to the machine keeps its slot; the tests read only the slots it leaves free.
        m_Poller->Read(m_Attached);
    }

    void TearDown() override { m_Poller.reset(); }

    GamepadFrame ReadFrame()
    {
        GamepadFrame frame;
        m_Poller->Read(frame);
        return frame;
    }

    // The slots that are connected in `frame` and were free when the test started.
    int CountTestSlots(const GamepadFrame& frame) const
    {
        int count = 0;
        for (int slot = 0; slot < kMaxGamepads; ++slot)
            count += frame.slots[slot].connected && !m_Attached.slots[slot].connected ? 1 : 0;
        return count;
    }

    // The first slot that is connected in `frame` and was free when the test started, or -1.
    int FirstTestSlot(const GamepadFrame& frame) const
    {
        for (int slot = 0; slot < kMaxGamepads; ++slot)
        {
            if (frame.slots[slot].connected && !m_Attached.slots[slot].connected)
                return slot;
        }
        return -1;
    }

    JobSystem::WorkStealingThreadPool m_Pool{2, 2};
    std::unique_ptr<GamepadPoller> m_Poller;
    GamepadFrame m_Attached;
};

} // namespace

// Each element lands on its GamepadAxis or GamepadButton; sticks read +Y up, triggers 0 to 1.
TEST_F(GameControllerBackendTests, ExtendedGamepadElementsReadInTheStandardLayout)
{
    GCController* controller = [GCController controllerWithExtendedGamepad];
    GCExtendedGamepad* pad = controller.extendedGamepad;
    [pad.leftThumbstick setValueForXAxis:0.25f yAxis:0.5f];
    [pad.rightThumbstick setValueForXAxis:-0.75f yAxis:-1.0f];
    [pad.leftTrigger setValue:0.4f];
    [pad.rightTrigger setValue:1.0f];
    PostControllerStatus(controller, true);

    GamepadFrame frame = ReadFrame();
    const int slot = FirstTestSlot(frame);
    ASSERT_GE(slot, 0) << "the connected controller has no slot";
    const float* axes = frame.slots[slot].axes;
    EXPECT_FLOAT_EQ(axes[static_cast<int>(GamepadAxis::LeftX)], 0.25f);
    EXPECT_FLOAT_EQ(axes[static_cast<int>(GamepadAxis::LeftY)], 0.5f);
    EXPECT_FLOAT_EQ(axes[static_cast<int>(GamepadAxis::RightX)], -0.75f);
    EXPECT_FLOAT_EQ(axes[static_cast<int>(GamepadAxis::RightY)], -1.0f);
    EXPECT_FLOAT_EQ(axes[static_cast<int>(GamepadAxis::LeftTrigger)], 0.4f);
    EXPECT_FLOAT_EQ(axes[static_cast<int>(GamepadAxis::RightTrigger)], 1.0f);
    EXPECT_EQ(PressedMask(frame.slots[slot]), 0u);

    for (const ButtonElement& element : kButtonElements)
    {
        [element.Element(pad) setValue:1.0f];
        frame = ReadFrame();
        EXPECT_EQ(PressedMask(frame.slots[slot]), ButtonBit(element.Button))
            << "GamepadButton " << static_cast<int>(element.Button);
        [element.Element(pad) setValue:0.0f];
    }
    for (const DpadDirection& direction : kDpadDirections)
    {
        [pad.dpad setValueForXAxis:direction.X yAxis:direction.Y];
        frame = ReadFrame();
        EXPECT_EQ(PressedMask(frame.slots[slot]), ButtonBit(direction.Button))
            << "GamepadButton " << static_cast<int>(direction.Button);
    }
    PostControllerStatus(controller, false);
}

// The writer: an arrival takes a slot on the next read, and a second arrival of the same controller takes
// none (two notifications; this test does not run Start's list of attached controllers). A removal frees
// only its own slot, and an arrival and its removal between two reads leave the slots as they were.
TEST_F(GameControllerBackendTests, ArrivalTakesOneSlotAndRemovalFreesOnlyItsOwn)
{
    GCController* first = [GCController controllerWithExtendedGamepad];
    GCController* second = [GCController controllerWithExtendedGamepad];
    [second.extendedGamepad.buttonB setValue:1.0f];

    PostControllerStatus(first, true);
    PostControllerStatus(first, true);
    GamepadFrame frame = ReadFrame();
    ASSERT_EQ(CountTestSlots(frame), 1);
    const int firstSlot = FirstTestSlot(frame);

    PostControllerStatus(second, true);
    frame = ReadFrame();
    ASSERT_EQ(CountTestSlots(frame), 2);

    PostControllerStatus(first, false);
    frame = ReadFrame();
    ASSERT_EQ(CountTestSlots(frame), 1);
    const int secondSlot = FirstTestSlot(frame);
    EXPECT_NE(secondSlot, firstSlot);
    EXPECT_EQ(PressedMask(frame.slots[secondSlot]), ButtonBit(GamepadButton::B));

    GCController* passing = [GCController controllerWithExtendedGamepad];
    PostControllerStatus(passing, true);
    PostControllerStatus(passing, false);
    frame = ReadFrame();
    EXPECT_EQ(CountTestSlots(frame), 1);
    EXPECT_TRUE(frame.slots[secondSlot].connected);

    PostControllerStatus(second, false);
    EXPECT_EQ(CountTestSlots(ReadFrame()), 0);
}

// A controller without the extended gamepad profile does not read as a gamepad.
TEST_F(GameControllerBackendTests, ControllerWithoutTheExtendedProfileTakesNoSlot)
{
    GCController* micro = [GCController controllerWithMicroGamepad];
    ASSERT_TRUE(micro.extendedGamepad == nil);
    PostControllerStatus(micro, true);
    EXPECT_EQ(CountTestSlots(ReadFrame()), 0);
    PostControllerStatus(micro, false);
}

// Stop releases every retain the backend took: the slots', the snapshots' and the writer's.
TEST_F(GameControllerBackendTests, StopReleasesEveryControllerItHeld)
{
    GCController* controller = [[GCController controllerWithExtendedGamepad] retain];
    const NSUInteger retainsBefore = [controller retainCount];

    PostControllerStatus(controller, true);
    ASSERT_EQ(CountTestSlots(ReadFrame()), 1);
    EXPECT_GT([controller retainCount], retainsBefore);
    m_Poller.reset();

    EXPECT_EQ([controller retainCount], retainsBefore);
    [controller release];
}
