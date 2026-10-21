#include "DebugServer/FramePhaseDebugHandlers.h"

#include "Core/Application.h"
#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/FramePhaseReport.h"
#include "Diagnostics/FramePhaseHistory.h"
#include "ECS/SystemWaveTrace.h"
#include "ECS/Systems.h"
#include "ECSModules/Rendering/RenderingLoop.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

// get_frame_phases: describe the newest frames of the phase history, joined with the ECS wave
// trace's frames where it holds them.
json HandleFramePhases(const EditorDebugServer::RequestContext& ctx, const Editor::FramePhaseHistory& history,
                       const Application& app)
{
    const std::int64_t window =
        ctx.params.value("window", static_cast<std::int64_t>(Editor::FramePhaseHistory::kCapacity));
    if (window < 1)
        return Editor::RefuseRequest("window must be at least 1 (the newest frames to summarise, up to " +
                                     std::to_string(Editor::FramePhaseHistory::kCapacity) + ")");
    const std::int64_t newest = ctx.params.value("frames", static_cast<std::int64_t>(0));
    if (newest < 0)
        return Editor::RefuseRequest("frames must be 0 or more (the newest frames to list one by one)");

    Editor::FramePhaseReportContext context;
    if (ctx.params.contains("minFrameMs"))
    {
        const double minFrameMs = ctx.params.value("minFrameMs", 0.0);
        if (!(minFrameMs > 0.0))
            return Editor::RefuseRequest("minFrameMs must be a frame time above 0 ms, such as 10");
        context.MinFrameMs = minFrameMs;
    }
    context.NewestFrames = static_cast<std::size_t>(newest);
    context.FramesRecorded = history.GetFramesRecorded();
    context.FixedFrameRate = app.IsFixedFrameRateEnabled();

    std::vector<Editor::FramePhaseHistory::Sample> frames;
    history.CopyNewest(frames, static_cast<std::size_t>(
                                   std::min<std::int64_t>(window, Editor::FramePhaseHistory::kCapacity)));

    std::vector<ECS::SystemWaveTrace::Frame> ecsFrames;
    std::vector<std::string> systemNames;
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    if (auto* systems = loop ? loop->GetSystemManager() : nullptr)
    {
        const ECS::SystemWaveTrace& trace = systems->GetWaveTrace();
        context.EcsTraceEnabled = trace.IsEnabled();
        if (trace.GetFramesRecorded() > 0)
        {
            trace.CopyFrames(ecsFrames, ECS::SystemWaveTrace::kFrameCapacity);
            systemNames.resize(systems->GetSequentialSystemCount());
            for (std::size_t slot = 0; slot < systemNames.size(); ++slot)
            {
                const char* name = systems->GetSequentialSystemName(slot);
                systemNames[slot] = name ? name : "<retired>";
            }
        }
    }
    context.EcsFrames = ecsFrames;
    context.SystemNames = systemNames;
    return Editor::BuildFramePhaseReport(frames, context);
}

} // namespace

void RegisterFramePhaseDebugHandlers(EditorDebugServer& server, const Editor::FramePhaseHistory& history,
                                     const Application& app)
{
    // get_frame_phases — the newest frames of the editor's phase history (FramePhaseHistory, the
    // last 1024 main-loop frames, always recorded), so a slow frame is attributed to the phase that
    // carried it after the fact. Params:
    //   window (int, optional): summarise the newest N frames (default and cap 1024).
    //   minFrameMs (number, optional): list every frame in the window at or above this frame time,
    //     and the spacing between them.
    //   frames (int, optional): also list the newest N frames whatever their time (default 0).
    // Reply: capacity, framesRecorded, window, fixedFrameRate, ecsTraceEnabled, ecsTraceFrames,
    //   summary {frameMs, spanMs, phases, and with minFrameMs: framesAtOrAbove,
    //   spikeIntervalFrames, spikeIntervalMs}, frames[] (see FramePhaseReport.h). A listed frame
    //   names its slowest ECS systems while get_ecs_wave_trace is armed and still holds the frame.
    server.RegisterHandler("get_frame_phases", [&history, &app](const EditorDebugServer::RequestContext& ctx)
                           { return HandleFramePhases(ctx, history, app); });
}

} // namespace GameEngine
