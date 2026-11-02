#include "DebugServer/Capture/FrameSequence/FrameSequenceRecording.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Platform/Thread.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using GameEngine::Editor::FrameFileFormat;
using GameEngine::Editor::FrameSequenceRecording;
using GameEngine::Editor::kFrameRecordingDefaultFrameRate;
using GameEngine::Editor::kFrameRecordingDefaultMaxFrames;
using GameEngine::Editor::kFrameRecordingEncodePriority;
using GameEngine::Editor::kFrameRecordingMaxEncodes;
using GameEngine::Editor::kFrameRecordingMaxReadbacks;

constexpr double kFrameMs = 1000.0 / 60.0;
constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 32;

size_t CountRows(const std::string& listing)
{
    size_t lines = 0;
    for (char c : listing)
        lines += c == '\n' ? 1u : 0u;
    return lines == 0 ? 0 : lines - 1; // the header row
}

// Presents `frameCount` frames. Each readback resolves `readbackLatency` frames after its
// frame, and each encode finishes on the frame after its readback.
void RunFrames(FrameSequenceRecording& recording, uint32_t frameCount, uint32_t readbackLatency)
{
    std::deque<std::pair<uint32_t, uint32_t>> readbacks; // index, frame it resolves on
    std::deque<uint32_t> encodes;
    for (uint32_t frame = 0; frame < frameCount + readbackLatency + 2; ++frame)
    {
        for (uint32_t index : encodes)
            recording.OnFrameWritten(index, 2.0);
        encodes.clear();
        while (!readbacks.empty() && readbacks.front().second <= frame)
        {
            recording.OnReadbackFinished(readbacks.front().first, kWidth, kHeight);
            encodes.push_back(readbacks.front().first);
            readbacks.pop_front();
        }
        if (frame < frameCount)
            if (const auto index = recording.OnFramePresented(1000 + frame, frame * kFrameMs))
                readbacks.emplace_back(*index, frame + readbackLatency);
    }
}

// A recording whose readbacks and encodes keep up lists every presented frame.
TEST(FrameSequenceRecordingTests, ListsEveryPresentedFrameWhenTheRingKeepsUp)
{
    FrameSequenceRecording recording(100, FrameFileFormat::Png, 0.0);
    RunFrames(recording, 30, 2);
    recording.Stop();

    ASSERT_TRUE(recording.IsDrained());
    const auto summary = recording.GetSummary();
    EXPECT_EQ(30u, summary.Presented);
    EXPECT_EQ(30u, summary.Written);
    EXPECT_EQ(0u, summary.Dropped);
    EXPECT_EQ(30u, CountRows(recording.BuildListing()));
}

// The listing has one row per written frame: its file, its render-graph frame and its time.
TEST(FrameSequenceRecordingTests, ListingRowsNameTheFileTheFrameAndTheTime)
{
    FrameSequenceRecording recording(100, FrameFileFormat::Png, 0.0);
    RunFrames(recording, 3, 2);
    recording.Stop();

    const std::string listing = recording.BuildListing();
    EXPECT_EQ(0u, listing.find("index,renderFrame,msSinceStart,deltaMs,width,height,file\n"));
    EXPECT_NE(std::string::npos, listing.find("0,1000,0,0,64,32,frame_000000.png\n"));
    EXPECT_NE(std::string::npos, listing.find("1,1001,16.667,16.667,64,32,frame_000001.png\n"));
    EXPECT_NE(std::string::npos, listing.find("2,1002,33.333,16.667,64,32,frame_000002.png\n"));
}

// A raw recording names its files .rgba: the RGBA8 bytes with no header.
TEST(FrameSequenceRecordingTests, RawRowsNameRgbaFiles)
{
    FrameSequenceRecording recording(100, FrameFileFormat::Raw, 0.0);
    RunFrames(recording, 2, 2);
    recording.Stop();

    const std::string listing = recording.BuildListing();
    EXPECT_NE(std::string::npos, listing.find("0,1000,0,0,64,32,frame_000000.rgba\n"));
    EXPECT_NE(std::string::npos, listing.find("1,1001,16.667,16.667,64,32,frame_000001.rgba\n"));
}

