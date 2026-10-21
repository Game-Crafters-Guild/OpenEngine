#pragma once

#include "Core/Application.h"
#include "Diagnostics/EditorFrameTimings.h"
#include "Diagnostics/FramePhaseHistory.h"

#include <cstdint>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{
namespace Rendering
{
class IDevice;
namespace RenderGraph
{
class RGFrame;
}
} // namespace Rendering
namespace Engine::Renderer
{
class RenderServices;
}
} // namespace GameEngine

namespace GameEngine::Editor
{

/**
 * @brief Fills the editor's FramePhaseHistory: once per main-loop frame, at the top of
 * EditorApplication::Update, it records the frame before from the timings the frame loop
 * already measured.
 *
 * At that point every producer still holds the previous frame's values: Application's phase
 * timings and frame delta, the AppUpdate sub-phases, the Scene View breakdown (stamped with the
 * frame it was measured in, so a frame that rendered no Scene View records none), the render
 * graph's and the device's last-frame stats. The first call only stamps the start of the frame
 * it opens.
 */
class FramePhaseRecorder
{
  public:
    /// What the editor hands over at the top of a frame. Pointers may be null when the producer
    /// does not exist (no main window yet, no device, no render services, no job system).
    struct FrameInputs
    {
        // Application::GetFrameCount() now: the frame that is starting. The recorded frame is the
        // one before it.
        std::uint64_t FrameCount = 0;
        // Application::GetDeltaTime(): the recorded frame's interval, start to start.
        double DeltaSeconds = 0.0;
        // Application::IsFixedFrameRateEnabled(): DeltaSeconds is then the fixed step, not wall
        // time, so the recorded FrameMs is taken from this recorder's own stamps instead.
        bool FixedFrameRate = false;
        Application::FramePhaseTimings Phases;
        AppUpdateSubPhaseTimings AppUpdate;
        SceneViewFrameTimings SceneView;
        double RenderPreloopMs = 0.0;
        const Rendering::RenderGraph::RGFrame* MainFrame = nullptr;
        const Rendering::IDevice* MainDevice = nullptr;
        const Engine::Renderer::RenderServices* Services = nullptr;
        const JobSystem::WorkStealingThreadPool* Jobs = nullptr;
    };

    /// Records the frame before `inputs.FrameCount` and stamps the start of the one beginning now.
    void RecordPreviousFrame(const FrameInputs& inputs);

    const FramePhaseHistory& GetHistory() const { return m_History; }

  private:
    FramePhaseHistory m_History;
    // SystemWaveTrace::NowNs() at the previous call: the start of the frame being recorded.
    std::uint64_t m_UpdateBeginNs = 0;
    // The job system's published-job count at the previous call.
    std::uint64_t m_JobsPublished = 0;
};

} // namespace GameEngine::Editor
