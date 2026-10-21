// The editor's per-frame phase history (FramePhaseHistory), the recorder that fills it at the top of
// each frame (FramePhaseRecorder) and get_frame_phases' reply (BuildFramePhaseReport) over hand-built
// frames: frame alignment, which frames are listed, the phase a slow frame's excess names, the spacing
// between slow frames, and the ECS frame a listed frame joins.

#include <gtest/gtest.h>

#include "DebugServer/FramePhaseReport.h"
#include "Diagnostics/FramePhaseHistory.h"
#include "Diagnostics/FramePhaseRecorder.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

namespace
{

using GameEngine::ECS::SystemWaveTrace;
using GameEngine::Editor::BuildFramePhaseReport;
using GameEngine::Editor::FramePhaseHistory;
using GameEngine::Editor::FramePhaseRecorder;
using GameEngine::Editor::FramePhaseReportContext;
using Sample = FramePhaseHistory::Sample;

constexpr std::uint64_t kNsPerMs = 1000 * 1000;

// A 4 ms frame starting at `index` * 4 ms: 1 ms of AppUpdate (all of it UI windows), 1 ms of
// engine update and 1.9 ms of render (1 ms of it graph execution), leaving 0.1 ms unaccounted.
Sample MakeFrame(std::uint64_t index)
{
    Sample sample;
    sample.FrameIndex = index;
    sample.UpdateBeginNs = index * 4 * kNsPerMs;
    sample.NextUpdateBeginNs = sample.UpdateBeginNs + 4 * kNsPerMs;
    sample.FrameMs = 4.0f;
    sample.AppUpdateMs = 1.0f;
    sample.UiWindowsMs = 1.0f;
    sample.EngineUpdateMs = 1.0f;
    sample.RenderMs = 1.9f;
    sample.RgExecuteMs = 1.0f;
    sample.SceneViewRendered = true;
    return sample;
}

// MakeFrame with 15 ms more in the UI windows' update.
Sample MakeUiSpike(std::uint64_t index)
{
    Sample sample = MakeFrame(index);
    sample.FrameMs += 15.0f;
    sample.AppUpdateMs += 15.0f;
    sample.UiWindowsMs += 15.0f;
    return sample;
}

std::vector<Sample> FramesWithSpikesAt(std::uint64_t count, std::initializer_list<std::uint64_t> spikes)
{
    std::vector<Sample> frames;
    for (std::uint64_t i = 0; i < count; ++i)
    {
        const bool spike = std::find(spikes.begin(), spikes.end(), i) != spikes.end();
        frames.push_back(spike ? MakeUiSpike(i) : MakeFrame(i));
    }
    return frames;
}

TEST(FramePhaseHistory, KeepsTheNewestFramesOldestFirstOnceTheRingWraps)
{
    FramePhaseHistory history;
    for (std::uint64_t i = 0; i < FramePhaseHistory::kCapacity + 3; ++i)
        history.Record(MakeFrame(i));

    std::vector<Sample> newest;
    history.CopyNewest(newest, 2);
    ASSERT_EQ(newest.size(), 2u);
    EXPECT_EQ(newest[0].FrameIndex, FramePhaseHistory::kCapacity + 1);
    EXPECT_EQ(newest[1].FrameIndex, FramePhaseHistory::kCapacity + 2);

    std::vector<Sample> all;
    history.CopyNewest(all, FramePhaseHistory::kCapacity * 2);
    ASSERT_EQ(all.size(), FramePhaseHistory::kCapacity);
    EXPECT_EQ(all.front().FrameIndex, 3u);
    EXPECT_EQ(history.GetFramesRecorded(), FramePhaseHistory::kCapacity + 3);
}

// What the editor hands the recorder at the top of frame `frameCount`: the previous frame's
// interval, and a Scene View breakdown last stamped in `sceneViewFrame`.
FramePhaseRecorder::FrameInputs MakeInputs(std::uint64_t frameCount, double deltaSeconds, std::uint64_t sceneViewFrame)
{
    FramePhaseRecorder::FrameInputs inputs;
    inputs.FrameCount = frameCount;
    inputs.DeltaSeconds = deltaSeconds;
    inputs.Phases.AppUpdateMs = 1.0;
    inputs.SceneView.worldViewsMs = 1.5;
    inputs.SceneView.frameIndex = sceneViewFrame;
    return inputs;
}

TEST(FramePhaseRecorder, RecordsThePreviousFrameWithItsOwnIntervalAndOnlyItsOwnSceneView)
{
    FramePhaseRecorder recorder;
    // Frame 5 starts: nothing before it to record.
    recorder.RecordPreviousFrame(MakeInputs(5, 0.100, 4));
    // Frame 6 starts: frame 5 took 4 ms and rendered the Scene View.
    recorder.RecordPreviousFrame(MakeInputs(6, 0.004, 5));
    // Frame 7 starts: frame 6 took 20 ms and rendered no Scene View (the breakdown is still frame 5's).
    recorder.RecordPreviousFrame(MakeInputs(7, 0.020, 5));
    // Frame 8 starts under a fixed frame rate: the delta is the fixed step, not frame 7's time.
    FramePhaseRecorder::FrameInputs fixedStep = MakeInputs(8, 1.0 / 60.0, 5);
    fixedStep.FixedFrameRate = true;
    recorder.RecordPreviousFrame(fixedStep);

    std::vector<Sample> samples;
    recorder.GetHistory().CopyNewest(samples, 8);
    ASSERT_EQ(samples.size(), 3u);

    EXPECT_EQ(samples[0].FrameIndex, 5u);
    EXPECT_FLOAT_EQ(samples[0].FrameMs, 4.0f);
    EXPECT_FLOAT_EQ(samples[0].AppUpdateMs, 1.0f);
    EXPECT_TRUE(samples[0].SceneViewRendered);
    EXPECT_FLOAT_EQ(samples[0].WorldViewsMs, 1.5f);

    EXPECT_EQ(samples[1].FrameIndex, 6u);
    EXPECT_FLOAT_EQ(samples[1].FrameMs, 20.0f);
    EXPECT_FALSE(samples[1].SceneViewRendered);
    EXPECT_FLOAT_EQ(samples[1].WorldViewsMs, 0.0f);
    EXPECT_EQ(samples[1].UpdateBeginNs, samples[0].NextUpdateBeginNs);

    EXPECT_EQ(samples[2].FrameIndex, 7u);
    const double stampedMs = static_cast<double>(samples[2].NextUpdateBeginNs - samples[2].UpdateBeginNs) / 1e6;
    EXPECT_NEAR(samples[2].FrameMs, stampedMs, 1e-3);
}

TEST(FramePhaseReport, ListsOnlyFramesAtOrAboveTheThresholdAndNamesThePhaseThatCarriedThem)
{
    const std::vector<Sample> frames = FramesWithSpikesAt(40, {12});
    FramePhaseReportContext context;
    context.MinFrameMs = 10.0;

    auto reply = BuildFramePhaseReport(frames, context);

    ASSERT_EQ(reply["frames"].size(), 1u);
    const auto& spike = reply["frames"][0];
    EXPECT_EQ(spike["frameIndex"], 12u);
    EXPECT_FLOAT_EQ(spike["frameMs"].get<float>(), 19.0f);
    ASSERT_GE(spike["excess"].size(), 2u);
    EXPECT_EQ(spike["excess"][0]["phase"], "appUpdate");
    EXPECT_EQ(spike["excess"][1]["phase"], "appUpdate.uiWindows");
    EXPECT_NEAR(spike["excess"][1]["aboveMedianMs"].get<double>(), 15.0, 1e-4);
    EXPECT_NEAR(spike["phases"]["unaccounted"].get<double>(), 0.1, 1e-4);
    EXPECT_EQ(reply["summary"]["framesAtOrAbove"], 1u);
    EXPECT_NEAR(reply["summary"]["frameMs"]["median"].get<double>(), 4.0, 1e-6);
    EXPECT_NEAR(reply["summary"]["frameMs"]["max"].get<double>(), 19.0, 1e-6);
}

TEST(FramePhaseReport, SpikeSpacingShowsTheirCadenceInFramesAndMilliseconds)
{
    const std::vector<Sample> frames = FramesWithSpikesAt(60, {5, 25, 45});
    FramePhaseReportContext context;
    context.MinFrameMs = 10.0;

    auto reply = BuildFramePhaseReport(frames, context);

    EXPECT_EQ(reply["summary"]["framesAtOrAbove"], 3u);
    EXPECT_EQ(reply["summary"]["spikeIntervalFrames"]["median"], 20.0);
    EXPECT_EQ(reply["summary"]["spikeIntervalMs"]["median"], 80.0);
}

TEST(FramePhaseReport, NewestFramesAreListedWhateverTheirTime)
{
    const std::vector<Sample> frames = FramesWithSpikesAt(10, {});
    FramePhaseReportContext context;
    context.NewestFrames = 3;

    auto reply = BuildFramePhaseReport(frames, context);

    ASSERT_EQ(reply["frames"].size(), 3u);
    EXPECT_EQ(reply["frames"][0]["frameIndex"], 7u);
    EXPECT_EQ(reply["frames"][2]["frameIndex"], 9u);
    EXPECT_TRUE(reply["frames"][2]["excess"].empty());
    EXPECT_FALSE(reply["summary"].contains("framesAtOrAbove"));
}

TEST(FramePhaseReport, AListedFrameNamesTheSlowestSystemsOfTheEcsFrameThatBeganInsideIt)
{
    // Spikes at frames 3 and 10. The ECS trace holds two frames: the first begins inside frame 9,
    // the second inside the spike at frame 10, where Extraction runs 1 ms on a worker. Frame 3 is
    // older than everything the trace holds (the trace keeps 256 frames, the ring 1024), so it
    // must name no ECS frame rather than the oldest one the trace has.
    const std::vector<Sample> frames = FramesWithSpikesAt(20, {3, 10});
    std::vector<SystemWaveTrace::Frame> ecsFrames(2);
    for (std::size_t f = 0; f < ecsFrames.size(); ++f)
    {
        SystemWaveTrace::Frame& ecs = ecsFrames[f];
        ecs.BeginNs = frames[9 + f].UpdateBeginNs + kNsPerMs / 2;
        ecs.EndNs = ecs.BeginNs + kNsPerMs;
        ecs.SystemCount = 2;
        ecs.Systems[0].Slot = 0;
        ecs.Systems[0].BeginNs = ecs.BeginNs;
        ecs.Systems[0].EndNs = ecs.BeginNs + kNsPerMs / 4;
        ecs.Systems[1].Slot = 1;
        ecs.Systems[1].Worker = 3;
        ecs.Systems[1].BeginNs = ecs.BeginNs;
        ecs.Systems[1].EndNs = ecs.BeginNs + (f == 1 ? kNsPerMs : kNsPerMs / 8);
    }
    const std::vector<std::string> names = {"Transforms", "Extraction"};
    FramePhaseReportContext context;
    context.MinFrameMs = 10.0;
    context.EcsFrames = ecsFrames;
    context.SystemNames = names;

    auto reply = BuildFramePhaseReport(frames, context);

    ASSERT_EQ(reply["frames"].size(), 2u);
    EXPECT_EQ(reply["frames"][0]["frameIndex"], 3u);
    EXPECT_FALSE(reply["frames"][0].contains("ecs"));
    const auto& ecs = reply["frames"][1]["ecs"];
    EXPECT_NEAR(ecs["ecsMs"].get<double>(), 1.0, 1e-9);
    ASSERT_EQ(ecs["slowestSystems"].size(), 2u);
    EXPECT_EQ(ecs["slowestSystems"][0]["name"], "Extraction");
    EXPECT_NEAR(ecs["slowestSystems"][0]["ms"].get<double>(), 1.0, 1e-9);
    EXPECT_TRUE(ecs["slowestSystems"][0]["onWorker"].get<bool>());
    EXPECT_EQ(ecs["slowestSystems"][1]["name"], "Transforms");
}

} // namespace
