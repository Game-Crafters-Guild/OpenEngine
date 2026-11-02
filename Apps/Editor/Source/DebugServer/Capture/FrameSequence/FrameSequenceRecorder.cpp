#include "DebugServer/Capture/FrameSequence/FrameSequenceRecorder.h"

#include "Core/Engine.h"
#include "DebugServer/Capture/ScreenshotUtils.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "Editor/CaptureOutputDirectory.h"
#include "EditorApplication/EditorApplication.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Image/Encode.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/Passes/TemporalDither.h"
#include "SceneView/SceneViewController.h"
#include "UI/UITextureSpace.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine
{

namespace
{

using json = nlohmann::json;

// Longest recording a request may ask for: ten minutes at 60 fps.
constexpr uint32_t kMaxFramesLimit = 36000;
constexpr double kMaxFrameRate = 240.0;
// Polls a stop request waits for the frames in flight to finish writing.
constexpr int kStopPollBudget = 30000;
constexpr const char* kListingFileName = "frames.csv";

// The format the encode pass writes and the readback returns: the bytes of a frame file.
constexpr Rendering::TextureFormat kRecordedFormat = Rendering::TextureFormat::RGBA8_UNORM;

// Writes the RGBA8 bytes as they are, with no header.
bool WriteRawFile(const std::filesystem::path& path, const std::vector<uint8_t>& rgba8)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(rgba8.data()), static_cast<std::streamsize>(rgba8.size()));
    return static_cast<bool>(file);
}

// Declares the screen's encode pass from the recorded texture into an RGBA8 transient, so the GPU
// applies the transfer curve, the deband and the dither, and the readback copies 4 bytes per pixel.
// The quantizer is the shared transfer decision, so a frame file matches the screen and the movie.
std::optional<Rendering::RenderGraph::RGTexture> DeclareEncodedFrame(Rendering::RenderGraph::RGFrame& frame,
                                                                     Rendering::RenderGraph::RGTexture source,
                                                                     UI::UITextureSpace sourceSpace,
                                                                     Rendering::TextureFormat presentedFormat,
                                                                     uint32_t frameOrdinal)
{
    namespace Passes = Rendering::Passes;
    const auto& sourceDesc = frame.Graph().ResourceDesc(source.Id);
    Rendering::TextureDesc desc{};
    desc.width = std::max(1u, sourceDesc.Width);
    desc.height = std::max(1u, sourceDesc.Height);
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(kRecordedFormat);
    desc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                 static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    desc.debugName = "FrameRecording.EncodedRGBA8";
    const auto encoded = frame.CreateTexture(desc.debugName, desc);
    if (!encoded.IsValid())
        return std::nullopt;

    const bool sourceIsEncoded = UI::IsEncodedAtRest(sourceSpace);
    const Passes::FinalizeParams params{
        .InputSpace = sourceIsEncoded ? Passes::FinalizeInputSpace::EncodedSrgb : Passes::FinalizeInputSpace::Linear,
        .Quantizer = Passes::SelectTransferQuantizer(sourceIsEncoded, presentedFormat, kRecordedFormat),
        .VolumeDebandThresholdLsb = Passes::GetBackbufferOutputDebandThresholdLsb(),
        .DitherPhase = Passes::MovieDitherPhase(frameOrdinal)};
    if (!Passes::AddSRGBEncodePassRG(frame, source, encoded, params, "FrameRecording.Encode").IsValid())
        return std::nullopt;
    return encoded;
}

} // namespace

// Written by the encode job before it completes; read on the main thread after IsDone.
struct FrameSequenceRecorder::EncodeOutcome
{
    bool Written = false;
    double EncodeMs = 0.0;
};

struct FrameSequenceRecorder::FrameEncodeRequest
{
    Rendering::ViewReadbackResult Readback;
    std::filesystem::path Path;
    Editor::FrameFileFormat Format = Editor::FrameFileFormat::Png;
};

// The texture one recorded frame reads back, and the space its bytes hold.
struct FrameSequenceRecorder::RecordedTexture
{
    Rendering::RenderGraph::RGTexture Texture{};
    UI::UITextureSpace Space = UI::UITextureSpace::DisplayLinearSdr();
};

