#include "ECS/SimulationClock.h"

#include "Logger/Logger.h"
#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::ECS
{

namespace
{
// A frame delta in float, such as 3.0f / 60.0f, can be a little less than a whole
// number of steps. Time within this part of a step still counts as the full step.
constexpr float64 kStepTolerance = 1.0e-5;
} // namespace

void SimulationClock::Advance(float64 frameDeltaSeconds)
{
    m_FrameSeconds = std::clamp(frameDeltaSeconds, 0.0, kMaxFrameDeltaSeconds) * m_TimeScale;
    m_AccumulatedSeconds += m_FrameSeconds;
    CountSteps();
}

void SimulationClock::Reset()
{
    m_Tick = 0;
    m_AccumulatedSeconds = m_FrameSeconds;
    CountSteps();
}

void SimulationClock::CountSteps()
{
    m_StepInFrame = 0;
    m_StepsThisFrame = 0;
    const float64 stepThreshold = m_FixedDeltaSeconds * (1.0 - kStepTolerance);
    while (m_AccumulatedSeconds >= stepThreshold && m_StepsThisFrame < m_MaxStepsPerFrame)
    {
        m_AccumulatedSeconds -= m_FixedDeltaSeconds;
        ++m_StepsThisFrame;
    }
    // The whole steps over the cap never run, so a hitch gives no burst on the next frames.
    const float64 stepsOverCap = std::floor(m_AccumulatedSeconds / m_FixedDeltaSeconds + kStepTolerance);
    m_AccumulatedSeconds = std::max(m_AccumulatedSeconds - stepsOverCap * m_FixedDeltaSeconds, 0.0);
    m_Alpha = Math::Clamp01(static_cast<float32>(m_AccumulatedSeconds / m_FixedDeltaSeconds));
}

void SimulationClock::Restore(uint64 tick)
{
    m_Tick = tick;
    m_StepsThisFrame = 0;
    m_StepInFrame = 0;
}

bool SimulationClock::BeginStep()
{
    if (m_StepInFrame >= m_StepsThisFrame)
        return false;
    ++m_StepInFrame;
    ++m_Tick;
    return true;
}

void SimulationClock::RestartSteps()
{
    m_Tick -= m_StepInFrame;
    m_StepInFrame = 0;
}

void SimulationClock::SetFixedDeltaSeconds(float64 seconds)
{
    if (!(seconds > 0.0))
    {
        Logger::Log::Error("[ECS] SimulationClock: fixed delta {} s refused; give a value above 0", seconds);
        return;
    }
    m_FixedDeltaSeconds = seconds;
}

void SimulationClock::SetMaxStepsPerFrame(uint32 steps)
{
    if (steps == 0)
    {
        Logger::Log::Error("[ECS] SimulationClock: 0 steps per frame refused; give 1 or more");
        return;
    }
    m_MaxStepsPerFrame = steps;
}

void SimulationClock::SetTimeScale(float64 scale)
{
    if (!(scale >= 0.0))
    {
        Logger::Log::Error("[ECS] SimulationClock: time scale {} refused; give 0 or more", scale);
        return;
    }
    m_TimeScale = scale;
}

} // namespace GameEngine::ECS
