// CBTUpdateRestGate: when the CBT update may be skipped. CPU only; the GPU side (the readback that
// feeds the gate, the skipped frames' buffers) is CBTUpdateNodeTest.AStillViewSettlesAndStopsUpdating.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>

#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTUpdateRestGate.h"

using namespace GameEngine::CBTTerrain;

namespace
{
constexpr std::array<uint8_t, 4> kInputsA{1, 2, 3, 4};
constexpr std::array<uint8_t, 4> kInputsB{1, 2, 3, 5};
// One clock reading for every call: the inputs never hold long enough to last.
const std::chrono::steady_clock::time_point kStill{};

CBTUpdateActivity Quiet()
{
    return CBTUpdateActivity{};
}

CBTUpdateActivity Splitting()
{
    CBTUpdateActivity activity{};
    activity.SplitServed = 12;
    return activity;
}
} // namespace

// The update is skipped once two readings of updates with the current inputs split and merged
// nothing with the pressure and keep steps unchanged; a reading that splits, or moves a step,
// restarts the count.
TEST(CBTUpdateRestGateTest, RestsAfterQuietReadingsOfTheCurrentInputs)
{
    CBTUpdateRestGate gate;
    EXPECT_FALSE(gate.CanSkip(kInputsA, kStill));
    for (uint64_t expected = 0; expected < 6u; ++expected)
        EXPECT_EQ(gate.OnUpdateRecorded(false), expected);

    gate.OnActivityRead(0u, Splitting());
    gate.OnActivityRead(1u, Quiet());
    EXPECT_FALSE(gate.CanSkip(kInputsA, kStill)) << "one quiet reading proves nothing about the steps";
    gate.OnActivityRead(2u, Quiet());
    EXPECT_TRUE(gate.CanSkip(kInputsA, kStill));

    CBTUpdateActivity pressureMoved{};
    pressureMoved.PressureStep = 1;
    gate.OnActivityRead(3u, pressureMoved);
    EXPECT_FALSE(gate.CanSkip(kInputsA, kStill)) << "a moved pressure step is not rest";
    gate.OnActivityRead(4u, pressureMoved);
    gate.OnActivityRead(5u, pressureMoved);
    EXPECT_TRUE(gate.CanSkip(kInputsA, kStill));
}

// New inputs, or a forced full re-evaluation, restart the count, and readings of updates recorded
// before the restart are ignored even when they land after it.
TEST(CBTUpdateRestGateTest, NewInputsAndForcedUpdatesRestartTheCount)
{
    CBTUpdateRestGate gate;
    EXPECT_FALSE(gate.CanSkip(kInputsA, kStill));
    for (uint64_t i = 0; i < 3u; ++i)
        gate.OnActivityRead(gate.OnUpdateRecorded(false), Quiet());
    ASSERT_TRUE(gate.CanSkip(kInputsA, kStill));

    const uint64_t beforeChange = gate.OnUpdateRecorded(false);
    EXPECT_FALSE(gate.CanSkip(kInputsB, kStill));
    gate.OnActivityRead(beforeChange, Quiet());
    gate.OnActivityRead(beforeChange, Quiet());
    EXPECT_FALSE(gate.CanSkip(kInputsB, kStill)) << "readings from before the input change count";
    for (uint64_t i = 0; i < 3u; ++i)
        gate.OnActivityRead(gate.OnUpdateRecorded(false), Quiet());
    ASSERT_TRUE(gate.CanSkip(kInputsB, kStill));

    const uint64_t forced = gate.OnUpdateRecorded(true);
    gate.OnActivityRead(forced, Quiet());
    gate.OnActivityRead(forced, Quiet());
    EXPECT_FALSE(gate.CanSkip(kInputsB, kStill)) << "a forced update's own reading counts";
    for (uint64_t i = 0; i < 3u; ++i)
        gate.OnActivityRead(gate.OnUpdateRecorded(false), Quiet());
    EXPECT_TRUE(gate.CanSkip(kInputsB, kStill));
}

