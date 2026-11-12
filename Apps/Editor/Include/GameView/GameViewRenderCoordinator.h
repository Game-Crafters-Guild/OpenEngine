#pragma once

#include "EditorApplication/EditorApplication.h"

namespace GameEngine::Editor
{

/// The Game View's per-window frame steps that run outside its controller.
class GameViewRenderCoordinator final
{
  public:
    using EditorWindowContext = EditorApplication::EditorWindowContext;

    GameViewRenderCoordinator() = delete;

    /// Call at the end of App Update, before extraction. A Game View that the frame records
    /// (an open movie session, or a mounted panel with a renderable size: the record route's
    /// facts) requests this frame's extraction, so it renders in its activation frame.
    static void RequestFrameBeforeExtraction(EditorWindowContext& ctx);
};

} // namespace GameEngine::Editor
