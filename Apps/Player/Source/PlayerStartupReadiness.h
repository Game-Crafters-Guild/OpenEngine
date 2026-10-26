#pragma once

#include "Types/Types.h"

namespace GameEngine
{

/// Whether the Player's startup scene is ready for its startup line and its --screenshot
/// capture: its resolve batch is complete and, if the game config waits for them, its spline
/// recipes are settled. The wait has no clock: each recipe's own hold budget bounds it.
class PlayerStartupReadiness
{
public:
    /// Starts the wait for the startup scene that just opened.
    void Begin(bool waitForSplineRecipes);
    /// Advances the wait by one frame. Returns true on the frame the scene becomes ready.
    bool Update(bool batchComplete, bool recipesSettled, float64 deltaSeconds);

    [[nodiscard]] bool IsPending() const;
    [[nodiscard]] uint32 Frames() const;
    [[nodiscard]] float64 Seconds() const;

private:
    bool m_Pending = false;
    bool m_WaitForSplineRecipes = true;
    uint32 m_Frames = 0;
    float64 m_Seconds = 0.0;
};

} // namespace GameEngine
