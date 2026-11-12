// get_gpu_profiler's reply (DescribeGpuProfiler) over hand-built pass rows: the pipeline-statistics
// counters on a measured row, no counter keys while they are off, the reason a row has no counters,
// and the reply-level reason on a device without pipeline-statistics queries.

#include <gtest/gtest.h>

#include "DebugServer/GpuProfiler/GpuProfilerReport.h"

#include <cstring>
#include <string>
#include <vector>

namespace
{

namespace RG = GameEngine::Rendering::RenderGraph;
using GameEngine::Editor::DescribeGpuProfiler;
using GameEngine::Editor::GpuProfilerReportInput;
using PassTiming = RG::RGFrame::RGPassTiming;
using StatisticsState = RG::RGFrame::RGPassStatisticsState;

PassTiming MakePass(const char* name, StatisticsState state)
{
    PassTiming timing;
    std::strncpy(timing.Name, name, sizeof(timing.Name) - 1);
    timing.CpuMs = 0.25;
    timing.GpuSpanMs = 1.5;
    timing.StatisticsState = state;
    return timing;
}

GpuProfilerReportInput MakeInput(const std::vector<PassTiming>& timings, bool statisticsEnabled,
                                 bool statisticsSupported)
{
    GpuProfilerReportInput input;
    input.Timings = timings;
    input.ProfilingEnabled = true;
    input.StatisticsEnabled = statisticsEnabled;
    input.StatisticsSupported = statisticsSupported;
    return input;
}

} // namespace

TEST(GpuProfilerReport, MeasuredRowCarriesEveryCounter)
{
    PassTiming water = MakePass("Water", StatisticsState::Measured);
    water.Statistics.InputAssemblyVertices = 1;
    water.Statistics.InputAssemblyPrimitives = 2;
    water.Statistics.VertexShaderInvocations = 3;
    water.Statistics.ClippingInvocations = 4;
    water.Statistics.ClippingPrimitives = 5;
    water.Statistics.FragmentShaderInvocations = 6;
    water.Statistics.ComputeShaderInvocations = 7;
    const std::vector<PassTiming> timings = {water};

    const nlohmann::json reply = DescribeGpuProfiler(MakeInput(timings, true, true));

    const nlohmann::json& row = reply["lastFrame"]["passes"][0];
    ASSERT_TRUE(row.contains("stats"));
    EXPECT_FALSE(row.contains("statsMissing"));
    const nlohmann::json& stats = row["stats"];
    EXPECT_EQ(stats["inputAssemblyVertices"], 1u);
    EXPECT_EQ(stats["inputAssemblyPrimitives"], 2u);
    EXPECT_EQ(stats["vertexShaderInvocations"], 3u);
    EXPECT_EQ(stats["clippingInvocations"], 4u);
    EXPECT_EQ(stats["clippingPrimitives"], 5u);
    EXPECT_EQ(stats["fragmentShaderInvocations"], 6u);
    EXPECT_EQ(stats["computeShaderInvocations"], 7u);
    EXPECT_EQ(reply["pipelineStatistics"]["enabled"], true);
    EXPECT_EQ(reply["pipelineStatistics"]["supported"], true);
    EXPECT_FALSE(reply["pipelineStatistics"].contains("reason"));
}

TEST(GpuProfilerReport, CountersAreAbsentWhileOff)
{
    const std::vector<PassTiming> timings = {MakePass("Water", StatisticsState::Off)};

    const nlohmann::json reply = DescribeGpuProfiler(MakeInput(timings, false, true));

    const nlohmann::json& row = reply["lastFrame"]["passes"][0];
    EXPECT_FALSE(row.contains("stats"));
    EXPECT_FALSE(row.contains("statsMissing"));
    EXPECT_EQ(reply["pipelineStatistics"]["enabled"], false);
    for (const nlohmann::json& warning : reply["warnings"])
        EXPECT_EQ(warning.get<std::string>().find("Pipeline statistics were on"), std::string::npos);
}

TEST(GpuProfilerReport, CountersOnWarnThatTheFrameIsNotATimingFrame)
{
    const std::vector<PassTiming> timings = {MakePass("Water", StatisticsState::Measured)};

    const nlohmann::json reply = DescribeGpuProfiler(MakeInput(timings, true, true));

    bool warned = false;
    for (const nlohmann::json& warning : reply["warnings"])
        warned = warned || warning.get<std::string>().find("Pipeline statistics were on") != std::string::npos;
    EXPECT_TRUE(warned);
}

// The warning reads the rows: the switch can change after the rows were recorded.
TEST(GpuProfilerReport, StatisticsWarningFollowsTheRowsNotTheSwitch)
{
    const std::vector<PassTiming> recordedOff = {MakePass("Water", StatisticsState::Off)};
    const std::vector<PassTiming> recordedOn = {MakePass("Water", StatisticsState::NotReady)};

    const nlohmann::json switchedOn = DescribeGpuProfiler(MakeInput(recordedOff, true, true));
    const nlohmann::json switchedOff = DescribeGpuProfiler(MakeInput(recordedOn, false, true));

    for (const nlohmann::json& warning : switchedOn["warnings"])
        EXPECT_EQ(warning.get<std::string>().find("Pipeline statistics were on"), std::string::npos);
    bool warned = false;
    for (const nlohmann::json& warning : switchedOff["warnings"])
        warned = warned || warning.get<std::string>().find("Pipeline statistics were on") != std::string::npos;
    EXPECT_TRUE(warned);
}