// A full readback ring drops the frame and counts it; the listing holds the others.
TEST(FrameSequenceRecordingTests, AFullReadbackRingDropsTheFrame)
{
    FrameSequenceRecording recording(100, FrameFileFormat::Png, 0.0);
    for (uint32_t frame = 0; frame < kFrameRecordingMaxReadbacks; ++frame)
        ASSERT_TRUE(recording.OnFramePresented(frame, frame * kFrameMs).has_value());
    EXPECT_FALSE(recording.OnFramePresented(9, 9 * kFrameMs).has_value());

    for (uint32_t index = 0; index < kFrameRecordingMaxReadbacks; ++index)
    {
        recording.OnReadbackFinished(index, kWidth, kHeight);
        recording.OnFrameWritten(index, 1.0);
    }
    recording.Stop();

    const auto summary = recording.GetSummary();
    EXPECT_EQ(kFrameRecordingMaxReadbacks + 1, summary.Presented);
    EXPECT_EQ(1u, summary.Dropped);
    EXPECT_EQ(kFrameRecordingMaxReadbacks, summary.Written);
    EXPECT_EQ(static_cast<size_t>(kFrameRecordingMaxReadbacks), CountRows(recording.BuildListing()));
}

// Encodes that fall behind drop new frames instead of queueing pixels without bound.
TEST(FrameSequenceRecordingTests, AnEncodeBacklogDropsTheFrame)
{
    FrameSequenceRecording recording(100, FrameFileFormat::Png, 0.0);
    for (uint32_t frame = 0; frame < kFrameRecordingMaxEncodes; ++frame)
    {
        const auto index = recording.OnFramePresented(frame, frame * kFrameMs);
        ASSERT_TRUE(index.has_value());
        recording.OnReadbackFinished(*index, kWidth, kHeight);
    }
    EXPECT_FALSE(recording.OnFramePresented(99, 99 * kFrameMs).has_value());
    EXPECT_EQ(1u, recording.GetSummary().Dropped);
    EXPECT_FALSE(recording.IsDrained());
}

// A readback whose frame was abandoned counts as a drop and lists no row.
TEST(FrameSequenceRecordingTests, ALostReadbackCountsAsADrop)
{
    FrameSequenceRecording recording(100, FrameFileFormat::Png, 0.0);
    const auto first = recording.OnFramePresented(0, 0.0);
    const auto second = recording.OnFramePresented(1, kFrameMs);
    ASSERT_TRUE(first && second);
    recording.OnReadbackLost(*first);
    recording.OnReadbackFinished(*second, kWidth, kHeight);
    recording.OnFrameWritten(*second, 1.0);
    recording.Stop();

    const auto summary = recording.GetSummary();
    EXPECT_EQ(1u, summary.Dropped);
    EXPECT_EQ(1u, summary.Written);
    const std::string listing = recording.BuildListing();
    EXPECT_EQ(1u, CountRows(listing));
    EXPECT_NE(std::string::npos, listing.find("1,1,16.667,0,64,32,frame_000001.png\n"));
}

// The cap ends the capture: frames after it are neither read back nor counted as presented.
TEST(FrameSequenceRecordingTests, TheFrameCapEndsTheCapture)
{
    FrameSequenceRecording recording(5, FrameFileFormat::Png, 0.0);
    RunFrames(recording, 12, 2);

    EXPECT_FALSE(recording.IsCapturing());
    EXPECT_TRUE(recording.IsDrained());
    const auto summary = recording.GetSummary();
    EXPECT_EQ(5u, summary.Presented);
    EXPECT_EQ(5u, summary.Written);
    EXPECT_EQ(0u, summary.Dropped);
}

// Stop ends the capture at once; frames in flight still finish and list.
TEST(FrameSequenceRecordingTests, StopKeepsTheFramesInFlight)
{
    FrameSequenceRecording recording(100, FrameFileFormat::Png, 0.0);
    const auto index = recording.OnFramePresented(0, 0.0);
    ASSERT_TRUE(index.has_value());
    recording.Stop();
    EXPECT_FALSE(recording.IsCapturing());
    EXPECT_FALSE(recording.OnFramePresented(1, kFrameMs).has_value());
    EXPECT_FALSE(recording.IsDrained());

    recording.OnReadbackFinished(*index, kWidth, kHeight);
    recording.OnFrameWritten(*index, 3.0);
    EXPECT_TRUE(recording.IsDrained());
    EXPECT_EQ(1u, recording.GetSummary().Presented);
    EXPECT_EQ(1u, recording.GetSummary().Written);
}

