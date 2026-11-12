#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <span>

#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{

// What the get_gpu_profiler reply describes: the last resolved frame of the main
// window's render graph and the state of its profiling switches.
struct GpuProfilerReportInput
{
    std::span<const Rendering::RenderGraph::RGFrame::RGPassTiming> Timings;
    // Each scope's PassRow indexes Timings.
    std::span<const Rendering::RenderGraph::RGFrame::RGScopeTiming> Scopes;
    std::span<const Rendering::RenderGraph::RGFrame::RGSkippedPass> Skipped;
    Rendering::RenderGraph::RGFrame::RGProfilingResolveStats ResolveStats;
    bool ProfilingEnabled = false;
    Rendering::TimestampSemantics TimingSemantics = Rendering::TimestampSemantics::PipelinePoint;
    bool StatisticsEnabled = false;
    bool StatisticsSupported = false;
};

// The get_gpu_profiler reply: one row per pass with its CPU and GPU times, the
// resolve counts, the warnings that change how the times read, and, while
// pipeline statistics are on, each row's counters or the reason it has none and
// the rows of the producers that draw inside it ("scopes"); and the passes that a
// feature skipped this frame, with their state ("skippedPasses").
nlohmann::json DescribeGpuProfiler(const GpuProfilerReportInput& input);

} // namespace GameEngine::Editor
