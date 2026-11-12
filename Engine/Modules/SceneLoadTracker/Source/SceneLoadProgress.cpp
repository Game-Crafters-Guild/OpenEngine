#include "SceneLoadTracker/SceneLoadProgress.h"

#include <algorithm>

namespace GameEngine
{
namespace
{
// The target stays below 1 until ready: 1 means ready to show.
constexpr double kTargetBeforeReady = 0.99;
// An estimate that grows by more than this share from one frame to the next moves the target's
// line to start at the displayed value.
constexpr double kRebaseGrowth = 0.10;

double Seconds(SceneLoadProgress::Clock::duration duration)
{
    return std::chrono::duration<double>(duration).count();
}
} // namespace

SceneLoadProgress::SceneLoadProgress(Clock::time_point start) : m_Start(start), m_BaseTime(start)
{
    m_Window.push_back({.Time = start, .Progress = 0.0});
}

void SceneLoadProgress::Update(Clock::time_point now, double secondsRemaining, bool ready)
{
    if (m_Ready)
        return;
    if (!m_ProgressShown && now - m_Start >= kBarDelay &&
        Seconds(now - m_Start) + secondsRemaining >= Seconds(kBarMinTotal))
        m_ProgressShown = true;
    if (ready)
    {
        // The speed limit applies only before ready, so the bar is 1 in the frame the load is ready.
        m_Ready = true;
        m_Progress = 1.0;
        return;
    }
    if (m_LastSecondsRemaining && secondsRemaining > (1.0 + kRebaseGrowth) * *m_LastSecondsRemaining)
    {
        m_BaseProgress = m_Progress;
        m_BaseTime = now;
    }
    m_LastSecondsRemaining = secondsRemaining;

    // The window's front holds the value the bar had kSpeedWindow ago.
    while (m_Window.size() > 1 && m_Window[1].Time <= now - kSpeedWindow)
        m_Window.pop_front();
    const double limit = m_Window.front().Progress + kSpeedLimit;
    m_Progress = std::max(m_Progress, std::min(Target(now, secondsRemaining), limit));
    m_Window.push_back({.Time = now, .Progress = m_Progress});
}

double SceneLoadProgress::Target(Clock::time_point now, double secondsRemaining) const
{
    const double elapsed = Seconds(now - m_BaseTime);
    const double total = elapsed + secondsRemaining;
    if (total <= 0.0)
        return m_BaseProgress;
    return std::min(kTargetBeforeReady, m_BaseProgress + (1.0 - m_BaseProgress) * elapsed / total);
}
} // namespace GameEngine