// A failed write is reported apart from drops and lists no row.
TEST(FrameSequenceRecordingTests, AFailedWriteListsNoRow)
{
    FrameSequenceRecording recording(100, FrameFileFormat::Png, 0.0);
    const auto index = recording.OnFramePresented(0, 0.0);
    ASSERT_TRUE(index.has_value());
    recording.OnReadbackFinished(*index, kWidth, kHeight);
    recording.OnFrameWriteFailed(*index);
    recording.Stop();

    EXPECT_TRUE(recording.IsDrained());
    EXPECT_EQ(1u, recording.GetSummary().Failed);
    EXPECT_EQ(0u, recording.GetSummary().Dropped);
    EXPECT_EQ(0u, CountRows(recording.BuildListing()));
}

// Drops give at most one notice per interval, and only when the count grew.
TEST(FrameSequenceRecordingTests, DropNoticesAreRateLimited)
{
    FrameSequenceRecording recording(100, FrameFileFormat::Png, 0.0);
    EXPECT_FALSE(recording.TakeDropNoticeDue(0.0));
    for (uint32_t frame = 0; frame <= kFrameRecordingMaxReadbacks; ++frame)
        recording.OnFramePresented(frame, frame * kFrameMs);

    EXPECT_TRUE(recording.TakeDropNoticeDue(100.0));
    recording.OnFramePresented(10, 110.0);
    EXPECT_FALSE(recording.TakeDropNoticeDue(500.0));
    EXPECT_TRUE(recording.TakeDropNoticeDue(1100.0));
    EXPECT_FALSE(recording.TakeDropNoticeDue(2500.0));
}

// The encode time of written frames is reported, so a recording states whether PNG keeps up.
TEST(FrameSequenceRecordingTests, TheSummaryReportsTheEncodeTime)
{
    FrameSequenceRecording recording(100, FrameFileFormat::Png, 0.0);
    for (uint32_t frame = 0; frame < 2; ++frame)
    {
        const auto index = recording.OnFramePresented(frame, frame * kFrameMs);
        ASSERT_TRUE(index.has_value());
        recording.OnReadbackFinished(*index, kWidth, kHeight);
        recording.OnFrameWritten(*index, frame == 0 ? 4.0 : 8.0);
    }
    const auto summary = recording.GetSummary();
    EXPECT_DOUBLE_EQ(6.0, summary.EncodeMsMean);
    EXPECT_DOUBLE_EQ(8.0, summary.EncodeMsMax);
    EXPECT_DOUBLE_EQ(kFrameMs, summary.DurationMs);
}

// The default frame rate takes 30 evenly spaced frames in each second of presented frames.
TEST(FrameSequenceRecordingTests, TheDefaultFrameRateTakesThirtyFramesASecond)
{
    FrameSequenceRecording recording(kFrameRecordingDefaultMaxFrames, FrameFileFormat::Png,
                                     kFrameRecordingDefaultFrameRate);
    for (uint32_t frame = 0; frame < 100; ++frame)
        recording.OnFramePresented(frame, 1.0 + frame * 10.0);
    EXPECT_EQ(70u, recording.GetSummary().Skipped);
}

// A frame rate takes the first presented frame at or after each tick of its clock, so
// the frames on disk are evenly spaced. The frames between ticks are skipped, not dropped.
TEST(FrameSequenceRecordingTests, AFrameRateTakesOneFramePerTick)
{
    constexpr double kPresentMs = 10.0;
    FrameSequenceRecording recording(1000, FrameFileFormat::Png, 30.0);
    for (uint32_t frame = 0; frame < 100; ++frame)
    {
        const auto index = recording.OnFramePresented(frame, 1.0 + frame * kPresentMs);
        if (!index)
            continue;
        recording.OnReadbackFinished(*index, kWidth, kHeight);
        recording.OnFrameWritten(*index, 2.0);
    }
    recording.Stop();

    const auto summary = recording.GetSummary();
    EXPECT_EQ(100u, summary.Presented);
    EXPECT_EQ(30u, summary.Written);
    EXPECT_EQ(70u, summary.Skipped);
    EXPECT_EQ(0u, summary.Dropped);
    const std::string listing = recording.BuildListing();
    EXPECT_NE(std::string::npos, listing.find("1,4,41,40,64,32,frame_000001.png\n"));
    EXPECT_NE(std::string::npos, listing.find("2,7,71,30,64,32,frame_000002.png\n"));
}

