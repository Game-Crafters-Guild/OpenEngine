#pragma once

#include "DebugServer/Capture/FrameSequence/FrameSequenceRecording.h"
#include "EditorApplication/EditorApplication.h"
#include "JobSystem/TaskHandle.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine::Rendering
{
class RGReadbackTicket;
struct ViewReadbackResult;
} // namespace GameEngine::Rendering

namespace GameEngine::Rendering::RenderGraph
{
class RGFrame;
}

namespace GameEngine
{

class EditorDebugServer;

// Serves start_frame_recording and stop_frame_recording. A recording reads back frames of
// the Scene View or the window and writes them as numbered PNG or raw files with frames.csv.
// While a recording captures, the editor presents at twice the recording's frame rate.
// While no recording runs, no render callback is queued and no frame pays for it.
class FrameSequenceRecorder : public std::enable_shared_from_this<FrameSequenceRecorder>
{
  public:
    static void RegisterHandlers(EditorDebugServer& server, EditorApplication& app);

    FrameSequenceRecorder(EditorDebugServer& server, EditorApplication& app) : m_Server(server), m_App(app) {}

  private:
    enum class RecordingTarget
    {
        Viewport,
        Window,
    };

    struct EncodeOutcome;
    struct FrameEncodeRequest;
    struct RecordedTexture;

    struct PendingReadback
    {
        uint32_t Index = 0;
        std::shared_ptr<Rendering::RGReadbackTicket> Ticket;
    };

    struct PendingEncode
    {
        uint32_t Index = 0;
        JobSystem::TaskHandle Job;
        std::shared_ptr<EncodeOutcome> Outcome;
    };

    static void EncodeFrame(const FrameEncodeRequest& request, EncodeOutcome& outcome);

    nlohmann::json Start(const nlohmann::json& params);
    nlohmann::json Stop(const std::string& requestId);
    void QueueRenderPhase();
    void OnRenderPhase();
    void CaptureThisFrame();
    std::optional<RecordedTexture> FindRecordedTexture(EditorApplication::EditorWindowContext& window,
                                                       Rendering::RenderGraph::RGFrame& frame) const;
    void PollReadbacks();
    void PollEncodes();
    void SubmitEncode(const PendingReadback& pending, Rendering::ViewReadbackResult&& readback);
    bool PollStop(nlohmann::json& outResult);
    void Finish();
    double MsSinceStart() const;

    EditorDebugServer& m_Server;
    EditorApplication& m_App;
    std::optional<Editor::FrameSequenceRecording> m_Recording;
    RecordingTarget m_Target = RecordingTarget::Viewport;
    Editor::FrameFileFormat m_Format = Editor::FrameFileFormat::Png;
    // The size of the last frame read back, for the stop reply. frames.csv gives each frame's size.
    uint32_t m_FrameWidth = 0;
    uint32_t m_FrameHeight = 0;
    std::filesystem::path m_Directory;
    std::chrono::steady_clock::time_point m_StartTime;
    std::vector<PendingReadback> m_Readbacks;
    std::vector<PendingEncode> m_Encodes;
    // The reply of the last recording that finished, until a stop request takes it.
    std::optional<nlohmann::json> m_FinishedReply;
    uint32_t m_RecordingSequence = 0;
    bool m_RenderPhaseQueued = false;
};

} // namespace GameEngine