void FrameSequenceRecorder::RegisterHandlers(EditorDebugServer& server, EditorApplication& app)
{
    auto recorder = std::make_shared<FrameSequenceRecorder>(server, app);
    server.RegisterHandler("start_frame_recording", [recorder](const EditorDebugServer::RequestContext& request)
                           { return recorder->Start(request.params); });
    server.RegisterHandler("stop_frame_recording", [recorder](const EditorDebugServer::RequestContext& request)
                           { return recorder->Stop(request.id); });
}

// Runs on a job worker: writes one encoded RGBA8 readback as PNG or raw bytes.
void FrameSequenceRecorder::EncodeFrame(const FrameEncodeRequest& request, EncodeOutcome& outcome)
{
    const auto begin = std::chrono::steady_clock::now();
    const auto& readback = request.Readback;
    if (readback.format != kRecordedFormat)
        return; // a readback in another format has no valid frame file; it counts as failed
    outcome.Written = request.Format == Editor::FrameFileFormat::Raw
                          ? WriteRawFile(request.Path, readback.pixels)
                          : Image::WritePngFile(request.Path, readback.pixels, readback.width, readback.height);
    outcome.EncodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

json FrameSequenceRecorder::Start(const json& params)
{
    if (m_Recording)
        return Editor::RefuseRequest("A frame recording is running or still writing its frames. Call stop_frame_recording and wait for its reply.");

    const std::string target = params.value("target", std::string("viewport"));
    if (target != "viewport" && target != "window")
        return Editor::RefuseRequest("Unknown target '" + target + "'. Use 'viewport' or 'window'.");

    const std::string format = params.value("format", std::string("png"));
    if (format != "png" && format != "raw")
        return Editor::RefuseRequest("Unknown format '" + format + "'. Use 'png' or 'raw'.");

    uint32_t maxFrames = Editor::kFrameRecordingDefaultMaxFrames;
    if (const auto it = params.find("maxFrames"); it != params.end())
    {
        if (!it->is_number_integer() || it->get<int64_t>() < 1 || it->get<int64_t>() > kMaxFramesLimit)
            return Editor::RefuseRequest("maxFrames must be an integer from 1 to " +
                                         std::to_string(kMaxFramesLimit) + ".");
        maxFrames = it->get<uint32_t>();
    }

    double frameRate = Editor::kFrameRecordingDefaultFrameRate;
    if (const auto it = params.find("frameRate"); it != params.end())
    {
        if (!it->is_number() || it->get<double>() < 0.0 || it->get<double>() > kMaxFrameRate)
            return Editor::RefuseRequest("frameRate must be a number from 0 (every presented frame) to " +
                                         std::to_string(static_cast<int>(kMaxFrameRate)) + ".");
        frameRate = it->get<double>();
    }

    std::filesystem::path directory =
        Editor::CaptureOutputDirectory() / ("frames_" + std::to_string(++m_RecordingSequence));
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return Editor::RefuseRequest("Cannot create the recording folder " + directory.string() + ": " +
                                     error.message());

    m_Target = target == "window" ? RecordingTarget::Window : RecordingTarget::Viewport;
    m_Directory = std::move(directory);
    m_Format = format == "raw" ? Editor::FrameFileFormat::Raw : Editor::FrameFileFormat::Png;
    m_FrameWidth = 0;
    m_FrameHeight = 0;
    m_Recording.emplace(maxFrames, m_Format, frameRate);
    m_App.SetFrameRateCap(m_Recording->PresentCapFps());
    m_StartTime = std::chrono::steady_clock::now();
    m_FinishedReply.reset();
    QueueRenderPhase();
    LOG_INFO("Frame recording: started, target={}, format={}, maxFrames={}, frameRate={}, folder={}", target, format,
             maxFrames, frameRate, m_Directory.string());
    return json{{"directory", m_Directory.string()},
                {"target", target},
                {"format", format},
                {"maxFrames", maxFrames},
                {"frameRate", frameRate}};
}

json FrameSequenceRecorder::Stop(const std::string& requestId)
{
    if (!m_Recording && !m_FinishedReply)
        return Editor::RefuseRequest("No frame recording is running. Call start_frame_recording first.");
    if (m_Recording)
    {
        m_Recording->Stop();
        m_App.SetFrameRateCap(m_Recording->PresentCapFps());
    }
    m_Server.EnqueueDeferredResponse(
        requestId, [self = shared_from_this()](json& outResult) { return self->PollStop(outResult); },
        kStopPollBudget);
    return EditorDebugServer::DeferredMarker();
}

void FrameSequenceRecorder::QueueRenderPhase()
{
    if (m_RenderPhaseQueued)
        return;
    m_RenderPhaseQueued = true;
    m_Server.EnqueueRenderCallback([self = shared_from_this()] { self->OnRenderPhase(); });
}

// Runs once per rendered frame while a recording is on. The server runs a callback queued
// from inside a callback on the next frame, so the re-queue below records every frame.
void FrameSequenceRecorder::OnRenderPhase()
{
    m_RenderPhaseQueued = false;
    if (!m_Recording)
        return;

    PollReadbacks();
    PollEncodes();
    if (m_Recording->IsCapturing())
        CaptureThisFrame();
    if (m_Recording->TakeDropNoticeDue(MsSinceStart()))
        LOG_WARNING("Frame recording: {} frames dropped so far; the readback or the encode did not keep up",
                    m_Recording->GetSummary().Dropped);

    if (m_Recording->IsCapturing() || !m_Recording->IsDrained())
    {
        QueueRenderPhase();
        return;
    }
    Finish();
}

void FrameSequenceRecorder::CaptureThisFrame()
{
    if (m_App.m_Windows.empty() || !m_App.m_Windows[0] || !m_App.m_Windows[0]->renderCtx)
        return;
    auto& window = *m_App.m_Windows[0];
    auto* device = window.renderCtx->GetDevice();
    auto* frame = window.rg2Capture.frame;
    if (!device || !frame || window.rg2Capture.frameIndex != frame->FrameIndex())
        return;
    const auto recorded = FindRecordedTexture(window, *frame);
    if (!recorded)
        return;

    const auto index = m_Recording->OnFramePresented(window.rg2Capture.frameIndex, MsSinceStart());
    if (!index)
        return;
    const auto presentedFormat = device->GetWindowTargetSwapchainFormat(window.renderCtx->GetWindowTarget());
    const auto encoded = DeclareEncodedFrame(*frame, recorded->Texture, recorded->Space, presentedFormat, *index);
    auto ticket = encoded ? Rendering::RequestTextureReadbackRG(device, *frame, *encoded, "FrameRecording") : nullptr;
    if (!ticket)
    {
        m_Recording->OnReadbackLost(*index);
        return;
    }
    m_Readbacks.push_back(PendingReadback{*index, std::move(ticket)});
}

// The Scene View's pipeline output (post-processed, overlays included) or the window's
// composite. Nothing when this frame declared neither: the pane is hidden or the view is
// catching up, and the frame is not a presented frame of the target.
std::optional<FrameSequenceRecorder::RecordedTexture> FrameSequenceRecorder::FindRecordedTexture(
    EditorApplication::EditorWindowContext& window, Rendering::RenderGraph::RGFrame& frame) const
{
    if (m_Target == RecordingTarget::Window)
    {
        if (window.rg2Capture.finalLinearId == 0xFFFFFFFFu)
            return std::nullopt;
        RecordedTexture recorded;
        recorded.Texture.Id = window.rg2Capture.finalLinearId;
        recorded.Space = window.rg2Capture.compositeSpace;
        return recorded;
    }

    auto* renderServices = m_App.MainRenderServices();
    const SceneViewController* controller = Editor::ResolveActiveSceneController(window);
    if (!renderServices || !controller || controller->GetViewId() == 0)
        return std::nullopt;
    const auto output = renderServices->GetPipelineOutputRG(frame, controller->GetViewId());
    if (!output.IsValid())
        return std::nullopt;
    return RecordedTexture{output.Out, renderServices->GetPipelineOutputSpaceRG()};
}

void FrameSequenceRecorder::PollReadbacks()
{
    for (size_t slot = 0; slot < m_Readbacks.size();)
    {
        auto& pending = m_Readbacks[slot];
        Rendering::ViewReadbackResult readback;
        if (pending.Ticket->TryGet(readback))
        {
            m_Recording->OnReadbackFinished(pending.Index, readback.width, readback.height);
            SubmitEncode(pending, std::move(readback));
        }
        else if (pending.Ticket->IsConsumed())
        {
            m_Recording->OnReadbackLost(pending.Index);
        }
        else
        {
            ++slot;
            continue;
        }
        m_Readbacks.erase(m_Readbacks.begin() + static_cast<std::ptrdiff_t>(slot));
    }
}

void FrameSequenceRecorder::PollEncodes()
{
    for (size_t slot = 0; slot < m_Encodes.size();)
    {
        auto& pending = m_Encodes[slot];
        if (pending.Job.IsValid() && !pending.Job.IsDone())
        {
            ++slot;
            continue;
        }
        if (pending.Outcome->Written)
            m_Recording->OnFrameWritten(pending.Index, pending.Outcome->EncodeMs);
        else
            m_Recording->OnFrameWriteFailed(pending.Index);
        m_Encodes.erase(m_Encodes.begin() + static_cast<std::ptrdiff_t>(slot));
    }
}

void FrameSequenceRecorder::SubmitEncode(const PendingReadback& pending, Rendering::ViewReadbackResult&& readback)
{
    m_FrameWidth = readback.width;
    m_FrameHeight = readback.height;
    auto outcome = std::make_shared<EncodeOutcome>();
    auto request = std::make_shared<FrameEncodeRequest>(
        FrameEncodeRequest{std::move(readback), m_Directory / m_Recording->FrameFileName(pending.Index), m_Format});
    auto job = EngineCore::GetInstance().GetJobSystem().Submit([request, outcome] { EncodeFrame(*request, *outcome); },
                                                               Editor::kFrameRecordingEncodePriority);
    m_Encodes.push_back(PendingEncode{pending.Index, std::move(job), std::move(outcome)});
}

bool FrameSequenceRecorder::PollStop(json& outResult)
{
    if (m_Recording)
    {
        PollReadbacks();
        PollEncodes();
        if (!m_Recording->IsDrained())
            return false;
        Finish();
    }
    outResult = m_FinishedReply.value_or(Editor::RefuseRequest("Another stop_frame_recording call took this recording's reply."));
    m_FinishedReply.reset();
    return true;
}

void FrameSequenceRecorder::Finish()
{
    const auto summary = m_Recording->GetSummary();
    const std::filesystem::path listingPath = m_Directory / kListingFileName;
    std::ofstream listing(listingPath, std::ios::binary);
    listing << m_Recording->BuildListing();
    listing.close();

    const double seconds = summary.DurationMs / 1000.0;
    json reply{{"directory", m_Directory.string()},
               {"listing", listingPath.string()},
               {"listingWritten", static_cast<bool>(listing)},
               {"target", m_Target == RecordingTarget::Window ? "window" : "viewport"},
               {"format", m_Format == Editor::FrameFileFormat::Raw ? "raw" : "png"},
               {"width", m_FrameWidth},
               {"height", m_FrameHeight},
               {"presented", summary.Presented},
               {"written", summary.Written},
               {"skipped", summary.Skipped},
               {"dropped", summary.Dropped},
               {"failed", summary.Failed},
               {"durationMs", summary.DurationMs},
               {"presentedFps", summary.PresentedFps},
               {"writtenFps", seconds > 0.0 ? summary.Written / seconds : 0.0},
               {"encodeMsMean", summary.EncodeMsMean},
               {"encodeMsMax", summary.EncodeMsMax}};
    LOG_INFO("Frame recording: stopped, {} presented, {} written, {} dropped, {} failed, folder={}",
             summary.Presented, summary.Written, summary.Dropped, summary.Failed, m_Directory.string());
    m_App.SetFrameRateCap(m_Recording->PresentCapFps());
    m_Readbacks.clear();
    m_Encodes.clear();
    m_Recording.reset();
    m_FinishedReply = std::move(reply);
}

double FrameSequenceRecorder::MsSinceStart() const
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_StartTime).count();
}

} // namespace GameEngine