TEST(GpuProfilerReport, DeviceWithoutTheFeatureStatesTheReason)
{
    const std::vector<PassTiming> timings = {MakePass("Water", StatisticsState::Unsupported)};

    const nlohmann::json reply = DescribeGpuProfiler(MakeInput(timings, true, false));

    EXPECT_EQ(reply["pipelineStatistics"]["supported"], false);
    ASSERT_TRUE(reply["pipelineStatistics"].contains("reason"));
    const std::string reason = reply["pipelineStatistics"]["reason"].get<std::string>();
    EXPECT_NE(reason.find("pipelineStatisticsQuery"), std::string::npos);
    const nlohmann::json& row = reply["lastFrame"]["passes"][0];
    EXPECT_FALSE(row.contains("stats"));
    ASSERT_TRUE(row.contains("statsMissing"));
    EXPECT_EQ(row["statsMissing"].get<std::string>(), reason);
}

TEST(GpuProfilerReport, EveryUnmeasuredStateNamesItsReason)
{
    const std::vector<PassTiming> timings = {MakePass("AsyncBlur", StatisticsState::NotGraphicsQueue),
                                             MakePass("Late", StatisticsState::QueriesUsedUp),
                                             MakePass("Lost", StatisticsState::NotReady)};

    const nlohmann::json reply = DescribeGpuProfiler(MakeInput(timings, true, true));

    const nlohmann::json& passes = reply["lastFrame"]["passes"];
    ASSERT_EQ(passes.size(), 3u);
    for (const nlohmann::json& row : passes)
        ASSERT_TRUE(row.contains("statsMissing")) << row["name"];
    EXPECT_NE(passes[0]["statsMissing"].get<std::string>().find("graphics queue"), std::string::npos);
    EXPECT_NE(passes[1]["statsMissing"].get<std::string>().find("used all"), std::string::npos);
    EXPECT_NE(passes[2]["statsMissing"].get<std::string>().find("no result"), std::string::npos);
}

// A producer's scope row sits under its pass row, and the pass total of cpuMs counts each pass once.
TEST(GpuProfilerReport, ScopeRowsSitUnderTheirPassRow)
{
    const std::vector<PassTiming> timings = {MakePass("DepthPrepass[View#1]", StatisticsState::Measured),
                                             MakePass("RenderEntities[View#1]", StatisticsState::Measured)};
    RG::RGFrame::RGScopeTiming grass;
    std::strncpy(grass.Name, "RenderEntities[View#1].Grass", sizeof(grass.Name) - 1);
    grass.PassRow = 1;
    grass.CpuMs = 0.125;
    grass.StatisticsState = StatisticsState::Measured;
    grass.Statistics.InputAssemblyPrimitives = 42;
    const std::vector<RG::RGFrame::RGScopeTiming> scopes = {grass};
    GpuProfilerReportInput input = MakeInput(timings, true, true);
    input.Scopes = scopes;

    const nlohmann::json reply = DescribeGpuProfiler(input);

    const nlohmann::json& passes = reply["lastFrame"]["passes"];
    ASSERT_EQ(passes.size(), 2u);
    EXPECT_FALSE(passes[0].contains("scopes"));
    ASSERT_TRUE(passes[1].contains("scopes"));
    ASSERT_EQ(passes[1]["scopes"].size(), 1u);
    const nlohmann::json& row = passes[1]["scopes"][0];
    EXPECT_EQ(row["name"], "RenderEntities[View#1].Grass");
    EXPECT_EQ(row["stats"]["inputAssemblyPrimitives"], 42u);
    EXPECT_DOUBLE_EQ(reply["lastFrame"]["totalCpuMs"].get<double>(), 0.5);
}

// A pass that its feature skipped is a row with its state and no times; it adds nothing to a total.
TEST(GpuProfilerReport, SkippedPassesCarryTheirState)
{
    const std::vector<PassTiming> timings = {MakePass("ShadowCascades[View#1].MinMax", StatisticsState::Off)};
    RG::RGFrame::RGSkippedPass cached;
    std::strncpy(cached.Name, "ShadowCascades[View#1].Cascade0", sizeof(cached.Name) - 1);
    cached.State = RG::RGFrame::RGSkippedPassState::Cached;
    RG::RGFrame::RGSkippedPass deferred;
    std::strncpy(deferred.Name, "ShadowCascades[View#1].Cascade3", sizeof(deferred.Name) - 1);
    deferred.State = RG::RGFrame::RGSkippedPassState::Deferred;
    const std::vector<RG::RGFrame::RGSkippedPass> skipped = {cached, deferred};
    GpuProfilerReportInput input = MakeInput(timings, true, false);
    input.Skipped = skipped;

    const nlohmann::json reply = DescribeGpuProfiler(input);

    const nlohmann::json& rows = reply["lastFrame"]["skippedPasses"];
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0]["name"], "ShadowCascades[View#1].Cascade0");
    EXPECT_EQ(rows[0]["state"], "cached");
    EXPECT_EQ(rows[1]["state"], "deferred");
    EXPECT_FALSE(rows[0].contains("gpuSpanMs"));
    EXPECT_EQ(reply["lastFrame"]["passes"].size(), 1u);
}
