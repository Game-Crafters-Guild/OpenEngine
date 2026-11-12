#pragma once

#include "Types/Types.h"

namespace GameEngine::ECS
{

class SystemManager;

/// The fixed-step simulation clock of one world: SystemManager::Update advances it once per frame and
/// runs the Simulation systems once per step. Simulated seconds are the tick times the fixed delta.
/// The host drives the steps; a system reads them.
/// Main thread: write between frames, read from any system.
class SimulationClock
{
public:
    static constexpr float64 kDefaultFixedDeltaSeconds = 1.0 / 60.0;
    static constexpr uint32 kDefaultMaxStepsPerFrame = 4;
    /// A longer frame (a hitch, a debugger break) adds only this much time.
    static constexpr float64 kMaxFrameDeltaSeconds = 0.25;

    /// Adds the frame delta, clamped to [0, kMaxFrameDeltaSeconds], times the time scale. This
    /// frame runs the whole steps the clock holds, at most the step cap; the clock drops the
    /// whole steps over the cap.
    void Advance(float64 frameDeltaSeconds);

    /// Starts the next step of this frame and advances the tick.
    /// @return false when this frame has no step left.
    bool BeginStep();
    /// Starts the clock again at tick 0, with only the time of this frame stored, as on its
    /// first frame. Keeps the step length, the step cap and the time scale.
    void Reset();
    /// Sets the tick to a saved value, for a scene load that restores a saved world. The next
    /// step is tick + 1. The steps of this frame end; the stored time and the alpha stay.
    void Restore(uint64 tick);

    /// The steps begun since the clock started. During a step, the number of that step.
    uint64 GetTick() const { return m_Tick; }
    /// The steps the last Advance gave this frame.
    uint32 GetStepsThisFrame() const { return m_StepsThisFrame; }
    /// The number of the current step in this frame, from 1; 0 before the first step.
    uint32 GetStepInFrame() const { return m_StepInFrame; }

    /// The part of a step that the clock holds and this frame does not run, in [0, 1]. Rendering
    /// blends the previous and the current step by this value.
    float32 GetAlpha() const { return m_Alpha; }

    float64 GetFixedDeltaSeconds() const { return m_FixedDeltaSeconds; }
    /// Sets the length of one step. A value of 0 or less is refused.
    void SetFixedDeltaSeconds(float64 seconds);
    /// Sets the most steps one frame runs. 0 is refused.
    void SetMaxStepsPerFrame(uint32 steps);

    float64 GetTimeScale() const { return m_TimeScale; }
    /// Sets the speed of simulated time against the frame time: 1 is real time, 2 is twice
    /// as fast, 0 is the pause (no step runs, the alpha stays). A negative value is refused.
    void SetTimeScale(float64 scale);

private:
    friend class SystemManager;

    // Moves the tick back to the tick before the first step of this frame. The next
    // run of Simulation systems in the same frame then steps the same ticks again.
    void RestartSteps();

    // Sets the steps this frame runs from the stored time, and the alpha from the rest.
    void CountSteps();

    float64 m_FixedDeltaSeconds = kDefaultFixedDeltaSeconds;
    float64 m_AccumulatedSeconds = 0.0;
    float64 m_FrameSeconds = 0.0;
    float64 m_TimeScale = 1.0;
    uint64 m_Tick = 0;
    uint32 m_MaxStepsPerFrame = kDefaultMaxStepsPerFrame;
    uint32 m_StepsThisFrame = 0;
    uint32 m_StepInFrame = 0;
    float32 m_Alpha = 0.0f;
};

} // namespace GameEngine::ECS
