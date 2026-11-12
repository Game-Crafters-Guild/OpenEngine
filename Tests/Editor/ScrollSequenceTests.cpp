#include <gtest/gtest.h>

#include "DebugServer/ScrollSequence.h"

#include <vector>

using GameEngine::Editor::ScrollSequence;

namespace
{
// The deltas each poll sends, until the sequence ends.
std::vector<int> DeltasPerPoll(ScrollSequence& wheel, ScrollSequence::Step& lastStep)
{
    std::vector<int> deltasPerPoll;
    int sends = 0;
    const ScrollSequence::SendToWindow countSend = [&]
    {
        ++sends;
        return true;
    };
    lastStep = ScrollSequence::Step::Pending;
    while (lastStep == ScrollSequence::Step::Pending)
    {
        sends = 0;
        lastStep = wheel.Poll(countSend);
        deltasPerPoll.push_back(sends);
    }
    return deltasPerPoll;
}
} // namespace

// send_scroll with a target and a count of 3: one hover frame, one delta on each of 3 frames, then the reply.
TEST(ScrollSequence, AMovedPointerWaitsOneFrameThenSendsOneDeltaAFrame)
{
    ScrollSequence wheel(3, true);
    ScrollSequence::Step lastStep = ScrollSequence::Step::Pending;
    const std::vector<int> deltasPerPoll = DeltasPerPoll(wheel, lastStep);
    EXPECT_EQ(lastStep, ScrollSequence::Step::Done);
    EXPECT_EQ(deltasPerPoll, (std::vector<int>{0, 1, 1, 1, 0}));
    EXPECT_EQ(wheel.FrameCount(), 3 + 2);
    EXPECT_EQ(static_cast<int>(deltasPerPoll.size()), wheel.FrameCount());
}

// send_scroll at the pointer sends the first delta on the first poll.
TEST(ScrollSequence, AWheelAtThePointerSendsOnTheFirstPoll)
{
    ScrollSequence wheel(2, false);
    ScrollSequence::Step lastStep = ScrollSequence::Step::Pending;
    const std::vector<int> deltasPerPoll = DeltasPerPoll(wheel, lastStep);
    EXPECT_EQ(lastStep, ScrollSequence::Step::Done);
    EXPECT_EQ(deltasPerPoll, (std::vector<int>{1, 1, 0}));
    EXPECT_EQ(static_cast<int>(deltasPerPoll.size()), wheel.FrameCount());
}

// A window that closes during the wheel ends the sequence on that poll, with no more sends.
TEST(ScrollSequence, AWindowThatClosesEndsTheSequence)
{
    ScrollSequence wheel(3, true);
    int sends = 0;
    const ScrollSequence::SendToWindow closesAfterOneSend = [&] { return ++sends <= 1; };
    EXPECT_EQ(wheel.Poll(closesAfterOneSend), ScrollSequence::Step::Pending);
    EXPECT_EQ(wheel.Poll(closesAfterOneSend), ScrollSequence::Step::Pending);
    EXPECT_EQ(wheel.Poll(closesAfterOneSend), ScrollSequence::Step::WindowClosed);
    EXPECT_EQ(sends, 2);
}
