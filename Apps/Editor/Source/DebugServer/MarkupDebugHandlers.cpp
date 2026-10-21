#include "DebugServer/MarkupDebugHandlers.h"

#include "DebugServer/DebugHandlers.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "Markups/MarkupEditorSurfaces.h"
#include "Markups/MarkupRequests.h"
#include "SceneViewController.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{

using json = nlohmann::json;

void RegisterMarkupDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    server.RegisterHandler("markup_list", [](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::ListMarkups(Editor::CurrentMarkupRequestContext(), ctx.params);
    });
    server.RegisterHandler("markup_get", [](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::GetMarkup(Editor::CurrentMarkupRequestContext(), ctx.params);
    });
    server.RegisterHandler("markup_create", [](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::CreateMarkup(Editor::CurrentMarkupRequestContext(), ctx.params);
    });
    server.RegisterHandler("markup_update", [](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::UpdateMarkup(Editor::CurrentMarkupRequestContext(), ctx.params);
    });
    server.RegisterHandler("markup_contains", [](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::ContainsInMarkup(Editor::CurrentMarkupRequestContext(), ctx.params);
    });
    server.RegisterHandler("markup_comment", [](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::CommentOnMarkup(Editor::CurrentMarkupRequestContext(), ctx.params);
    });
    // Frames the mark-up's bounding sphere with look_at's pose math; never focus_view,
    // which would ask the operating system for the foreground.
    server.RegisterHandler("markup_frame", [&app](const EditorDebugServer::RequestContext& ctx) -> json {
        SceneViewCameraPose pose{};
        json result = Editor::FrameMarkup(Editor::CurrentMarkupRequestContext(), ctx.params, pose);
        if (Editor::IsRefusal(result))
            return result;
        if (!ApplyMainSceneViewPose(app, pose))
            return Editor::RefuseRequest("No Scene View to frame in; open one first");
        return result;
    });
    server.RegisterHandler("markup_set_visible", [](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::SetMarkupsVisible(Editor::CurrentMarkupRequestContext(), ctx.params);
    });
}

} // namespace GameEngine
