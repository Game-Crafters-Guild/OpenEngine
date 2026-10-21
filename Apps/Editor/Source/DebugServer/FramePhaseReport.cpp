#include "DebugServer/FramePhaseReport.h"

#include <algorithm>
#include <array>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

using json = nlohmann::json;
using Sample = FramePhaseHistory::Sample;
using EcsFrame = ECS::SystemWaveTrace::Frame;

constexpr double kNsPerMs = 1e6;
// Phases ranked in a listed frame's excess.
constexpr std::size_t kExcessPhases = 5;
// An excess below this is noise, not attribution.
constexpr double kMinExcessMs = 0.05;
// Systems named per listed frame from the ECS wave trace.
constexpr std::size_t kSlowestSystems = 5;
constexpr const char* kUnaccounted = "unaccounted";

struct Phase
{
    const char* Name;
    float Sample::*Field;
    // False for values that are not main-thread time inside the frame (the GPU
    // timeline's period), so they never claim a frame's excess.
    bool Attributable;
};

// Dotted names nest a sub-phase under its phase. renderServices, renderGraph
// and gpuSync times are measured inside the phases above them (the ECS update
// and the render), so they repeat time rather than add to it.
constexpr std::array kPhases = {
    Phase{"poll", &Sample::PollMs, true},
    Phase{"input", &Sample::InputMs, true},
    Phase{"appUpdate", &Sample::AppUpdateMs, true},
    Phase{"appUpdate.preUi", &Sample::PreUiMs, true},
    Phase{"appUpdate.debugPanels", &Sample::DebugPanelsMs, true},
    Phase{"appUpdate.uiWindows", &Sample::UiWindowsMs, true},
    Phase{"appUpdate.tail", &Sample::UpdateTailMs, true},
    Phase{"engineUpdate", &Sample::EngineUpdateMs, true},
    Phase{"render", &Sample::RenderMs, true},
    Phase{"render.preloop", &Sample::RenderPreloopMs, true},
    Phase{"render.beginFrame", &Sample::BeginFrameMs, true},
    Phase{"render.worldViews", &Sample::WorldViewsMs, true},
    Phase{"render.thumbnails", &Sample::ThumbnailsMs, true},
    Phase{"render.uiRecord", &Sample::UiRecordMs, true},
    Phase{"render.terminalPasses", &Sample::TerminalPassesMs, true},
    Phase{"render.rgCompile", &Sample::RgCompileMs, true},
    Phase{"render.rgExecute", &Sample::RgExecuteMs, true},
    Phase{"render.variantCompile", &Sample::VariantCompileMs, true},
    Phase{"render.prewarm", &Sample::PrewarmMs, true},
    Phase{"render.present", &Sample::PresentMs, true},
    Phase{"sleep", &Sample::SleepMs, true},
    Phase{"renderServices.sort", &Sample::SortMs, true},
    Phase{"renderServices.bucketerSchedule", &Sample::BucketerScheduleMs, true},
    Phase{"renderServices.extraction", &Sample::ExtractionMs, true},
    Phase{"renderGraph.compile", &Sample::GraphCompileMs, true},
    Phase{"renderGraph.schedule", &Sample::GraphScheduleMs, true},
    Phase{"renderGraph.generateBarriers", &Sample::GraphBarriersMs, true},
    Phase{"renderGraph.buildSubmissionPlan", &Sample::GraphSubmissionPlanMs, true},
    Phase{"renderGraph.queueSubmit", &Sample::GraphQueueSubmitMs, true},
    Phase{"renderGraph.barrierEmit", &Sample::GraphBarrierEmitMs, true},
    Phase{"gpuSync.fenceWait", &Sample::FenceWaitMs, true},
    Phase{"gpuSync.acquire", &Sample::AcquireMs, true},
    Phase{"gpuSync.presentTransitionSubmit", &Sample::PresentTransitionSubmitMs, true},
    Phase{"gpuSync.queuePresent", &Sample::QueuePresentMs, true},
    Phase{"gpuSync.gpuFramePeriod", &Sample::GpuFramePeriodMs, false},
};

constexpr std::size_t kUnaccountedColumn = kPhases.size();
constexpr std::size_t kColumns = kPhases.size() + 1;

