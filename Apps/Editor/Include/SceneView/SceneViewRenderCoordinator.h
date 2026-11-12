#pragma once

#include "EditorApplication/EditorApplication.h"
#include "Types/FunctionRef.h"

#include <cstdint>
#include <span>
#include <vector>

namespace GameEngine
{
class SceneViewController;
class SceneViewPanel;
class SceneViewport;
class UIElement;
class UIManager;

namespace Input
{
class InputSystem;
}

namespace Rendering
{
namespace RenderGraph
{
class RGFrame;
}
}

namespace Engine::Renderer::Pipeline
{
struct ViewTargetsRG;
}

namespace Editor
{
class SceneThumbnailCapture;

class SceneViewRenderCoordinator final
{
public:
    using EditorWindowContext = EditorApplication::EditorWindowContext;

    /// The frame's Scene View input across every window: the first interacting viewport consumes it.
    struct InputFrame
    {
        Input::InputSystem* Input = nullptr;
        float DeltaSeconds = 0.0f;
        SceneViewController* ActiveInputController = nullptr;
        bool InputConsumed = false;
    };

    SceneViewRenderCoordinator() = delete;

    /// Binds the mounted Scene View panel's panes to the window's controllers; returns the panel or null.
    static SceneViewPanel* SyncMountedPanelControllers(EditorWindowContext* ctx);
    /// Visits every live viewport (SceneViewportRegistry) mounted in the window's UI.
    static void ForEachLiveViewport(EditorWindowContext* ctx, FunctionRef<void(SceneViewport&)> visit);
    /// Ticks the camera of every live viewport of the window; the first interacting viewport takes the input.
    static void TickLiveViewports(EditorWindowContext* ctx, InputFrame& frame);
    static void ForEachController(EditorWindowContext* windowContext,
                                  FunctionRef<void(SceneViewController*)> visit);
    static void SyncSelectionAcrossControllers(EditorWindowContext* ctx, SceneViewController* source);
    static void SyncViewportStateAcrossControllers(EditorWindowContext* ctx, SceneViewController* source);

    // ── RenderGraph arm (slice 7d single-view; 8e-3 quad) ──
    // Phase 1 (pre-spine): declares the targets of each renderable pane and of each live pane bound to a
    // controller outside the window's main and quad set. targets[0] is ctx->scene's entry when it declares
    // (the scene-save thumbnail reads it). Returns false when nothing declared.
    static bool CollectRG(EditorWindowContext* ctx,
                           Rendering::RenderGraph::RGFrame& frame,
                           std::vector<Engine::Renderer::Pipeline::ViewTargetsRG>& outTargets);
    // Phase 1.5 (post-spine, pre-bind): declare the editor overlay passes
    // (gizmos / selection mask / outline / composite) onto the pipeline's
    // ACTUAL FinalColor, per pane with a span entry. Needs the same targets
    // span the spine consumed (each pane's imported Depth rides in it).
    static void DeclareOverlaysRG(EditorWindowContext* ctx,
                                   Rendering::RenderGraph::RGFrame& frame,
                                   std::span<const Engine::Renderer::Pipeline::ViewTargetsRG> targets);
    // Phase 2 (post-spine): binds each declared pane's pipeline output to its viewport, every
    // frame. Returns true when a pane shows an image this frame; false when every pane shows the
    // empty placeholder.
    static bool BindRG(EditorWindowContext* ctx, Rendering::RenderGraph::RGFrame& frame,
                        bool pureRGFrame);
    // Called at the end of App Update: requests each renderable pane's view for this
    // frame's extraction, so the first frame of a pane renders the scene.
    static void RequestFramesBeforeExtraction(EditorWindowContext* ctx);

    // Failable viewport-size resolve for RenderGraph arms: false when the element is
    // unmounted/degenerate (the caller declares nothing — never the old
    // resolver's backbuffer-size fallback). Shared with the 7f game arm.
    static bool TryResolveRenderableViewportSize(const UIManager* ui,
                                                 UIElement* viewport,
                                                 std::uint32_t& outW,
                                                 std::uint32_t& outH);
};

} // namespace Editor
} // namespace GameEngine
