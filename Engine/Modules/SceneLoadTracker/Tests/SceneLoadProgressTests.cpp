#include "SceneLoadTracker/SceneLoadProgress.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

using namespace GameEngine;
using Clock = SceneLoadProgress::Clock;

namespace
{
constexpr double kFrameSeconds = 1.0 / 60.0;
constexpr double kNeverReady = 1e9;

struct Frame
{
    double Seconds = 0.0;
    double Progress = 0.0;
    bool Ready = false;
    bool ProgressShown = false;
};

Clock::time_point At(Clock::time_point start, double seconds)
{
    return start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

// Frame times from `from` to `to` at 60 frames per second.
std::vector<double> FrameTimes(double from, double to)
{
    std::vector<double> times;
    for (int frame = 0; from + frame * kFrameSeconds < to; ++frame)
        times.push_back(from + frame * kFrameSeconds);
    return times;
}

// Runs one load's frames with the tracker's estimate at each frame and ready from `readyAt`.
std::vector<Frame> RunLoad(const std::vector<double>& times, const std::function<double(double)>& secondsRemaining,
                       double readyAt)
{
    const Clock::time_point start = Clock::now();
    SceneLoadProgress progress(start);
    std::vector<Frame> frames;
    for (const double seconds : times)
    {
        const bool ready = seconds >= readyAt;
        progress.Update(At(start, seconds), ready ? 0.0 : secondsRemaining(seconds), ready);
        frames.push_back(
            {.Seconds = seconds, .Progress = progress.Progress(), .Ready = ready, .ProgressShown = progress.IsProgressShown()});
    }
    return frames;
}

// The largest rise of the bar within any 250 ms: each frame against the last frame at least
// 250 ms before it, or 0 before the load began.
double LargestRiseInAWindow(const std::vector<Frame>& frames)
{
    const double window = std::chrono::duration<double>(SceneLoadProgress::kSpeedWindow).count();
    double largest = 0.0;
    for (std::size_t current = 0; current < frames.size(); ++current)
    {
        double before = 0.0;
        for (std::size_t earlier = 0; earlier < current && frames[earlier].Seconds <= frames[current].Seconds - window; ++earlier)
            before = frames[earlier].Progress;
        largest = std::max(largest, frames[current].Progress - before);
    }
    return largest;
}

double ProgressAt(const std::vector<Frame>& frames, double seconds)
{
    double displayed = 0.0;
    for (const Frame& frame : frames)
        if (frame.Seconds <= seconds)
            displayed = frame.Progress;
    return displayed;
}

const Frame* FirstReady(const std::vector<Frame>& frames)
{
    const auto it = std::find_if(frames.begin(), frames.end(), [](const Frame& frame) { return frame.Ready; });
    return it != frames.end() ? &*it : nullptr;
}
} // namespace

TEST(SceneLoadProgress, TheBarNeverGoesBackwardWhenTheEstimateGrowsALittle)
{
    // A true 10 s load whose estimate grows by 8 % at 4 s: too little to move the line, enough
    // to put the target below the bar.
    const std::vector<Frame> frames = RunLoad(
        FrameTimes(0.0, 9.0), [](double t) { return t < 4.0 ? 10.0 - t : 1.08 * 6.0 - (t - 4.0); }, kNeverReady);
    ASSERT_GT(ProgressAt(frames, 4.0), 0.35);
    for (std::size_t i = 1; i < frames.size(); ++i)
        ASSERT_GE(frames[i].Progress, frames[i - 1].Progress) << "at " << frames[i].Seconds << " s";
}

TEST(SceneLoadProgress, TheBarRisesAtMostTheSpeedLimitInAnyQuarterSecond)
{
    // The estimate says 20 s, then drops to 0.2 s at 3 s; a frame at 3.5 s takes 1 s.
    std::vector<double> times = FrameTimes(0.0, 3.5);
    for (const double t : FrameTimes(4.5, 7.0))
        times.push_back(t);
    const std::vector<Frame> frames =
        RunLoad(times, [](double t) { return t < 3.0 ? 20.0 - t : 0.2; }, kNeverReady);
    EXPECT_LE(LargestRiseInAWindow(frames), SceneLoadProgress::kSpeedLimit + 1e-9);
    EXPECT_GT(frames.back().Progress, 0.9) << "the bar still climbs to the target at the limit";
    EXPECT_LT(frames.back().Progress, 1.0) << "100 % only once ready";
}

TEST(SceneLoadProgress, AnEstimateThatGrowsByMoreThanATenthRestartsTheLineAtTheBar)
{
    // A true 10 s load that finds 7 s more work at 3 s: it ends at 17 s.
    const std::vector<Frame> frames =
        RunLoad(FrameTimes(0.0, 16.9), [](double t) { return t < 3.0 ? 10.0 - t : 17.0 - t; }, kNeverReady);
    const double atGrowth = ProgressAt(frames, 3.0);
    EXPECT_GT(ProgressAt(frames, 3.5), atGrowth + 0.01) << "the bar keeps moving after the growth";
    for (const Frame& frame : frames)
    {
        if (frame.Seconds < 3.0)
            continue;
        const double line = atGrowth + (1.0 - atGrowth) * (frame.Seconds - 3.0) / (17.0 - 3.0);
        ASSERT_NEAR(frame.Progress, line, 0.1) << "at " << frame.Seconds << " s";
    }
}

TEST(SceneLoadProgress, TheBarIsOneInTheFrameTheLoadIsReady)
{
    // {estimated total, ready at, bar shown}: short loads, ready before 1 s or with a total under 2.5 s;
    // a load on time; and an estimate 6x too long, so the bar is low just before ready.
    struct Case
    {
        double Total, ReadyAt;
        bool ProgressShown;
    };
    for (const Case& load : {Case{0.8, 0.8, false}, Case{3.0, 0.8, false}, Case{2.0, 2.0, false},
                             Case{4.0, 4.0, true}, Case{20.0, 3.0, true}})
    {
        // The frames run 5 s past ready, as when a game holds the reveal.
        const std::vector<Frame> frames = RunLoad(FrameTimes(0.0, load.ReadyAt + 5.0),
                                                  [total = load.Total](double t) { return total - t; }, load.ReadyAt);
        const Frame* ready = FirstReady(frames);
        ASSERT_NE(ready, nullptr) << load.Total << " s";
        EXPECT_EQ(ready->Progress, 1.0) << load.Total << " s: the speed limit does not hold the last step";
        EXPECT_LT((ready - 1)->Progress, 1.0) << load.Total << " s: 100 % only once ready";
        EXPECT_EQ(frames.back().Progress, 1.0) << load.Total << " s";
        EXPECT_EQ(frames.back().ProgressShown, load.ProgressShown) << load.Total << " s: the time after ready does not show the bar";
    }
}