// The inputs hold when they equal the previous call's (the camera is still), which is when the update
// applies the rest rule; they stop holding the moment they change, and after a reset.
TEST(CBTUpdateRestGateTest, InputsHoldOnlyWhileTheyRepeat)
{
    CBTUpdateRestGate gate;
    EXPECT_EQ(gate.Settle(), kSettleMoved);
    gate.CanSkip(kInputsA, kStill);
    EXPECT_EQ(gate.Settle(), kSettleMoved) << "the first update at new inputs is not at rest";
    gate.CanSkip(kInputsA, kStill);
    EXPECT_EQ(gate.Settle(), kSettleHeld);
    gate.CanSkip(kInputsB, kStill);
    EXPECT_EQ(gate.Settle(), kSettleMoved);
    gate.CanSkip(kInputsB, kStill);
    EXPECT_EQ(gate.Settle(), kSettleHeld);
    gate.Reset();
    EXPECT_EQ(gate.Settle(), kSettleMoved);
}

// The rest lasts once the inputs have held for kKeepProbeSettle, counted from the update where they changed; a
// change starts the count again.
TEST(CBTUpdateRestGateTest, TheRestLastsAfterTheProbeSettle)
{
    const auto settle = CBTUpdateRestGate::kKeepProbeSettle;
    CBTUpdateRestGate gate;
    gate.CanSkip(kInputsA, kStill);
    gate.CanSkip(kInputsA, kStill + settle - std::chrono::milliseconds(1));
    EXPECT_EQ(gate.Settle(), kSettleHeld) << "a rest shorter than the settle has not lasted";
    gate.CanSkip(kInputsA, kStill + settle);
    EXPECT_EQ(gate.Settle(), kSettleLasted);
    gate.CanSkip(kInputsB, kStill + 2 * settle);
    EXPECT_EQ(gate.Settle(), kSettleMoved);
    gate.CanSkip(kInputsB, kStill + 2 * settle + std::chrono::milliseconds(1));
    EXPECT_EQ(gate.Settle(), kSettleHeld) << "the settle counts from the last change";
}

// A reading whose keep search has a step left to take is not rest, even when it split and merged nothing with
// both steps unchanged: a probe without a measured tree, a moved update with a keep step to probe, and a keep
// step above its floor. A found keep step, and a moved update with nothing to probe, are rest.
TEST(CBTUpdateRestGateTest, AnOpenKeepSearchIsNotRest)
{
    CBTUpdateActivity probing{};
    probing.OffFrustumKeepStep = 8;
    probing.OffFrustumKeepFloor = kKeepFloorProbing;
    CBTUpdateActivity arriving{};
    arriving.OffFrustumKeepStep = 9;
    arriving.OffFrustumKeepFloor = kKeepFloorArriving;
    CBTUpdateActivity searching{};
    searching.OffFrustumKeepStep = 7;
    searching.OffFrustumKeepFloor = 0;
    for (const CBTUpdateActivity& open : {probing, arriving, searching})
    {
        CBTUpdateRestGate gate;
        EXPECT_FALSE(gate.CanSkip(kInputsA, kStill));
        for (uint64_t i = 0; i < 3u; ++i)
            gate.OnActivityRead(gate.OnUpdateRecorded(false), open);
        EXPECT_FALSE(gate.CanSkip(kInputsA, kStill)) << "keep step " << open.OffFrustumKeepStep << ", floor "
                                             << open.OffFrustumKeepFloor;
    }

    CBTUpdateActivity found{};
    found.OffFrustumKeepStep = 9;
    found.OffFrustumKeepFloor = 9;
    CBTUpdateActivity movedUngated{};
    movedUngated.OffFrustumKeepFloor = kKeepFloorArriving;
    for (const CBTUpdateActivity& rest : {found, movedUngated})
    {
        CBTUpdateRestGate gate;
        EXPECT_FALSE(gate.CanSkip(kInputsA, kStill));
        for (uint64_t i = 0; i < 3u; ++i)
            gate.OnActivityRead(gate.OnUpdateRecorded(false), rest);
        EXPECT_TRUE(gate.CanSkip(kInputsA, kStill)) << "keep step " << rest.OffFrustumKeepStep << ", floor "
                                            << rest.OffFrustumKeepFloor;
    }
}