// The frame interval no top-level phase covers: the OS event pump
// (Platform::Window::PollEvents runs between frames, after the previous frame's
// clock started and before this one's phases), the CPU profiler's frame close and
// log dump while it is armed (CpuProfiler::BeginFrame, EndFrameAndTryDumpToLog),
// the loop's own bookkeeping and a frame cap's wait outside SleepMs. Can read
// slightly negative from clock granularity; reported as measured.
double UnaccountedMs(const Sample& sample)
{
    const double phases = static_cast<double>(sample.PollMs) + sample.InputMs + sample.AppUpdateMs +
                          sample.EngineUpdateMs + sample.RenderMs + sample.SleepMs;
    return static_cast<double>(sample.FrameMs) - phases;
}

double Column(const Sample& sample, std::size_t column)
{
    if (column == kUnaccountedColumn)
        return UnaccountedMs(sample);
    return static_cast<double>(sample.*(kPhases[column].Field));
}

const char* ColumnName(std::size_t column)
{
    return column == kUnaccountedColumn ? kUnaccounted : kPhases[column].Name;
}

bool IsAttributable(std::size_t column)
{
    return column == kUnaccountedColumn || kPhases[column].Attributable;
}

// Nearest-rank percentile of a sorted, non-empty sample.
double Percentile(const std::vector<double>& sorted, double fraction)
{
    const std::size_t rank = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1) + 0.5);
    return sorted[std::min(rank, sorted.size() - 1)];
}

json SortedDistribution(const std::vector<double>& sorted)
{
    if (sorted.empty())
        return nullptr;
    return json{{"median", Percentile(sorted, 0.5)},
                {"p95", Percentile(sorted, 0.95)},
                {"p99", Percentile(sorted, 0.99)},
                {"max", sorted.back()}};
}

json Distribution(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    return SortedDistribution(values);
}

std::string SystemName(const FramePhaseReportContext& context, std::uint32_t slot)
{
    if (slot < context.SystemNames.size())
        return context.SystemNames[slot];
    return "<slot " + std::to_string(slot) + ">";
}

bool IsListed(const Sample& sample, std::size_t position, std::size_t count, const FramePhaseReportContext& context)
{
    if (position + context.NewestFrames >= count)
        return true;
    return context.MinFrameMs && static_cast<double>(sample.FrameMs) >= *context.MinFrameMs;
}

// The ECS wave trace frame that began inside `sample`, or null.
const EcsFrame* FindEcsFrame(const Sample& sample, std::span<const EcsFrame> ecsFrames)
{
    const auto first = std::lower_bound(ecsFrames.begin(), ecsFrames.end(), sample.UpdateBeginNs,
                                        [](const EcsFrame& frame, std::uint64_t ns) { return frame.BeginNs < ns; });
    if (first == ecsFrames.end() || first->BeginNs >= sample.NextUpdateBeginNs)
        return nullptr;
    return &*first;
}

json DescribeEcs(const EcsFrame& frame, const FramePhaseReportContext& context)
{
    std::vector<const ECS::SystemWaveTrace::SystemSample*> systems;
    systems.reserve(frame.SystemCount);
    for (std::size_t s = 0; s < frame.SystemCount; ++s)
        systems.push_back(&frame.Systems[s]);
    const std::size_t named = std::min(kSlowestSystems, systems.size());
    std::partial_sort(systems.begin(), systems.begin() + static_cast<std::ptrdiff_t>(named), systems.end(),
                      [](const auto* a, const auto* b) { return a->EndNs - a->BeginNs > b->EndNs - b->BeginNs; });
    json slowest = json::array();
    for (std::size_t i = 0; i < named; ++i)
    {
        const auto& system = *systems[i];
        slowest.push_back({{"name", SystemName(context, system.Slot)},
                           {"ms", static_cast<double>(system.EndNs - system.BeginNs) / kNsPerMs},
                           {"onWorker", system.Worker != ECS::SystemWaveTrace::kCallerThread}});
    }
    return json{{"ecsMs", static_cast<double>(frame.EndNs - frame.BeginNs) / kNsPerMs},
                {"slowestSystems", std::move(slowest)}};
}

