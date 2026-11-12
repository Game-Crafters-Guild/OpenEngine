#pragma once

#include "SceneView/SceneViewport/SceneViewCameraGesture.h"
#include "SceneView/SceneViewport/SceneViewportBinding.h"
#include "SceneView/SceneViewport/SceneViewportMeasureSettings.h"

#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "UI/UIElement.h"

#include <cstdint>
#include <string>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Input
{
class InputSystem;
}

namespace GameEngine
{
class Label;
class SceneViewHoverPill;
class SceneViewMeasureOverlay;
class SceneViewRulerOverlay;
class ViewportRotationGizmo;
struct SceneViewHoverPillLabels;

/// One Scene View pane, declared as `<SceneViewport slot="top" />`: the image of its bound controller's view
/// with its overlays. Edits go through the binding, so a pane on a second world edits that world.
/// The pane takes its own pointer and key input and acts on the bound controller; the binding's host gets
/// the actions that span panes (SceneViewportHost). A bound pane is in the live list (SceneViewportRegistry).
/// Main thread only.
class SceneViewport final : public UIElement
{
public:
    SceneViewport();
    ~SceneViewport() override;

    /// Builds the children from the control's layout. The slot is one of "perspective", "top", "front" and "side":
    /// the perspective pane keeps the base ids ("SceneViewRulerOverlay"), the others add the slot ("SceneViewRulerOverlayTop").
    /// The first slot is final: a later call (a layout hot reload) builds nothing, and a different slot logs an error.
    void SetSlot(const std::string& slot);

    /// Sets the controller, the undo service, the change notifications and the dirty callback the pane edits through.
    /// The rotation gizmo, the ruler and the measure overlay read the controller. A pane with a controller joins the
    /// live list.
    void Bind(const SceneViewportBinding& binding);
    const SceneViewportBinding& GetBinding() const { return m_Binding; }
    /// The bound controller's world, or null while the pane has no controller.
    ECS::World* GetWorld() const;
    /// The external image name of the pane's view, unique per pane, set on the first bind with a controller.
    /// The render coordinator publishes a controller outside the window's own Scene View set under this name;
    /// the window's main and quad controllers keep their fixed names ("scene_main" in the CSS).
    const std::string& GetImageName() const { return m_ImageName; }

    /// Sets the measure tool's settings. Turning the tool off ends a measure drag without an entity.
    void SetMeasureSettings(const SceneViewportMeasureSettings& settings);
    /// Ends a measure drag, with its entity, when the left press ended without a release in the pane (a
    /// cancel). The host calls it once per frame.
    void FinishMeasureDragIfReleased();
    /// Updates the rotation gizmo, the ruler and the measure overlay to the bound controller's camera and mode.
    /// The host calls it once per frame for each pane it shows.
    void TickOverlays();
    /// The pane's rotation gizmo, or null when the layout has none.
    const UIElement* GetRotationGizmo() const;
    /// Makes this pane the host's active pane and clears the keyboard focus, so the camera keys go to the
    /// editor's input actions. A press in the pane or on its rotation gizmo calls it. While another pane of the
    /// same host drags, the pane does not become active; the focus is cleared all the same.
    void Activate();
    /// The box of the host's shown label (SceneViewportHost::GetHostLabel) in window layout pixels when this pane
    /// is its parent. Empty when the host shows no label, the label is over another pane, or there is no host.
    Mathematics::Rect GetHostLabelBox() const;
    /// True while a live pane of `host` other than `except` drags its camera gesture. `except` may be null.
    static bool IsHostDragging(const SceneViewportHost& host, const SceneViewport* except);

    /// True while the pointer is inside the pane. A captured drag that leaves the pane reads false; a consumer
    /// that needs the drag reads the camera gesture.
    bool IsPointerInside() const { return m_PointerInside; }
    /// The last pointer position inside the pane, in pixels from the pane's top-left corner.
    const Mathematics::Vector2& GetPointerLocalPosition() const { return m_PointerLocal; }
    /// Shows the hover pill beside the pointer while the pointer is inside this pane: the active tool's text,
    /// else the name of the bound controller's hovered pick, clear of the pane's label and the host's label over
    /// the pane. Hides it otherwise, during a camera drag that shows no tool text, and while the measure tool is on.
    /// The host calls it once per frame.
    void UpdateHoverPill();