// The cap counts the frames the clock takes, so maxFrames / frameRate is the recording's length.
TEST(FrameSequenceRecordingTests, TheFrameCapCountsTakenFrames)
{
    constexpr double kPresentMs = 10.0;
    FrameSequenceRecording recording(5, FrameFileFormat::Png, 30.0);
    for (uint32_t frame = 0; frame < 30; ++frame)
    {
        const auto index = recording.OnFramePresented(frame, frame * kPresentMs);
        if (!index)
            continue;
        recording.OnReadbackFinished(*index, kWidth, kHeight);
        recording.OnFrameWritten(*index, 1.0);
    }

    EXPECT_FALSE(recording.IsCapturing());
    const auto summary = recording.GetSummary();
    EXPECT_EQ(5u, summary.Written); // the frames at 0, 40, 70, 110 and 140 ms
    EXPECT_EQ(15u, summary.Presented);
    EXPECT_EQ(10u, summary.Skipped);
}

// Each row gives its frame's size, so raw files of a target that resized stay readable.
TEST(FrameSequenceRecordingTests, ListingRowsCarryTheFrameSize)
{
    FrameSequenceRecording recording(100, FrameFileFormat::Raw, 0.0);
    const auto first = recording.OnFramePresented(0, 0.0);
    const auto second = recording.OnFramePresented(1, kFrameMs);
    ASSERT_TRUE(first && second);
    recording.OnReadbackFinished(*first, 1920, 1080);
    recording.OnReadbackFinished(*second, 1280, 720);
    recording.OnFrameWritten(*first, 1.0);
    recording.OnFrameWritten(*second, 1.0);
    recording.Stop();

    const std::string listing = recording.BuildListing();
    EXPECT_NE(std::string::npos, listing.find("0,0,0,0,1920,1080,frame_000000.rgba\n"));
    EXPECT_NE(std::string::npos, listing.find("1,1,16.667,16.667,1280,720,frame_000001.rgba\n"));
}

// The presented frame rate counts from the first presented frame, not from the start.
TEST(FrameSequenceRecordingTests, ThePresentedFrameRateStartsAtTheFirstFrame)
{
    FrameSequenceRecording recording(100, FrameFileFormat::Png, 0.0);
    for (const double ms : {100.0, 110.0, 120.0})
        recording.OnFramePresented(0, ms);
    EXPECT_DOUBLE_EQ(100.0, recording.GetSummary().PresentedFps);
}

// After a stall, the next tick follows the late frame's time, so the frames after it do not bunch up.
TEST(FrameSequenceRecordingTests, AFrameAfterAStallDoesNotBunchTheNextTicks)
{
    FrameSequenceRecording recording(1000, FrameFileFormat::Png, 10.0);
    for (const double ms : {0.0, 350.0, 360.0, 370.0, 380.0, 390.0, 400.0, 410.0, 450.0})
    {
        const auto index = recording.OnFramePresented(0, ms);
        if (!index)
            continue;
        recording.OnReadbackFinished(*index, kWidth, kHeight);
        recording.OnFrameWritten(*index, 1.0);
    }
    EXPECT_EQ(3u, recording.GetSummary().Written); // the frames at 0, 350 and 400 ms
}

struct Encode
{
    uint32_t Index = 0;
    JobSystem::TaskHandle Job;
};

// Tells the recording about each finished encode and removes it from `encodes`.
void CollectFinishedEncodes(FrameSequenceRecording& recording, std::vector<Encode>& encodes, double encodeMs)
{
    for (const Encode& encode : encodes)
    {
        if (encode.Job.IsDone())
            recording.OnFrameWritten(encode.Index, encodeMs);
    }
    std::erase_if(encodes, [](const Encode& encode) { return encode.Job.IsDone(); });
}

// The encodes share the lane of the frame's own jobs, so a busy frame cannot hold them back.
TEST(FrameSequenceRecordingTests, EncodesRunAtNormalPriority)
{
    EXPECT_EQ(JobSystem::JobPriority::Normal, kFrameRecordingEncodePriority);
}

