#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

/// The AI clock of one world, a singleton. An AI tick is every Divider-th step of the world's
/// SimulationClock. AiTickSystem writes Divider, Tick and Due each step; a game may set RateHz.
struct AiClock
{
    static constexpr float32 kDefaultRateHz = 20.0f;

    /// The AI ticks per simulated second. Divider is the step rate over this value, rounded, at least 1.
    float32 RateHz = kDefaultRateHz;
    /// The seed of every AI draw in the world; one seed and one input give one result.
    uint64 Seed = 0;
    /// The simulation steps per AI tick.
    uint32 Divider = 1;
    /// The simulation tick divided by Divider.
    uint32 Tick = 0;
    /// True on the step that starts an AI tick; the AI systems do no work on other steps.
    bool Due = false;
};

static_assert(std::is_trivially_copyable_v<AiClock>);

} // namespace GameEngine::Components
