#include "DebugServer/GpuProfiler/GpuProfilerReport.h"

#include <algorithm>
#include <string>

namespace GameEngine::Editor
{
namespace
{
namespace RG = Rendering::RenderGraph;
using StatisticsState = RG::RGFrame::RGPassStatisticsState;

constexpr const char* kStatisticsUnsupportedReason =
    "This GPU or graphics backend has no pipeline-statistics queries (the Vulkan feature "
    "pipelineStatisticsQuery is off, or the backend is Metal or WebGPU).";

nlohmann::json DescribeStatistics(const Rendering::PipelineStatistics& statistics)
{
    return {{"inputAssemblyVertices", statistics.InputAssemblyVertices},
            {"inputAssemblyPrimitives", statistics.InputAssemblyPrimitives},
            {"vertexShaderInvocations", statistics.VertexShaderInvocations},
            {"clippingInvocations", statistics.ClippingInvocations},
            {"clippingPrimitives", statistics.ClippingPrimitives},
            {"fragmentShaderInvocations", statistics.FragmentShaderInvocations},
            {"computeShaderInvocations", statistics.ComputeShaderInvocations}};
}

// Empty for Off and Measured: those rows carry no reason.
const char* StatisticsMissingReason(StatisticsState state)
{
    switch (state)
    {
    case StatisticsState::Off:
    case StatisticsState::Measured: return "";
    case StatisticsState::Unsupported: return kStatisticsUnsupportedReason;
    case StatisticsState::NotGraphicsQueue:
        return "The pass ran on an async compute or copy queue. The counters are recorded on the graphics "
               "queue only.";
    case StatisticsState::QueriesUsedUp:
        return "The frame used all of its pipeline-statistics queries before this pass.";
    case StatisticsState::NotReady: return "The query had no result when the frame slot returned.";
    }
    return "";
}

const char* SkippedPassStateName(RG::RGFrame::RGSkippedPassState state)
{
    switch (state)
    {
    case RG::RGFrame::RGSkippedPassState::Cached: return "cached";
    case RG::RGFrame::RGSkippedPassState::Deferred: return "deferred";
    case RG::RGFrame::RGSkippedPassState::MissingInput: return "missingInput";
    }
    return "";
}

nlohmann::json DescribeScope(const RG::RGFrame::RGScopeTiming& timing)
{
    nlohmann::json row = {{"name", timing.Name}, {"cpuMs", timing.CpuMs}, {"gpuSpanMs", timing.GpuSpanMs}};
    if (timing.StatisticsState == StatisticsState::Measured)
        row["stats"] = DescribeStatistics(timing.Statistics);
    else if (timing.StatisticsState != StatisticsState::Off)
        row["statsMissing"] = StatisticsMissingReason(timing.StatisticsState);
    return row;
}

nlohmann::json DescribePass(const RG::RGFrame::RGPassTiming& timing)
{
    nlohmann::json row = {{"name", timing.Name},
                          {"queue", RG::RGQueueName(timing.Queue)},
                          {"phase", timing.Phase},
                          {"cpuMs", timing.CpuMs},
                          {"gpuSpanMs", timing.GpuSpanMs},
                          {"spanShared", timing.SpanShared},
                          {"spanCounted", timing.SpanCounted}};
    if (timing.StatisticsState == StatisticsState::Measured)
        row["stats"] = DescribeStatistics(timing.Statistics);
    else if (timing.StatisticsState != StatisticsState::Off)
        row["statsMissing"] = StatisticsMissingReason(timing.StatisticsState);
    return row;
}

nlohmann::json DescribeWarnings(const GpuProfilerReportInput& input)
{
    const RG::RGFrame::RGProfilingResolveStats& resolveStats = input.ResolveStats;
    nlohmann::json warnings = nlohmann::json::array();
    if (input.TimingSemantics == Rendering::TimestampSemantics::EncoderSpan)
    {
        warnings.push_back(
            "gpuSpanMs values are summed command-encoder spans, not additive pass costs: "
            "one encoder can carry several passes and is then reported to each of them "
            "(spanShared), and encoders overlap one another because the GPU runs them "
            "concurrently. Treat each as an upper bound and do NOT sum them — use "
            "resolveStats.distinctSpanGpuMs (itself an upper bound) or the device's GPU frame "
            "period for a frame total. To total a group of passes, add only the ones with "
            "spanCounted true: each measurement is counted on exactly one pass.");
    }
    if (resolveStats.NonMonotonic > 0)
    {
        warnings.push_back("resolveStats.nonMonotonicTimestamps = " + std::to_string(resolveStats.NonMonotonic) +
                           ": that many passes' raw timestamp pairs closed before they opened "
                           "and were discarded, so their gpuSpanMs reads 0.");
    }
    if (resolveStats.ReadFailures > 0 || resolveStats.InvalidQueryIndices > 0)
    {
        warnings.push_back("resolveStats readFailures/invalidQueryIndices are non-zero: those "
                           "passes have no measurement this frame and read 0.");
    }
    // A row has a state other than Off only when the switch was on as its pass was recorded.
    const bool recordedWithStatistics =
        std::ranges::any_of(input.Timings, [](const RG::RGFrame::RGPassTiming& timing)
                            { return timing.StatisticsState != StatisticsState::Off; });
    if (recordedWithStatistics)
    {
        warnings.push_back("Pipeline statistics were on for this frame: every pass recorded inline, and each "
                           "pass on the graphics queue, and each producer scope inside it, had a query around it, "
                           "so its cpuMs and gpuSpanMs include that cost. Turn them off with "
                           "{\"stats\": false} and read times from a later frame.");
    }
    return warnings;
}
} // namespace

nlohmann::json DescribeGpuProfiler(const GpuProfilerReportInput& input)
{
    nlohmann::json passes = nlohmann::json::array();
    double totalCpu = 0.0;
    for (const RG::RGFrame::RGPassTiming& timing : input.Timings)
    {
        totalCpu += timing.CpuMs;
        passes.push_back(DescribePass(timing));
    }
    // A scope row sits under its pass row. Its counters are part of the pass row's counters and
    // its span is inside the pass span, so neither adds to a total.
    for (const RG::RGFrame::RGScopeTiming& scope : input.Scopes)
    {
        if (scope.PassRow >= passes.size())
            continue;
        nlohmann::json& pass = passes[scope.PassRow];
        if (!pass.contains("scopes"))
            pass["scopes"] = nlohmann::json::array();
        pass["scopes"].push_back(DescribeScope(scope));
    }
    // A pass that its feature skipped this frame costs nothing, so it has a state and no times.
    nlohmann::json skippedPasses = nlohmann::json::array();
    for (const RG::RGFrame::RGSkippedPass& skipped : input.Skipped)
        skippedPasses.push_back({{"name", skipped.Name}, {"state", SkippedPassStateName(skipped.State)}});

    const RG::RGFrame::RGProfilingResolveStats& resolveStats = input.ResolveStats;
    nlohmann::json result;
    result["enabled"] = input.ProfilingEnabled;
    if (!input.ProfilingEnabled)
        result["hint"] = "Profiling disarmed — call with {\"enabled\": true}, then poll again.";
    result["timingSemantics"] =
        input.TimingSemantics == Rendering::TimestampSemantics::EncoderSpan ? "encoder-span" : "pipeline-point";
    result["warnings"] = DescribeWarnings(input);
    nlohmann::json statistics = {{"enabled", input.StatisticsEnabled}, {"supported", input.StatisticsSupported}};
    if (!input.StatisticsSupported)
        statistics["reason"] = kStatisticsUnsupportedReason;
    result["pipelineStatistics"] = std::move(statistics);
    result["resolveStats"] = {{"invalidQueryIndices", resolveStats.InvalidQueryIndices},
                              {"readFailures", resolveStats.ReadFailures},
                              {"nonMonotonicTimestamps", resolveStats.NonMonotonic},
                              {"resolvedPasses", resolveStats.ResolvedPasses},
                              {"sharedSpanPasses", resolveStats.SharedSpanPasses},
                              {"distinctSpanGpuMs", resolveStats.DistinctSpanGpuMs}};
    result["lastFrame"] = {
        {"passes", std::move(passes)}, {"skippedPasses", std::move(skippedPasses)}, {"totalCpuMs", totalCpu}};
    return result;
}

} // namespace GameEngine::Editor
