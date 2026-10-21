#pragma once

#include "Diagnostics/FramePhaseHistory.h"
#include "ECS/SystemWaveTrace.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace GameEngine::Editor
{

// What the get_frame_phases reply describes besides the samples themselves.
struct FramePhaseReportContext
{
    std::uint64_t FramesRecorded = 0;
    bool FixedFrameRate = false;
    // List every frame whose FrameMs is at or above this.
    std::optional<double> MinFrameMs;
    // Also list the newest N frames whatever their time.
    std::size_t NewestFrames = 0;
    // The ECS wave trace's recorded frames, oldest first (empty while it holds
    // none), and system names by SystemManager slot. A listed frame names its
    // slowest systems when one of these frames began inside it.
    bool EcsTraceEnabled = false;
    std::span<const ECS::SystemWaveTrace::Frame> EcsFrames;
    std::span<const std::string> SystemNames;
};

// The get_frame_phases reply over `frames` (oldest first): the frame time and
// every phase as {median, p95, max} over the window, then the listed frames one
// by one with their phases, the phases that ran furthest above their window
// median ("excess", the attribution of a slow frame) and, where the ECS wave
// trace holds the frame, its slowest systems. With MinFrameMs set, the spacing
// between listed frames in frames and milliseconds shows a periodic spike's
// cadence. Times are milliseconds.
nlohmann::json BuildFramePhaseReport(std::span<const FramePhaseHistory::Sample> frames,
                                     const FramePhaseReportContext& context);

} // namespace GameEngine::Editor
