// The community controller mapping database: the parse, the match by vendor and product id, and a
// controller's raw controls mapped to the GamepadButton and GamepadAxis layout.
#include "Gamepad/ControllerMapping/ControllerMapping.h"
#include "Gamepad/ControllerMapping/ControllerMappingDatabase.h"
#include "Input/GamepadFrame.h"
#include "Input/InputScriptApi.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

namespace GameEngine::Input
{
namespace
{

constexpr std::string_view kDualSenseLine =
    "030000004c050000e60c000000000000,PS5 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
    "dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,misc1:b14,"
    "rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,touchpad:b13,x:b0,y:b3,"
    "platform:Windows,";

constexpr std::uint16_t kSonyVendorId = 0x054C;
constexpr std::uint16_t kDualSenseProductId = 0x0CE6;

bool Button(const GamepadFrame::Slot& slot, GamepadButton button)
{
    return slot.buttons[static_cast<int>(button)];
}

float Axis(const GamepadFrame::Slot& slot, GamepadAxis axis)
{
    return slot.axes[static_cast<int>(axis)];
}

TEST(ControllerMappingDatabaseTests, ParseKeepsThePlatformsEntriesAndSkipsCommentsOtherPlatformsAndXInput)
{
    const std::string text = std::string("# Windows\r\n\r\n") + std::string(kDualSenseLine) +
                             "\r\n"
                             "xinput,XInput Controller,a:b0,platform:Windows,\n"
                             "030000004c050000e60c000011010000,PS5 Controller,a:b1,platform:Linux,\n"
                             "not a mapping line for this platform,platform:Mac OS X,\n";

    const ControllerMappingDatabase database = ControllerMappingDatabase::Parse(text, "Windows");

    EXPECT_EQ(database.EntryCount(), 1u);
    EXPECT_TRUE(database.RefusedLines().empty());
}

TEST(ControllerMappingDatabaseTests, MalformedLinesAreRefusedAndNamed)
{
    const std::string text = "0300004c05,Short Guid Pad,a:b0,platform:Windows,\n"
                             "030000004c050000e60c000000000000,No Colon Pad,a-b0,platform:Windows,\n"
                             "030000004c050000e60c000000000000,Bad Source Pad,a:q7,platform:Windows,\n" +
                             std::string(kDualSenseLine) + "\n";

    const ControllerMappingDatabase database = ControllerMappingDatabase::Parse(text, "Windows");

    EXPECT_EQ(database.EntryCount(), 1u);
    ASSERT_EQ(database.RefusedLines().size(), 3u);
    EXPECT_EQ(database.RefusedLines()[0].LineNumber, 1);
    EXPECT_NE(database.RefusedLines()[0].Reason.find("Short Guid Pad"), std::string::npos);
    EXPECT_EQ(database.RefusedLines()[1].LineNumber, 2);
    EXPECT_NE(database.RefusedLines()[1].Reason.find("a-b0"), std::string::npos);
    EXPECT_EQ(database.RefusedLines()[2].LineNumber, 3);
    EXPECT_NE(database.RefusedLines()[2].Reason.find("q7"), std::string::npos);
}

TEST(ControllerMappingDatabaseTests, FindMatchesByVendorAndProductIdAndAnUnknownControllerFindsNothing)
{
    // The second line's GUID carries no vendor and product id (non-zero bytes 6 and 7).
    const std::string text = std::string(kDualSenseLine) +
                             "\n03000095090000010000000000000000,Elecom Pad,a:b0,platform:Windows,\n";

    const ControllerMappingDatabase database = ControllerMappingDatabase::Parse(text, "Windows");

    const ControllerMapping* dualSense = database.Find(kSonyVendorId, kDualSenseProductId);
    ASSERT_NE(dualSense, nullptr);
    EXPECT_EQ(dualSense->Name, "PS5 Controller");
    EXPECT_EQ(dualSense->VendorId, kSonyVendorId);
    EXPECT_EQ(dualSense->ProductId, kDualSenseProductId);
    EXPECT_EQ(database.Find(kSonyVendorId, 0x0CE7), nullptr);
    EXPECT_EQ(database.Find(0x0995, 0x0100), nullptr);
    EXPECT_TRUE(database.RefusedLines().empty());
}

TEST(ControllerMappingDatabaseTests, FakeControllerRawControlsMapToGamepadCodesThroughItsEntry)
{
    const ControllerMappingDatabase database = ControllerMappingDatabase::Parse(kDualSenseLine, "Windows");
    const ControllerMapping* mapping = database.Find(kSonyVendorId, kDualSenseProductId);
    ASSERT_NE(mapping, nullptr);

    // Twelve buttons, so guide (b12) is past the reading and reads released.
    const float axes[6] = {-1.0f, 0.5f, 0.25f, -1.0f, 1.0f, 0.0f};
    bool buttons[12] = {};
    buttons[1] = true; // a
    buttons[9] = true; // start
    const std::uint8_t hats[1] = {1 | 2}; // up and right

    GamepadFrame::Slot slot;
    mapping->Apply({axes, buttons, hats}, slot);

    EXPECT_TRUE(slot.connected);
    EXPECT_TRUE(Button(slot, GamepadButton::A));
    EXPECT_FALSE(Button(slot, GamepadButton::X));
    EXPECT_TRUE(Button(slot, GamepadButton::Start));
    EXPECT_FALSE(Button(slot, GamepadButton::Guide));
    EXPECT_TRUE(Button(slot, GamepadButton::DpadUp));
    EXPECT_TRUE(Button(slot, GamepadButton::DpadRight));
    EXPECT_FALSE(Button(slot, GamepadButton::DpadDown));
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::LeftX), -1.0f);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::LeftY), -0.5f);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::RightX), 0.25f);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::RightY), 0.0f);
    // A full trigger axis at rest reads -1, which is a released trigger.
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::LeftTrigger), 0.0f);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::RightTrigger), 1.0f);
}

