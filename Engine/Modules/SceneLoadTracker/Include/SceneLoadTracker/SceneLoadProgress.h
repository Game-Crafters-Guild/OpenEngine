#pragma once

#include <chrono>
#include <deque>
#include <optional>

namespace GameEngine
{
/// The progress bar of one scene load, from the tracker's estimate of the seconds until ready
/// to show. Before ready, the bar never goes down and never rises more than kSpeedLimit in any
/// kSpeedWindow. It is 1 from the frame the load is ready. The loader keeps one per load. Main thread.
class SceneLoadProgress
{
  public:
    using Clock = std::chrono::steady_clock;

    static constexpr double kSpeedLimit = 0.10;
    static constexpr Clock::duration kSpeedWindow = std::chrono::milliseconds(250);
    /// The bar shows once the load has run kBarDelay and its estimated total is kBarMinTotal or more.
    static constexpr Clock::duration kBarDelay = std::chrono::seconds(1);
    static constexpr Clock::duration kBarMinTotal = std::chrono::milliseconds(2500);

    explicit SceneLoadProgress(Clock::time_point start);

    /// Once per frame. `secondsRemaining` is the tracker's estimate. The first call with `ready`
    /// sets the bar to 1, and the calls after it change nothing.
    void Update(Clock::time_point now, double secondsRemaining, bool ready);

    double Progress() const { return m_Progress; }
    /// True from the frame the bar shows (kBarDelay and kBarMinTotal), and it stays true. Only
    /// the frames up to the ready frame can set it.
    bool IsProgressShown() const { return m_ProgressShown; }

  private:
    struct Sample
    {
        Clock::time_point Time;
        double Progress = 0.0;
    };

    double Target(Clock::time_point now, double secondsRemaining) const;

    Clock::time_point m_Start;
    double m_Progress = 0.0;
    bool m_ProgressShown = false;
    bool m_Ready = false;
    // The line the target follows: from (m_BaseProgress, m_BaseTime) to 1 at the estimated end.
    // It moves to the displayed value when the estimate grows by more than 10 %.
    double m_BaseProgress = 0.0;
    Clock::time_point m_BaseTime;
    std::optional<double> m_LastSecondsRemaining;
    // The frames of the last kSpeedWindow, and the last frame before it at the front.
    std::deque<Sample> m_Window;
};
} // namespace GameEngine