json DescribeExcess(const Sample& sample, const std::array<double, kColumns>& medians)
{
    // (ms above the median, column); a tie keeps table order, so a phase precedes its sub-phase.
    std::vector<std::pair<double, std::size_t>> excess;
    for (std::size_t column = 0; column < kColumns; ++column)
    {
        if (!IsAttributable(column))
            continue;
        const double above = Column(sample, column) - medians[column];
        if (above >= kMinExcessMs)
            excess.emplace_back(above, column);
    }
    const std::size_t named = std::min(kExcessPhases, excess.size());
    std::partial_sort(excess.begin(), excess.begin() + static_cast<std::ptrdiff_t>(named), excess.end(),
                      [](const auto& a, const auto& b)
                      { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    json out = json::array();
    for (std::size_t i = 0; i < named; ++i)
    {
        const std::size_t column = excess[i].second;
        out.push_back({{"phase", ColumnName(column)},
                       {"ms", Column(sample, column)},
                       {"aboveMedianMs", excess[i].first}});
    }
    return out;
}

json DescribeFrame(const Sample& sample, std::uint64_t newestEndNs, const std::array<double, kColumns>& medians,
                   const FramePhaseReportContext& context)
{
    json phases = json::object();
    for (std::size_t column = 0; column < kColumns; ++column)
    {
        const double value = Column(sample, column);
        if (value != 0.0)
            phases[ColumnName(column)] = value;
    }
    json frame = {{"frameIndex", sample.FrameIndex},
                  {"frameMs", sample.FrameMs},
                  {"agoMs", static_cast<double>(newestEndNs - sample.UpdateBeginNs) / kNsPerMs},
                  {"sceneViewRendered", sample.SceneViewRendered},
                  {"jobsPublished", sample.JobsPublished},
                  {"graphSubmissions", sample.GraphSubmissions},
                  {"graphRenderPasses", sample.GraphRenderPasses},
                  {"excess", DescribeExcess(sample, medians)},
                  {"phases", std::move(phases)}};
    if (const EcsFrame* ecs = FindEcsFrame(sample, context.EcsFrames))
        frame["ecs"] = DescribeEcs(*ecs, context);
    return frame;
}

} // namespace

json BuildFramePhaseReport(std::span<const Sample> frames, const FramePhaseReportContext& context)
{
    json reply = {{"capacity", FramePhaseHistory::kCapacity},
                  {"framesRecorded", context.FramesRecorded},
                  {"window", frames.size()},
                  {"fixedFrameRate", context.FixedFrameRate},
                  {"ecsTraceEnabled", context.EcsTraceEnabled},
                  {"ecsTraceFrames", context.EcsFrames.size()}};
    if (frames.empty())
    {
        reply["frames"] = json::array();
        return reply;
    }

    std::array<double, kColumns> medians{};
    json phaseSummary = json::object();
    std::vector<double> sorted(frames.size());
    for (std::size_t column = 0; column < kColumns; ++column)
    {
        for (std::size_t f = 0; f < frames.size(); ++f)
            sorted[f] = Column(frames[f], column);
        std::sort(sorted.begin(), sorted.end());
        medians[column] = Percentile(sorted, 0.5);
        if (sorted.back() != 0.0 || sorted.front() != 0.0)
            phaseSummary[ColumnName(column)] = SortedDistribution(sorted);
    }

    std::vector<double> frameMs;
    frameMs.reserve(frames.size());
    for (const Sample& sample : frames)
        frameMs.push_back(sample.FrameMs);

    const std::uint64_t newestEndNs = frames.back().NextUpdateBeginNs;
    json listed = json::array();
    std::vector<double> intervalFrames, intervalMs;
    const Sample* previousSpike = nullptr;
    for (std::size_t f = 0; f < frames.size(); ++f)
    {
        const Sample& sample = frames[f];
        if (!IsListed(sample, f, frames.size(), context))
            continue;
        listed.push_back(DescribeFrame(sample, newestEndNs, medians, context));
        const bool spike = context.MinFrameMs && static_cast<double>(sample.FrameMs) >= *context.MinFrameMs;
        if (!spike)
            continue;
        if (previousSpike)
        {
            intervalFrames.push_back(static_cast<double>(sample.FrameIndex - previousSpike->FrameIndex));
            intervalMs.push_back(static_cast<double>(sample.UpdateBeginNs - previousSpike->UpdateBeginNs) / kNsPerMs);
        }
        previousSpike = &sample;
    }

    json summary = {{"frameMs", Distribution(std::move(frameMs))},
                    {"spanMs", static_cast<double>(newestEndNs - frames.front().UpdateBeginNs) / kNsPerMs},
                    {"phases", std::move(phaseSummary)}};
    if (context.MinFrameMs)
    {
        summary["minFrameMs"] = *context.MinFrameMs;
        summary["framesAtOrAbove"] = static_cast<std::uint64_t>(intervalFrames.size() + (previousSpike ? 1 : 0));
        summary["spikeIntervalFrames"] = Distribution(std::move(intervalFrames));
        summary["spikeIntervalMs"] = Distribution(std::move(intervalMs));
    }
    reply["summary"] = std::move(summary);
    reply["frames"] = std::move(listed);
    return reply;
}

} // namespace GameEngine::Editor
