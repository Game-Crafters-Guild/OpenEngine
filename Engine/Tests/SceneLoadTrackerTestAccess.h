#pragma once

// Test-only seam onto SceneLoadTracker::Tick. In the engine only EngineCore::Update calls it;
// the suites step a tracker frame by frame without a running engine. Friended by SceneLoadTracker.

#include "SceneLoadTracker/SceneLoadTracker.h"

#include <chrono>

namespace GameEngine
{
struct SceneLoadTrackerTestAccess
{
    static void Tick(SceneLoadTracker& tracker, std::chrono::steady_clock::time_point now) { tracker.Tick(now); }
};
} // namespace GameEngine