TEST(ControllerMappingDatabaseTests, HalfAxisAndInvertedSourcesMapToTheirRange)
{
    const ControllerMappingDatabase database = ControllerMappingDatabase::Parse(
        "030000007e0500000620000000000000,Half Axis Pad,+leftx:h0.2,-leftx:h0.8,lefty:a1~,dpup:-a1,"
        "righttrigger:+a2,platform:Windows,",
        "Windows");
    const ControllerMapping* mapping = database.Find(0x057E, 0x2006);
    ASSERT_NE(mapping, nullptr);

    const float axes[3] = {0.0f, -1.0f, 0.5f};
    const std::uint8_t leftHat[1] = {8};
    GamepadFrame::Slot left;
    mapping->Apply({axes, {}, leftHat}, left);
    EXPECT_FLOAT_EQ(Axis(left, GamepadAxis::LeftX), -1.0f);
    EXPECT_FLOAT_EQ(Axis(left, GamepadAxis::LeftY), -1.0f);
    EXPECT_TRUE(Button(left, GamepadButton::DpadUp));
    EXPECT_FLOAT_EQ(Axis(left, GamepadAxis::RightTrigger), 0.5f);

    const std::uint8_t rightHat[1] = {2};
    GamepadFrame::Slot right;
    mapping->Apply({axes, {}, rightHat}, right);
    EXPECT_FLOAT_EQ(Axis(right, GamepadAxis::LeftX), 1.0f);
}

TEST(ControllerMappingDatabaseTests, MappedSticksPushedUpReadPositiveY)
{
    const ControllerMappingDatabase database = ControllerMappingDatabase::Parse(
        "030000004c050000e60c000000000000,Stick Pad,leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Windows,\n",
        "Windows");
    const ControllerMapping* mapping = database.Find(kSonyVendorId, kDualSenseProductId);
    ASSERT_NE(mapping, nullptr);
    // The database reads a stick +Y down, so a raw -1 is a stick pushed fully up.
    const float axes[4] = {0.5f, -1.0f, -0.25f, -0.5f};
    GamepadFrame::Slot slot;
    mapping->Apply({axes, {}, {}}, slot);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::LeftY), 1.0f);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::RightY), 0.5f);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::LeftX), 0.5f);
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::RightX), -0.25f);
}

TEST(ControllerMappingDatabaseTests, InvertedHalfAxisReadsItsOwnHalfReversed)
{
    const ControllerMappingDatabase database = ControllerMappingDatabase::Parse(
        "030000004c050000e60c000000000000,Inverted Half,righttrigger:+a0~,platform:Windows,\n", "Windows");
    const ControllerMapping* mapping = database.Find(kSonyVendorId, kDualSenseProductId);
    ASSERT_NE(mapping, nullptr);
    const float axes[] = {0.25f};
    GamepadFrame::Slot slot;
    mapping->Apply({axes, {}, {}}, slot);
    // "+a0~" reads the half above the center, reversed: a raw 0.25 reads 0.75.
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::RightTrigger), 0.75f);
}

TEST(ControllerMappingDatabaseTests, InvertedHalfAxisPastTheReadingReadsReleased)
{
    const ControllerMappingDatabase database = ControllerMappingDatabase::Parse(
        "030000004c050000e60c000000000000,Missing Axis,righttrigger:+a5~,platform:Windows,\n", "Windows");
    const ControllerMapping* mapping = database.Find(kSonyVendorId, kDualSenseProductId);
    ASSERT_NE(mapping, nullptr);
    const float axes[] = {0.25f};
    GamepadFrame::Slot slot;
    mapping->Apply({axes, {}, {}}, slot);
    // The reading has one axis, so "+a5~" reads released, not fully pressed.
    EXPECT_FLOAT_EQ(Axis(slot, GamepadAxis::RightTrigger), 0.0f);
}

TEST(ControllerMappingDatabaseTests, ShippedDatabaseParsesWithoutRefusalsAndHasTheDualSenseAndSwitchPro)
{
    const ControllerMappingDatabase database = ControllerMappingDatabase::ParseShipped("Windows");

    EXPECT_TRUE(database.RefusedLines().empty());
    EXPECT_GT(database.EntryCount(), 800u);
    EXPECT_NE(database.Find(kSonyVendorId, kDualSenseProductId), nullptr);
    EXPECT_NE(database.Find(0x057E, 0x2009), nullptr);
}

} // namespace
} // namespace GameEngine::Input
