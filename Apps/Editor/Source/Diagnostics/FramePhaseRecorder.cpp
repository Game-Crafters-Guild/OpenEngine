#include "Diagnostics/FramePhaseRecorder.h"

#include "ECS/SystemWaveTrace.h"
#include "Engine/Rendering/RenderServices.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::Editor
{

namespace
{

constexpr double kNsPerMs = 1e6;

float Ms(double ms)
{
    return static_cast<float>(ms);
}

void CopyRenderServicesStats(const Engine::Renderer::RenderServices& services, FramePhaseHistory::Sample& sample)
{
    const auto& timeline = services.GetRenderTimelineStats();
    sample.SortMs = Ms(timeline.SortMs);
    sample.BucketerScheduleMs = Ms(timeline.BucketerScheduleMs);
    sample.ExtractionMs = Ms(services.GetRenderExtractionStats().TotalMs);
}

void CopyRenderGraphStats(const Rendering::RenderGraph::RGFrame& frame, FramePhaseHistory::Sample& sample)
{
    const auto& stats = frame.Stats();
    sample.GraphCompileMs = Ms(stats.CompileMs);
    sample.GraphScheduleMs = Ms(stats.ScheduleMs);
    sample.GraphBarriersMs = Ms(stats.GenerateBarriersMs);
    sample.GraphSubmissionPlanMs = Ms(stats.BuildSubmissionPlanMs);
    sample.GraphQueueSubmitMs = Ms(stats.QueueSubmitMs);
    sample.GraphBarrierEmitMs = Ms(stats.BarrierEmitMs);
    sample.GraphSubmissions = stats.SubmissionsMade;
    sample.GraphRenderPasses = stats.RenderPassesBegun;
}

void CopyDeviceSyncTimings(const Rendering::IDevice& device, FramePhaseHistory::Sample& sample)
{
    Rendering::IDevice::FrameSyncTimings sync{};
    if (!device.GetLastFrameSyncTimings(sync))
        return;
    sample.FenceWaitMs = Ms(sync.beginFrameWaitMs);
    sample.AcquireMs = Ms(sync.acquireMs);
    sample.PresentTransitionSubmitMs = Ms(sync.presentTransitionSubmitMs);
    sample.QueuePresentMs = Ms(sync.presentMs);
    sample.GpuFramePeriodMs = Ms(sync.frameGpuPeriodMs);
}

void CopySceneView(const FramePhaseRecorder::FrameInputs& inputs, FramePhaseHistory::Sample& sample)
{
    const SceneViewFrameTimings& scene = inputs.SceneView;
    sample.SceneViewRendered = scene.frameIndex == sample.FrameIndex;
    if (!sample.SceneViewRendered)
        return;
    sample.RenderPreloopMs = Ms(inputs.RenderPreloopMs);
    sample.BeginFrameMs = Ms(scene.beginFrameMs);
    sample.WorldViewsMs = Ms(scene.worldViewsMs);
    sample.ThumbnailsMs = Ms(scene.thumbnailsMs);
    sample.UiRecordMs = Ms(scene.uiRecordMs);
    sample.TerminalPassesMs = Ms(scene.terminalPassesMs);
    sample.RgCompileMs = Ms(scene.rgCompileMs);
    sample.RgExecuteMs = Ms(scene.rgExecuteMs);
    sample.VariantCompileMs = Ms(scene.variantCompileMs);
    sample.PrewarmMs = Ms(scene.prewarmMs);
    sample.PresentMs = Ms(scene.presentMs);
}

} // namespace

void FramePhaseRecorder::RecordPreviousFrame(const FrameInputs& inputs)
{
    const std::uint64_t nowNs = ECS::SystemWaveTrace::NowNs();
    const std::uint64_t jobsPublished = inputs.Jobs ? inputs.Jobs->GetPublishedJobCount() : 0;
    const bool havePreviousFrame = m_UpdateBeginNs != 0;
    FramePhaseHistory::Sample sample;
    sample.UpdateBeginNs = m_UpdateBeginNs;
    sample.JobsPublished = jobsPublished - m_JobsPublished;
    m_UpdateBeginNs = nowNs;
    m_JobsPublished = jobsPublished;
    if (!havePreviousFrame || inputs.FrameCount == 0)
        return;

    sample.FrameIndex = inputs.FrameCount - 1;
    sample.NextUpdateBeginNs = nowNs;
    sample.FrameMs = inputs.FixedFrameRate ? Ms(static_cast<double>(nowNs - sample.UpdateBeginNs) / kNsPerMs)
                                           : Ms(inputs.DeltaSeconds * 1000.0);

    const Application::FramePhaseTimings& phases = inputs.Phases;
    sample.PollMs = Ms(phases.PollEventsMs);
    sample.InputMs = Ms(phases.InputMs);
    sample.AppUpdateMs = Ms(phases.AppUpdateMs);
    sample.EngineUpdateMs = Ms(phases.EngineUpdateMs);
    sample.RenderMs = Ms(phases.RenderMs);
    sample.SleepMs = Ms(phases.SleepMs);

    sample.PreUiMs = Ms(inputs.AppUpdate.preUiMs);
    sample.DebugPanelsMs = Ms(inputs.AppUpdate.debugPanelsMs);
    sample.UiWindowsMs = Ms(inputs.AppUpdate.uiWindowsMs);
    sample.UpdateTailMs = Ms(inputs.AppUpdate.tailMs);

    CopySceneView(inputs, sample);
    if (inputs.Services)
        CopyRenderServicesStats(*inputs.Services, sample);
    if (inputs.MainFrame)
        CopyRenderGraphStats(*inputs.MainFrame, sample);
    if (inputs.MainDevice)
        CopyDeviceSyncTimings(*inputs.MainDevice, sample);
    m_History.Record(sample);
}

} // namespace GameEngine::Editor