// An encode slower than the present interval still writes each frame the clock takes, in order,
// with no drop: 240 fps presented, 60 fps taken, every encode a job that sleeps 8 ms.
TEST(FrameSequenceRecordingTests, ASlowEncodeWritesEveryTakenFrameInOrder)
{
    constexpr double kPresentMs = 1000.0 / 240.0;
    constexpr uint32_t kPresentedFrames = 120;
    constexpr uint32_t kFramesPerTick = 4;
    constexpr auto kEncodeTime = std::chrono::milliseconds(8);

    JobSystem::WorkStealingThreadPool pool{2};
    FrameSequenceRecording recording(1000, FrameFileFormat::Png, 60.0);
    std::vector<Encode> encodes;
    std::optional<uint32_t> readback; // resolves on the next presented frame
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t frame = 0; frame < kPresentedFrames || readback || !encodes.empty(); ++frame)
    {
        const auto due = start + std::chrono::duration<double, std::milli>(frame * kPresentMs);
        if (const auto now = std::chrono::steady_clock::now(); now < due)
            GameEngine::Platform::SleepPrecise(due - now);
        CollectFinishedEncodes(recording, encodes, static_cast<double>(kEncodeTime.count()));
        if (readback)
        {
            recording.OnReadbackFinished(*readback, kWidth, kHeight);
            encodes.push_back(Encode{*readback, pool.Submit([kEncodeTime] { std::this_thread::sleep_for(kEncodeTime); },
                                                            kFrameRecordingEncodePriority)});
            readback.reset();
        }
        if (frame < kPresentedFrames)
            readback = recording.OnFramePresented(frame, 1.0 + frame * kPresentMs);
    }

    const auto summary = recording.GetSummary();
    EXPECT_EQ(0u, summary.Dropped);
    EXPECT_EQ(kPresentedFrames / kFramesPerTick, summary.Written);
    const std::string listing = recording.BuildListing();
    size_t previousRow = 0;
    for (uint32_t index = 0; index < summary.Written; ++index)
    {
        const std::string row = '\n' + std::to_string(index) + ',' + std::to_string(index * kFramesPerTick) + ',';
        const size_t at = listing.find(row);
        ASSERT_NE(std::string::npos, at) << row;
        EXPECT_LT(previousRow, at);
        previousRow = at;
    }
}

// The recording asks for a present cap at twice its frame rate while it captures, and for none after.
TEST(FrameSequenceRecordingTests, ThePresentCapHoldsWhileTheRecordingCaptures)
{
    FrameSequenceRecording stopped(100, FrameFileFormat::Png, 60.0);
    EXPECT_DOUBLE_EQ(120.0, stopped.PresentCapFps());
    stopped.Stop();
    EXPECT_DOUBLE_EQ(0.0, stopped.PresentCapFps());

    FrameSequenceRecording capped(1, FrameFileFormat::Png, 60.0);
    capped.OnFramePresented(0, 0.0);
    EXPECT_DOUBLE_EQ(0.0, capped.PresentCapFps());

    const FrameSequenceRecording everyFrame(100, FrameFileFormat::Png, 0.0);
    EXPECT_DOUBLE_EQ(0.0, everyFrame.PresentCapFps());
}

// The frame limiter holds each present to at least the cap interval and adds up to 1 ms of jitter.
// At the present cap, every tick of the clock still takes a frame: 2 s at 60 fps writes 120 frames.
TEST(FrameSequenceRecordingTests, PresentsAtTheCapFillEveryTick)
{
    constexpr uint32_t kTicks = 120;
    constexpr double kDurationMs = kTicks * kFrameMs;
    constexpr uint32_t kJitterSteps = 11;
    constexpr double kJitterStepMs = 0.1;

    FrameSequenceRecording recording(1000, FrameFileFormat::Png, 60.0);
    const double capIntervalMs = 1000.0 / recording.PresentCapFps();
    double ms = 0.0;
    for (uint32_t frame = 0; ms < kDurationMs; ++frame)
    {
        if (const auto index = recording.OnFramePresented(frame, ms))
        {
            recording.OnReadbackFinished(*index, kWidth, kHeight);
            recording.OnFrameWritten(*index, 1.0);
        }
        ms += capIntervalMs + (frame * 7 % kJitterSteps) * kJitterStepMs;
    }

    EXPECT_EQ(kTicks, recording.GetSummary().Written);
}

} // namespace
