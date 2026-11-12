#pragma once

namespace GameEngine::Mathematics
{
struct Vector2;
}

namespace GameEngine
{
class SceneViewController;
class SceneViewport;
class UIElement;
struct UIEvent;

/// The window around Scene View panes: it owns what spans the panes (the active pane, the pan hold key, the
/// chrome keys, the context menu, the measure tool's state, the split view, its own labels over a pane and the view
/// kind of each pane). A pane without a host has no chrome keys and no menu.
/// Main thread only.
class SceneViewportHost
{
public:
    /// A press, a scroll, a key press or a drag move in `pane`: it becomes the active and the hovered pane. The pane
    /// does not call it while another pane of this host drags.
    virtual void ActivateViewport(SceneViewport& pane) = 0;
    /// The pointer enters or moves in `pane` (`inside` true), or leaves it (`inside` false).
    virtual void HoverViewport(SceneViewport& pane, bool inside) = 0;
    /// True while the pan hold key is down: a left press then pans.
    virtual bool IsPanHoldDown() const = 0;
    /// A left press panned with the pan hold key: the key's release is then not a tap.
    virtual void MarkPanHoldDragged() = 0;
    /// A key press (`pressed` true) or release in `pane`. Returns true when the host handled the key; the pane
    /// then stops the event.
    virtual bool HandleViewportKey(SceneViewport& pane, UIEvent& ev, bool pressed) = 0;
    /// A right click that did not drag, at `position` in window pixels. Returns true when a menu opened.
    virtual bool ShowViewportContextMenu(const Mathematics::Vector2& position) = 0;
    /// A press on a measure endpoint: the measure tool turns off in every pane, and `keep` takes the move tool
    /// so the endpoint's entity can be moved.
    virtual void ExitMeasureTool(SceneViewController& keep) = 0;
    /// True while the host shows its panes side by side (the quad view). A pane's own rotation gizmo setting
    /// applies only then.
    virtual bool IsSplitView() const = 0;
    /// The host's own label over one of its panes (the FPS readout) while it is shown, else null. The pane that is
    /// the label's parent keeps its hover pill and rotation gizmo clear of it (SceneViewport::GetHostLabelBox).
    virtual const UIElement* GetHostLabel() const = 0;
    /// An orbit drag starts on the rotation gizmo of `pane`: the pane's view kind becomes a free perspective view.
    virtual void NotifyRotationGizmoOrbit(SceneViewport& pane) = 0;
    /// A click on an axis of the rotation gizmo of `pane` (0 to 5: +X, +Y, +Z, -X, -Y, -Z): the pane's view kind
    /// becomes the view along that axis.
    virtual void NotifyRotationGizmoAxisSnap(SceneViewport& pane, int axis) = 0;

protected:
    ~SceneViewportHost() = default;
};

} // namespace GameEngine