    /// The pane's camera gesture: the pointer handlers write it, Tick reads it.
    SceneViewCameraGesture& GetCameraGesture() { return m_CameraGesture; }
    const SceneViewCameraGesture& GetCameraGesture() const { return m_CameraGesture; }
    /// Sets whether Tick drives the bound controller's camera. The host sets it for its active pane only.
    void SetDrivesCamera(bool drives) { m_DrivesCamera = drives; }
    /// Adds a look delta in degrees to the gesture. No effect during a camera tween or in a fixed-axis view.
    void ApplyLookDelta(float deltaYawDeg, float deltaPitchDeg);

    /// Starts the pointer tracking of a camera drag at the pointer position in window pixels.
    void BeginCameraDragTracking(const Mathematics::Vector2& pointer);
    /// Ends the pointer tracking of a camera drag and clears the 2D axis lock.
    void EndCameraDragTracking();
    /// Pans the bound controller by the pointer delta of a pan drag, at once in the event and not in Tick.
    /// Shift locks a 2D or orthographic pan to the first axis it leaves a dead zone on.
    /// Returns true and stops the event when the drag pans.
    bool ApplyImmediatePan(UIEvent& ev);

    /// Drives the bound controller's camera for one frame from the gesture; reads the frame's input only when
    /// `consumeInput` is true. Returns true while the gesture moves the camera; no effect on a pane that does
    /// not drive its camera.
    bool Tick(float deltaSeconds, Input::InputSystem* input, bool consumeInput);

private:
    void OnMouseDown(UIEvent& ev);
    void OnMouseUp(UIEvent& ev);
    void OnMouseCancel(UIEvent& ev);
    void OnMouseMove(UIEvent& ev);
    void OnMouseEnter(UIEvent& ev);
    void OnMouseLeave(UIEvent& ev);
    void OnScroll(UIEvent& ev);
    void OnKeyDown(UIEvent& ev);
    void OnKeyUp(UIEvent& ev);
    SceneViewHoverPillLabels HoverPillLabels() const;
    // Calls the host's ActivateViewport unless another pane of the same host drags: a drag captures the pointer in
    // its pane, so the active slot stays with that pane until the drag ends.
    void ActivateInHost();
    bool HandleMeasurePointerDown(SceneViewController& controller, UIEvent& ev);
    bool HandleMeasurePointerMove(SceneViewController& controller, UIEvent& ev);
    bool HandleMeasurePointerUp(SceneViewController& controller, UIEvent& ev);
    void FinishMeasureDrag(bool createEntity);

    enum class PanAxisLock : std::uint8_t
    {
        None,
        Horizontal,
        Vertical,
    };

    void ConstrainPanDeltaToShiftAxis(bool shiftHeld, Mathematics::Vector2& delta);
    void ClearPanAxisLock();

    SceneViewportBinding m_Binding;
    // The slot the children were built for; empty until the first SetSlot.
    std::string m_Slot;
    std::string m_ImageName;
    SceneViewCameraGesture m_CameraGesture;
    bool m_DrivesCamera = false;
    // The last pointer position of a camera drag, in window pixels.
    bool m_DragPointerValid = false;
    Mathematics::Vector2 m_DragPointer;
    // Shift + pan in a 2D or orthographic view: the axis lock and the motion summed inside the dead zone.
    PanAxisLock m_PanAxisLock = PanAxisLock::None;
    Mathematics::Vector2 m_PanAxisSum;
    bool m_PanShiftWasHeld = false;
    // The pointer inside the pane, in pixels from the pane's top-left corner.
    bool m_PointerInside = false;
    Mathematics::Vector2 m_PointerLocal;
    // The measure drag: its ends in world units.
    SceneViewportMeasureSettings m_MeasureSettings;
    bool m_MeasureDragActive = false;
    bool m_MeasureDragIs2D = false;
    Mathematics::Vector3 m_MeasureDragStart;
    Mathematics::Vector3 m_MeasureDragEnd;
    Label* m_Label = nullptr; // not owned
    SceneViewHoverPill* m_HoverPill = nullptr; // not owned
    SceneViewMeasureOverlay* m_MeasureOverlay = nullptr; // not owned
    SceneViewRulerOverlay* m_RulerOverlay = nullptr; // not owned
    ViewportRotationGizmo* m_RotationGizmo = nullptr; // not owned
};

} // namespace GameEngine
