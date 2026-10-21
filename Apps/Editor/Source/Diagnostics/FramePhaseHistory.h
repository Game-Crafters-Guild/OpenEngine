#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace GameEngine::Editor
{

/**
 * @brief The editor's per-frame phase history: one sample per main-loop frame,
 * for the newest kCapacity frames, so a slow frame can be attributed after the
 * fact to the phase that carried it.
 *
 * A sample copies timings the frame loop already measured (Application's phase
 * timings, the editor's AppUpdate and Scene View sub-phases, the device's sync
 * timings, the render graph's and RenderServices' per-frame stats); recording
 * reads no clock of its own beyond the one steady-clock stamp that places the
 * frame on SystemWaveTrace's time line. Always on: the ring is allocated once
 * at construction and a record is one fixed-size copy into the next slot.
 *
 * Times are milliseconds unless a name says otherwise. Every phase field is
 * zero when its producer did not run that frame (no Scene View rendered, no
 * device, no render graph).
 *
 * Threads: recorded and read on the main thread (the editor records at the top
 * of its Update and serves debug requests later in the same Update).
 */
class FramePhaseHistory
{
  public:
    static constexpr std::size_t kCapacity = 1024;

    struct Sample
    {
        // Application::GetFrameCount() while the frame ran.
        std::uint64_t FrameIndex = 0;
        // SystemWaveTrace::NowNs() at the start of this frame's
        // EditorApplication::Update and of the next frame's: the frame's ECS
        // update, its render and the next frame's poll and input fall inside.
        std::uint64_t UpdateBeginNs = 0;
        std::uint64_t NextUpdateBeginNs = 0;
        // Jobs published to the job system (every thread) between the two stamps.
        std::uint64_t JobsPublished = 0;

        // Application's frame interval: this frame's start to the next frame's
        // start, the value its 250 ms frame window holds. Under a fixed frame rate
        // Application's delta is the fixed step, not elapsed time, so the recorder
        // takes NextUpdateBeginNs - UpdateBeginNs instead.
        float FrameMs = 0.0f;

        // Application::FramePhaseTimings. PollMs is the gamepad read; the OS
        // event pump (Platform::Window::PollEvents) runs before the frame's
        // clock starts and so falls in the previous frame's FrameMs outside
        // every phase here.
        float PollMs = 0.0f;
        float InputMs = 0.0f;
        float AppUpdateMs = 0.0f;
        float EngineUpdateMs = 0.0f;
        float RenderMs = 0.0f;
        float SleepMs = 0.0f;

        // AppUpdate's sub-phases.
        float PreUiMs = 0.0f;
        float DebugPanelsMs = 0.0f;
        float UiWindowsMs = 0.0f;
        float UpdateTailMs = 0.0f;

        // Render's sub-phases for the window that rendered the Scene View;
        // zero with SceneViewRendered false when none did.
        float RenderPreloopMs = 0.0f;
        float BeginFrameMs = 0.0f;
        float WorldViewsMs = 0.0f;
        float ThumbnailsMs = 0.0f;
        float UiRecordMs = 0.0f;
        float TerminalPassesMs = 0.0f;
        float RgCompileMs = 0.0f;
        float RgExecuteMs = 0.0f;
        float VariantCompileMs = 0.0f;
        float PrewarmMs = 0.0f;
        float PresentMs = 0.0f;

        // RenderServices' render timeline and extraction (last app frame).
        float SortMs = 0.0f;
        float BucketerScheduleMs = 0.0f;
        float ExtractionMs = 0.0f;

        // The main window's render graph (RGFrame::FrameStats).
        float GraphCompileMs = 0.0f;
        float GraphScheduleMs = 0.0f;
        float GraphBarriersMs = 0.0f;
        float GraphSubmissionPlanMs = 0.0f;
        float GraphQueueSubmitMs = 0.0f;
        float GraphBarrierEmitMs = 0.0f;
        std::uint32_t GraphSubmissions = 0;
        std::uint32_t GraphRenderPasses = 0;

        // The main window's device (IDevice::FrameSyncTimings).
        float FenceWaitMs = 0.0f;
        float AcquireMs = 0.0f;
        float PresentTransitionSubmitMs = 0.0f;
        float QueuePresentMs = 0.0f;
        // Spacing of consecutive frame-end GPU timestamps, read a frame or more
        // late (when the frame's fence signals).
        float GpuFramePeriodMs = 0.0f;

        bool SceneViewRendered = false;
    };

    FramePhaseHistory();
    ~FramePhaseHistory();
    FramePhaseHistory(const FramePhaseHistory&) = delete;
    FramePhaseHistory& operator=(const FramePhaseHistory&) = delete;

    /// Writes `sample` into the next slot, overwriting the oldest once full.
    void Record(const Sample& sample);

    /// Samples recorded since construction, the overwritten ones included.
    std::uint64_t GetFramesRecorded() const { return m_FramesRecorded; }

    /// Copies up to `maxFrames` of the newest samples into `out`, oldest first,
    /// replacing its contents.
    void CopyNewest(std::vector<Sample>& out, std::size_t maxFrames) const;

  private:
    std::unique_ptr<Sample[]> m_Samples;
    std::uint64_t m_FramesRecorded = 0;
};

} // namespace GameEngine::Editor
