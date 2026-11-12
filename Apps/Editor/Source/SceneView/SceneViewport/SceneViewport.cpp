#include "SceneView/SceneViewport/SceneViewport.h"

#include "Core/CpuProfiler.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "EditorInputActions.h"
#include "Engine/Rendering/RenderServices.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "Mathematics/Rect.h"
#include "Mathematics/Types.h"
#include "SceneView/SceneViewport/SceneViewCameraGesture.h"
#include "SceneView/SceneViewController.h"
#include "SceneView/SceneViewEvents.h"
#include "SceneView/SceneViewRenderCoordinator.h"
#include "SceneView/SceneViewport/SceneViewHoverPill.h"
#include "SceneView/SceneViewport/SceneViewHoverPillLabels.h"
#include "SceneView/SceneViewport/SceneViewportEdits.h"
#include "SceneView/SceneViewport/SceneViewportHost.h"
#include "SceneView/SceneViewport/SceneViewportRegistry.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Widgets/SceneView/SceneViewMeasureOverlay.h"
#include "UI/Controls/Widgets/SceneView/SceneViewRulerOverlay.h"
#include "UI/Controls/Widgets/ViewportRotationGizmo.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace GameEngine
{
namespace
{
constexpr const char* kLayoutAssetPath = "UI/controls/SceneViewport/SceneViewport.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/SceneViewport/SceneViewport.css";
constexpr std::string_view kLabelId = "SceneViewViewportLabel";
constexpr std::string_view kRulerOverlayId = "SceneViewRulerOverlay";
constexpr std::string_view kMeasureOverlayId = "SceneViewMeasureOverlay";
constexpr std::string_view kHoverPillId = "SceneViewHoverPill";
constexpr std::string_view kRotationGizmoId = "ViewportRotationGizmo";
constexpr std::array<std::string_view, 6> kRequiredIds = {
    kLabelId, kRulerOverlayId, kMeasureOverlayId, kRotationGizmoId, "SceneViewHdrTestPatternOverlay", kHoverPillId};

/// The names one slot gives its pane: the id suffix of every child, the label's id suffix and the label's text.
struct SlotNames
{
    std::string_view Slot;
    std::string_view IdSuffix;
    std::string_view LabelIdSuffix;
    std::string_view LabelText;
};

constexpr std::array<SlotNames, 4> kSlotNames = {{
    {"perspective", "", "Perspective", "Persp"},
    {"top", "Top", "Top", "Top"},
    {"front", "Front", "Front", "Front"},
    {"side", "Side", "Side", "Side"},
}};

const SlotNames* FindSlotNames(std::string_view slot)
{
    for (const SlotNames& names : kSlotNames)
    {
        if (names.Slot == slot)
            return &names;
    }
    return nullptr;
}

/// The box of `element` in pixels from the pane's top-left corner; empty before the element's first layout.
Mathematics::Rect BoxInPane(const UIElement& pane, const UIElement* element)
{
    if (!element || element->GetLayoutWidth() <= 0.0f || element->GetLayoutHeight() <= 0.0f)
        return {};
    return Mathematics::Rect{element->GetLayoutX() - pane.GetLayoutX(), element->GetLayoutY() - pane.GetLayoutY(),
                             element->GetLayoutWidth(), element->GetLayoutHeight()};
}

void ApplySlotIds(UIElement& element, const SlotNames& names)
{
    const std::string& id = element.GetId();
    if (id == kLabelId)
        element.SetId(std::string(kLabelId) + std::string(names.LabelIdSuffix));
    else if (!id.empty())
        element.SetId(id + std::string(names.IdSuffix));
}

constexpr std::string_view kImageNamePrefix = "scene_viewport_";
// The index of the next pane's image name. Main thread only.
std::uint32_t s_NextImageIndex = 0;

constexpr float kMaxPitchDeg = 89.9f;
// The pointer travel in pixels a Shift + pan sums before it locks to one axis.
constexpr float kPanAxisDeadZonePx = 4.0f;

/// The camera input of one frame: the look and dolly deltas and the held move keys.
struct CameraMotion
{
    float LookYawDelta = 0.0f;
    float LookPitchDelta = 0.0f;
    float DollyDelta = 0.0f;
    bool Forward = false;
    bool Backward = false;
    bool Left = false;
    bool Right = false;
    bool Up = false;
    bool Down = false;
    bool Faster = false;
};

CameraMotion ReadCameraMotion(const Input::InputSystem& input, const SceneViewCameraGesture& gesture)
{
    CameraMotion motion;
    const Input::ActionState yawState = input.GetActionState(EditorInput::kSceneViewLookYaw);
    const Input::ActionState pitchState = input.GetActionState(EditorInput::kSceneViewLookPitch);
    const Editor::SceneViewSettings& settings = Editor::SceneViewSettings::Get();
    const float sensitivity = settings.GetLookSensitivity();
    const bool looking = gesture.IsLooking();
    if (looking || gesture.IsOrbiting())
    {
        motion.LookYawDelta = yawState.value * -sensitivity;
        motion.LookPitchDelta = pitchState.value * (settings.GetInvertY() ? -sensitivity : sensitivity);
    }
    // A dolly zooms with the horizontal mouse delta.
    if (gesture.Dollying)
        motion.DollyDelta = -yawState.value;
    if (looking)
    {
        motion.Forward = input.IsActionActive(EditorInput::kSceneViewMoveForward);
        motion.Backward = input.IsActionActive(EditorInput::kSceneViewMoveBackward);
        motion.Left = input.IsActionActive(EditorInput::kSceneViewMoveLeft);
        motion.Right = input.IsActionActive(EditorInput::kSceneViewMoveRight);
        motion.Up = input.IsActionActive(EditorInput::kSceneViewMoveUp);
        motion.Down = input.IsActionActive(EditorInput::kSceneViewMoveDown);
        motion.Faster = input.IsActionActive(EditorInput::kSceneViewMoveFaster);
    }
    return motion;
}

constexpr int kLeftButton = 0;
// Wheel units to dolly units. Positive ScrollY scrolls down, so the dolly negates it: wheel up zooms in.
constexpr float kScrollToDollyScale = 0.25f;

/// The pointer of `ev` in pixels from the pane's top-left corner.
Mathematics::Vector2 LocalPointer(const UIElement& pane, const UIEvent& ev)
{
    return Mathematics::Vector2(ev.X - pane.GetLayoutX(), ev.Y - pane.GetLayoutY());
}

/// Clamps a pane-local pointer into the pane, so a measure drag past the edge ends on the edge.
Mathematics::Vector2 ClampToPane(const UIElement& pane, const Mathematics::Vector2& local)
{
    return Mathematics::Vector2(std::clamp(local.x, 0.0f, std::max(0.0f, pane.GetLayoutWidth())),
                                std::clamp(local.y, 0.0f, std::max(0.0f, pane.GetLayoutHeight())));
}

Editor::SceneTools::ScenePointerEvent MakePointerEvent(const UIElement& pane,
                                                       SceneViewController& controller,
                                                       const Mathematics::Vector2& local,
                                                       const UIEvent& ev,
                                                       Editor::SceneTools::PointerButton button,
                                                       Editor::SceneTools::PointerPhase phase)
{
    Editor::SceneTools::ScenePointerEvent toolEv{};
    toolEv.viewX = local.x;
    toolEv.viewY = local.y;
    toolEv.viewW = pane.GetLayoutWidth();
    toolEv.viewH = pane.GetLayoutHeight();
    toolEv.ray = controller.MakeGizmoRay(local.x, local.y, toolEv.viewW, toolEv.viewH);
    controller.PopulatePointerCameraState(toolEv);
    toolEv.button = button;
    toolEv.phase = phase;
    Editor::SceneTools::PopulateScenePointerMods(toolEv, ev.Mods);
    return toolEv;
}

Editor::SceneTools::SceneKeyEvent MakeKeyEvent(const UIEvent& ev, bool pressed)
{
    Editor::SceneTools::SceneKeyEvent keyEv{};
    keyEv.keyCode = static_cast<std::uint32_t>(ev.Key);
    keyEv.pressed = pressed;
    keyEv.alt = (ev.Mods & Input::kModAlt) != 0;
    keyEv.ctrl = (ev.Mods & Input::kModControl) != 0;
    keyEv.shift = (ev.Mods & Input::kModShift) != 0;
    return keyEv;
}

/// A measure end in world units. A perspective view puts the end on the plane through the orbit pivot that
/// faces the camera; a 2D or orthographic view uses the ray origin.
Mathematics::Vector3 MeasurePointFromPointer(const SceneViewController& controller,
                                             const Editor::SceneTools::ScenePointerEvent& ev)
{
    if (controller.Is2DMode() || controller.IsOrthographic())
        return ev.ray.origin;
    const Mathematics::Vector3 planePoint = ev.cameraPos + ev.cameraForward * controller.GetCameraPose().Distance;
    float t = 0.0f;
    Mathematics::Vector3 hit;
    if (Mathematics::IntersectRayPlane(ev.ray, planePoint, ev.cameraForward, t, hit))
        return hit;
    return ev.ray.origin;
}

/// The camera keys with no modifier conflict: [ and ] step the camera history, F frames the origin and
/// Shift+F frames all. Returns true when `ev` is one of them.
bool HandleCameraKey(SceneViewController& controller, const UIEvent& ev)
{
    if (ev.Mods == 0 && ev.Key == Input::kKeyCode_LeftBracket)
    {
        controller.StepCameraHistoryBack();
        return true;
    }
    if (ev.Mods == 0 && ev.Key == Input::kKeyCode_RightBracket)
    {
        controller.StepCameraHistoryForward();
        return true;
    }
    if (ev.Key != Input::kKeyCode_F)
        return false;
    const bool onlyShift = (ev.Mods & Input::kModShift) != 0 &&
                           (ev.Mods & (Input::kModControl | Input::kModAlt | Input::kModSuper)) == 0;
    if (onlyShift)
    {
        controller.FrameAll();
        return true;
    }
    if (ev.Mods == 0)
    {
        controller.FrameOrigin();
        return true;
    }
    return false;
}

/// The pill text of the controller's hovered pick: the name, "+N" for the hovered descendants, and the position in
/// a depth cycle ("2/7") while Ctrl+scroll or a click on the same spot walks the picks. Empty with no valid pick.
std::string HoverPickText(const SceneViewController& controller)
{
    const ECS::World& world = controller.GetWorld();
    const ECS::EntityHandle hovered = controller.GetHoveredEntity();
    if (!hovered.IsValid() || !world.IsValid(hovered))
        return {};
    std::string text = Editor::EntityDisplayName(world, hovered);
    const std::size_t descendantCount = controller.GetHoveredDescendants().size();
    if (descendantCount > 0)
        text += " · +" + std::to_string(descendantCount);
    int cycleIndex = 0;
    int cycleCount = 0;
    if (controller.GetPickCycleStatus(cycleIndex, cycleCount))
        text += " · " + std::to_string(cycleIndex) + "/" + std::to_string(cycleCount);
    return text;
}
} // namespace

SceneViewport::SceneViewport()
{
    RegisterEventHandler(kEventMouseDown, [this](UIEvent& ev) { OnMouseDown(ev); });
    RegisterEventHandler(kEventMouseUp, [this](UIEvent& ev) { OnMouseUp(ev); });
    RegisterEventHandler(kEventMouseCancel, [this](UIEvent& ev) { OnMouseCancel(ev); });
    RegisterEventHandler(kEventMouseMove, [this](UIEvent& ev) { OnMouseMove(ev); });
    RegisterEventHandler(kEventMouseEnter, [this](UIEvent& ev) { OnMouseEnter(ev); });
    RegisterEventHandler(kEventMouseLeave, [this](UIEvent& ev) { OnMouseLeave(ev); });
    RegisterEventHandler(kEventScroll, [this](UIEvent& ev) { OnScroll(ev); });
    RegisterEventHandler(kEventKeyDown, [this](UIEvent& ev) { OnKeyDown(ev); });
    RegisterEventHandler(kEventKeyUp, [this](UIEvent& ev) { OnKeyUp(ev); });
}

SceneViewport::~SceneViewport()
{
    SceneViewportRegistry::Unregister(*this);
}

void SceneViewport::SetSlot(const std::string& slot)
{
    const SlotNames* names = FindSlotNames(slot);
    if (!names)
    {
        Logger::Log::Error("SceneViewport: unknown slot '{}'; use perspective, top, front or side", slot);
        return;
    }
    // A layout hot reload sets the attribute again on the live pane: the children exist once.
    if (!m_Slot.empty())
    {
        if (m_Slot != names->Slot)
            Logger::Log::Error("SceneViewport: the pane's slot is '{}' and does not change to '{}'; close and open "
                               "the panel to use the new slot",
                               m_Slot, slot);
        return;
    }
    m_Slot = names->Slot;
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    std::unique_ptr<UIElement> layout = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!layout)
        return;
    m_Label = dynamic_cast<Label*>(layout->FindById(HashStringId(kLabelId)));
    m_MeasureOverlay = dynamic_cast<SceneViewMeasureOverlay*>(layout->FindById(HashStringId(kMeasureOverlayId)));
    m_RulerOverlay = dynamic_cast<SceneViewRulerOverlay*>(layout->FindById(HashStringId(kRulerOverlayId)));
    m_HoverPill = dynamic_cast<SceneViewHoverPill*>(layout->FindById(HashStringId(kHoverPillId)));
    m_RotationGizmo = dynamic_cast<ViewportRotationGizmo*>(layout->FindById(HashStringId(kRotationGizmoId)));
    if (m_RotationGizmo)
        m_RotationGizmo->SetPane(this, static_cast<size_t>(names - kSlotNames.data()));
    if (m_Label)
        m_Label->SetText(std::string(names->LabelText));
    while (!layout->GetChildren().empty())
    {
        std::unique_ptr<UIElement> child = layout->TakeChild(layout->GetChildren().front().get());
        ApplySlotIds(*child, *names);
        AddChild(std::move(child));
    }
}

void SceneViewport::Bind(const SceneViewportBinding& binding)
{
    m_Binding = binding;
    if (binding.Controller && m_ImageName.empty())
        m_ImageName = std::string(kImageNamePrefix) + std::to_string(s_NextImageIndex++);
    if (m_RotationGizmo)
        m_RotationGizmo->SetSceneController(binding.Controller);
    if (m_RulerOverlay)
        m_RulerOverlay->SetSceneController(binding.Controller);
    if (m_MeasureOverlay)
        m_MeasureOverlay->SetSceneController(binding.Controller);
    if (binding.Controller)
        SceneViewportRegistry::Register(*this);
    else
        SceneViewportRegistry::Unregister(*this);
}

ECS::World* SceneViewport::GetWorld() const
{
    return m_Binding.Controller ? &m_Binding.Controller->GetWorld() : nullptr;
}

void SceneViewport::UpdateHoverPill()
{
    if (!m_HoverPill)
        return;
    const SceneViewController* controller = m_Binding.Controller;
    // The pill covers the measure labels, so it hides while the measure tool is on.
    if (!controller || !m_PointerInside || m_MeasureSettings.Enabled)
    {
        m_HoverPill->Hide();
        return;
    }

    // The active tool's text (a snap), during the tool's own drags too, else the text of the overlay under a
    // resting pointer (a junction marker), takes the place of the pick's name.
    const Editor::SceneTools::ISceneTool* activeTool = controller->GetToolContext().GetActiveTool();
    const std::string* toolText = activeTool ? &activeTool->GetHoverText() : nullptr;
    if ((!toolText || toolText->empty()) && !m_CameraGesture.Dragging)
        toolText = &controller->GetOverlayHoverText();
    if (toolText && !toolText->empty())
    {
        m_HoverPill->Show(*toolText, true, m_PointerLocal, HoverPillLabels());
        return;
    }

    // A pick name hides during a camera drag: a pill that follows the pointer changes the layout every frame.
    const Editor::SceneViewSettings& settings = Editor::SceneViewSettings::Get();
    if (m_CameraGesture.Dragging || !settings.GetHoverNamePillEnabled() || controller->IsHoverNamePillSuppressedBySource())
    {
        m_HoverPill->Hide();
        return;
    }
    const std::string text = HoverPickText(*controller);
    if (text.empty())
    {
        m_HoverPill->Hide();
        return;
    }
    m_HoverPill->Show(text, settings.GetHoverNamePillNearCursor(), m_PointerLocal, HoverPillLabels());
}

SceneViewHoverPillLabels SceneViewport::HoverPillLabels() const
{
    SceneViewHoverPillLabels labels;
    labels.PaneLabel = BoxInPane(*this, m_Label);
    const Mathematics::Rect hostLabel = GetHostLabelBox();
    if (!hostLabel.IsEmpty())
        labels.HostLabel = Mathematics::Rect{hostLabel.X - GetLayoutX(), hostLabel.Y - GetLayoutY(), hostLabel.Width,
                                             hostLabel.Height};
    return labels;
}

Mathematics::Rect SceneViewport::GetHostLabelBox() const
{
    const UIElement* label = m_Binding.Host ? m_Binding.Host->GetHostLabel() : nullptr;
    if (!label || label->GetParent() != this || label->GetLayoutWidth() <= 0.0f || label->GetLayoutHeight() <= 0.0f)
        return {};
    return Mathematics::Rect{label->GetLayoutX(), label->GetLayoutY(), label->GetLayoutWidth(),
                             label->GetLayoutHeight()};
}

bool SceneViewport::IsHostDragging(const SceneViewportHost& host, const SceneViewport* except)
{
    for (const SceneViewport* pane : SceneViewportRegistry::GetLive())
    {
        if (pane != except && pane->m_Binding.Host == &host && pane->m_CameraGesture.Dragging)
            return true;
    }
    return false;
}

void SceneViewport::ActivateInHost()
{
    SceneViewportHost* host = m_Binding.Host;
    if (!host || IsHostDragging(*host, this))
        return;
    host->ActivateViewport(*this);
}

void SceneViewport::ApplyLookDelta(float deltaYawDeg, float deltaPitchDeg)
{
    SceneViewController* controller = m_Binding.Controller;
    if (!controller)
        return;
    // A camera tween (a bookmark jump) owns the angles; 2D and fixed-axis views pan instead of look.
    if (controller->IsTweenActive() || controller->UsesFixedViewPanControls())
        return;
    m_CameraGesture.YawDeg += deltaYawDeg;
    // Moving the mouse up pitches the camera up.
    m_CameraGesture.PitchDeg = std::clamp(m_CameraGesture.PitchDeg - deltaPitchDeg, -kMaxPitchDeg, kMaxPitchDeg);
}

void SceneViewport::BeginCameraDragTracking(const Mathematics::Vector2& pointer)
{
    m_DragPointerValid = true;
    m_DragPointer = pointer;
}

void SceneViewport::EndCameraDragTracking()
{
    m_DragPointerValid = false;
    ClearPanAxisLock();
}

bool SceneViewport::ApplyImmediatePan(UIEvent& ev)
{
    if (!m_CameraGesture.Dragging)
    {
        EndCameraDragTracking();
        return false;
    }

    if (!m_DragPointerValid)
    {
        BeginCameraDragTracking(Mathematics::Vector2(ev.X, ev.Y));
        if (m_CameraGesture.Panning)
            ev.Stop();
        return m_CameraGesture.Panning;
    }

    const Mathematics::Vector2 pointer(ev.X, ev.Y);
    const Mathematics::Vector2 rawDelta = pointer - m_DragPointer;
    m_DragPointer = pointer;

    SceneViewController* controller = m_Binding.Controller;
    if (!m_CameraGesture.Panning || !controller)
    {
        if (!m_CameraGesture.Panning)
            ClearPanAxisLock();
        return false;
    }

    // Window pixels grow down; the pan delta grows up.
    Mathematics::Vector2 panDelta(rawDelta.x, -rawDelta.y);
    const bool shiftHeld = (ev.Mods & Input::kModShift) != 0;
    ConstrainPanDeltaToShiftAxis(shiftHeld, panDelta);

    if (panDelta.x != 0.0f || panDelta.y != 0.0f)
        controller->UpdatePan(panDelta.x, panDelta.y, GetLayoutHeight());

    ev.Stop();
    return true;
}

void SceneViewport::ClearPanAxisLock()
{
    m_PanShiftWasHeld = false;
    m_PanAxisLock = PanAxisLock::None;
    m_PanAxisSum = Mathematics::Vector2();
}

void SceneViewport::ConstrainPanDeltaToShiftAxis(bool shiftHeld, Mathematics::Vector2& delta)
{
    SceneViewController* controller = m_Binding.Controller;
    if (!shiftHeld || !controller || !(controller->Is2DMode() || controller->IsOrthographic()) ||
        !m_CameraGesture.Panning)
    {
        ClearPanAxisLock();
        return;
    }

    if (!m_PanShiftWasHeld)
    {
        m_PanAxisLock = PanAxisLock::None;
        m_PanAxisSum = Mathematics::Vector2();
    }
    m_PanShiftWasHeld = true;

    // A locked axis drops the other component of every later delta.
    if (m_PanAxisLock == PanAxisLock::Horizontal)
    {
        delta.y = 0.0f;
        return;
    }
    if (m_PanAxisLock == PanAxisLock::Vertical)
    {
        delta.x = 0.0f;
        return;
    }

    // Before the lock: the motion inside the dead zone is held back. The first exit picks the axis and
    // applies the summed motion along it.
    m_PanAxisSum = m_PanAxisSum + delta;
    if (Mathematics::Vector2::Dot(m_PanAxisSum, m_PanAxisSum) < kPanAxisDeadZonePx * kPanAxisDeadZonePx)
    {
        delta = Mathematics::Vector2();
        return;
    }

    if (std::fabs(m_PanAxisSum.x) >= std::fabs(m_PanAxisSum.y))
    {
        m_PanAxisLock = PanAxisLock::Horizontal;
        delta = Mathematics::Vector2(m_PanAxisSum.x, 0.0f);
    }
    else
    {
        m_PanAxisLock = PanAxisLock::Vertical;
        delta = Mathematics::Vector2(0.0f, m_PanAxisSum.y);
    }
    m_PanAxisSum = Mathematics::Vector2();
}

bool SceneViewport::Tick(float deltaSeconds, Input::InputSystem* input, bool consumeInput)
{
    SceneViewController* controller = m_Binding.Controller;
    if (!controller || !m_DrivesCamera)
        return false;
    GE_CPU_PROFILE_SCOPE("SceneViewport.Tick");

    SceneViewCameraGesture& gesture = m_CameraGesture;
    const bool interacting = gesture.IsInteracting();
    const bool orbiting = gesture.IsOrbiting();
    CameraMotion motion;
    if (interacting && input && consumeInput)
        motion = ReadCameraMotion(*input, gesture);

    // The first orbit frame skips the look delta, so the orbit pivot does not drift.
    const bool firstOrbitFrame = gesture.ConsumeFirstOrbitFrame();
    if ((gesture.IsLooking() || orbiting) && (motion.LookYawDelta != 0.0f || motion.LookPitchDelta != 0.0f) &&
        !firstOrbitFrame)
        ApplyLookDelta(motion.LookYawDelta, motion.LookPitchDelta);

    const float speedMultiplier = motion.Faster ? Editor::SceneViewSettings::Get().GetFastMoveMultiplier() : 1.0f;

    // The size is set before the camera update, so frustum culling uses this frame's layout size.
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    Editor::SceneViewRenderCoordinator::TryResolveRenderableViewportSize(GetOwnerManager(), this, width, height);
    controller->SetLastViewportSize(width, height);

    // The angles, the orbit and the dolly settle before any view-projection is built, so the world pass and
    // the grid see the same camera.
    controller->SetCameraAnglesDeg(gesture.YawDeg, gesture.PitchDeg);
    controller->UpdateOrbit(gesture.IsOrbiting());
    if (gesture.Dollying && motion.DollyDelta != 0.0f)
        controller->UpdateDolly(motion.DollyDelta);
    controller->Update(deltaSeconds, motion.Forward, motion.Backward, motion.Left, motion.Right, motion.Up,
                       motion.Down, speedMultiplier);
    controller->UpdateCamera(deltaSeconds, motion.Forward, motion.Backward, motion.Left, motion.Right, motion.Up,
                             motion.Down, speedMultiplier);
    if (Engine::Renderer::RenderServices* renderServices = controller->GetRenderServices())
        renderServices->Views().SetViewpointCamera(controller->GetCameraId());

    // A tween moves the controller's angles; the gesture follows them.
    const auto pose = controller->GetCameraPose();
    gesture.YawDeg = pose.YawDeg;
    gesture.PitchDeg = pose.PitchDeg;
    return interacting;
}

void SceneViewport::SetMeasureSettings(const SceneViewportMeasureSettings& settings)
{
    // The host pushes the settings every frame: only a change of the on and off state cancels a measure.
    const bool turnedOff = m_MeasureSettings.Enabled && !settings.Enabled;
    m_MeasureSettings = settings;
    if (!settings.Enabled)
        m_MeasureDragActive = false;
    if (!m_MeasureOverlay)
        return;
    m_MeasureOverlay->SetMeasureToolEnabled(settings.Enabled);
    m_MeasureOverlay->SetUnitSystem(settings.Units);
    m_MeasureOverlay->SetTwoDMode(settings.TwoDMode);
    if (turnedOff)
        m_MeasureOverlay->CancelMeasure();
}

void SceneViewport::FinishMeasureDragIfReleased()
{
    const bool leftDragging = m_CameraGesture.Dragging && m_CameraGesture.DragButton == kLeftButton;
    if (m_MeasureDragActive && !leftDragging)
        FinishMeasureDrag(true);
}

void SceneViewport::OnMouseDown(UIEvent& ev)
{
    // A pane with no controller has nothing to drive: no gesture and no capture.
    SceneViewController* controller = m_Binding.Controller;
    if (!controller)
        return;
    Activate();
    SceneViewportHost* host = m_Binding.Host;
    const bool panHold = host && host->IsPanHoldDown();
    m_CameraGesture.BeginPress(ev.Button, Mathematics::Vector2(ev.X, ev.Y), (ev.Mods & Input::kModAlt) != 0,
                               Input::IsPrimaryShortcutModifier(ev.Mods), controller->UsesFixedViewPanControls(),
                               panHold);
    if (panHold && ev.Button == kLeftButton)
        host->MarkPanHoldDragged();
    BeginCameraDragTracking(Mathematics::Vector2(ev.X, ev.Y));
    ev.Capture(this);

    if (!m_CameraGesture.ToolDrag)
        return;
    if (HandleMeasurePointerDown(*controller, ev))
        return;
    if (m_MeasureSettings.Enabled)
    {
        ev.Stop();
        return;
    }
    using namespace Editor::SceneTools;
    controller->GetToolContext().HandlePointerEvent(
        MakePointerEvent(*this, *controller, LocalPointer(*this, ev), ev, PointerButton::Left, PointerPhase::Down));
}

void SceneViewport::OnMouseUp(UIEvent& ev)
{
    // A release never changes the active pane: the press did.
    const bool menuClick = m_CameraGesture.EndPress(ev.Button);
    if (!m_CameraGesture.Dragging)
        EndCameraDragTracking();
    if (menuClick && m_Binding.Host && m_Binding.Host->ShowViewportContextMenu(Mathematics::Vector2(ev.X, ev.Y)))
    {
        ev.Stop();
        return;
    }

    SceneViewController* controller = m_Binding.Controller;
    if (!controller || ev.Button != kLeftButton)
        return;
    if (HandleMeasurePointerUp(*controller, ev))
        return;
    if (m_MeasureSettings.Enabled)
    {
        ev.Stop();
        return;
    }
    using namespace Editor::SceneTools;
    controller->GetToolContext().HandlePointerEvent(
        MakePointerEvent(*this, *controller, LocalPointer(*this, ev), ev, PointerButton::Left, PointerPhase::Up));
}

void SceneViewport::OnMouseCancel(UIEvent& ev)
{
    // The manager ended the press without a release. The drag ends as on a release, and
    // FinishMeasureDragIfReleased then ends a measure drag.
    m_CameraGesture.EndPress(ev.Button);
    if (!m_CameraGesture.Dragging)
        EndCameraDragTracking();
}

void SceneViewport::OnMouseMove(UIEvent& ev)
{
    const Mathematics::Vector2 local = LocalPointer(*this, ev);
    // A captured move can be outside the pane.
    m_PointerInside = ContainsPoint(ev.X, ev.Y);
    if (m_PointerInside)
        m_PointerLocal = local;
    SceneViewportHost* host = m_Binding.Host;
    if (host && m_CameraGesture.Dragging)
        ActivateInHost();
    else if (host)
        host->HoverViewport(*this, true);

    SceneViewController* controller = m_Binding.Controller;
    if (!controller)
        return;
    if (m_RulerOverlay)
        m_RulerOverlay->SetCursor(local.x, local.y, true);

    m_CameraGesture.TrackPointer(Mathematics::Vector2(ev.X, ev.Y));
    // Alt and the primary modifier can change after the press: the camera mode follows them.
    m_CameraGesture.UpdateModifiers((ev.Mods & Input::kModAlt) != 0, Input::IsPrimaryShortcutModifier(ev.Mods),
                                    controller->UsesFixedViewPanControls(), host && host->IsPanHoldDown());

    if (HandleMeasurePointerMove(*controller, ev))
        return;
    if (ApplyImmediatePan(ev))
        return;
    if (m_MeasureSettings.Enabled)
        return;

    using namespace Editor::SceneTools;
    if (!m_CameraGesture.Dragging)
    {
        const ScenePointerEvent hoverEv =
            MakePointerEvent(*this, *controller, local, ev, PointerButton::None, PointerPhase::Move);
        controller->HandleHoverDetection(hoverEv);
        controller->GetToolContext().HandlePointerEvent(hoverEv);
        return;
    }
    if (m_CameraGesture.DragButton != kLeftButton)
        return;
    GE_CPU_PROFILE_SCOPE("SceneViewport.DragMouseMove");
    controller->GetToolContext().HandlePointerEvent(
        MakePointerEvent(*this, *controller, local, ev, PointerButton::Left, PointerPhase::Move));
}

void SceneViewport::OnMouseEnter(UIEvent&)
{
    if (m_Binding.Host)
        m_Binding.Host->HoverViewport(*this, true);
}

void SceneViewport::OnMouseLeave(UIEvent&)
{
    // A measure drag lives on past the edge: the captured release ends it.
    m_PointerInside = false;
    if (m_Binding.Host)
        m_Binding.Host->HoverViewport(*this, false);
    if (m_RulerOverlay)
        m_RulerOverlay->SetCursor(0.0f, 0.0f, false);
}

void SceneViewport::OnScroll(UIEvent& ev)
{
    ActivateInHost();
    SceneViewController* controller = m_Binding.Controller;
    const float dy = ev.ScrollY;
    if (!controller || dy == 0.0f)
        return;

    const Mathematics::Vector2 local = LocalPointer(*this, ev);
    const float viewW = GetLayoutWidth();
    const float viewH = GetLayoutHeight();
    // Ctrl/Cmd + wheel cycles the pick candidates under the pointer.
    if ((ev.Mods & (Input::kModControl | Input::kModSuper)) != 0)
    {
        controller->CycleEntityUnderCursor(local.x, local.y, viewW, viewH, dy > 0.0f ? 1 : -1);
        ev.Stop();
        return;
    }

    const Editor::SceneViewSettings& settings = Editor::SceneViewSettings::Get();
    if (!settings.GetScrollWheelDollyEnabled())
        return;
    const bool reversed = settings.GetScrollWheelDollyReversed();
    // Pixel-perfect 2D: one discrete scale step per wheel event, the world point under the pointer anchored.
    // 2D zooms opposite to 3D: a wheel toward the content zooms out.
    if (controller->Is2DMode() && settings.GetPixelPerfect2D())
    {
        const int step = (dy > 0.0f) != reversed ? 1 : -1;
        controller->ZoomPixelPerfectAtCursor(step, local.x, local.y, viewW, viewH);
        ev.Stop();
        return;
    }
    if (controller->IsOrthographic() && !controller->Is2DMode())
    {
        controller->ZoomOrthographicStep((dy > 0.0f) != reversed ? 1 : -1);
        ev.Stop();
        return;
    }

    float deltaY = -dy * kScrollToDollyScale;
    if (reversed)
        deltaY = -deltaY;
    if (controller->Is2DMode())
        deltaY = -deltaY;
    controller->UpdateDolly(deltaY);
    // Stopped, so a parent scroll view does not scroll too.
    ev.Stop();
}

void SceneViewport::OnKeyDown(UIEvent& ev)
{
    SceneViewportHost* host = m_Binding.Host;
    if (host)
    {
        ActivateInHost();
        if (host->HandleViewportKey(*this, ev, true))
        {
            ev.Stop();
            return;
        }
    }
    SceneViewController* controller = m_Binding.Controller;
    if (!controller)
        return;
    // A camera gesture holds WASD and Shift for the camera: the camera keys wait for its end.
    if (!m_CameraGesture.IsInteracting() && HandleCameraKey(*controller, ev))
    {
        ev.Stop();
        return;
    }
    controller->GetToolContext().HandleKeyEvent(MakeKeyEvent(ev, true));
}

void SceneViewport::OnKeyUp(UIEvent& ev)
{
    // A release never changes the active pane.
    if (m_Binding.Host && m_Binding.Host->HandleViewportKey(*this, ev, false))
    {
        ev.Stop();
        return;
    }
    if (SceneViewController* controller = m_Binding.Controller)
        controller->GetToolContext().HandleKeyEvent(MakeKeyEvent(ev, false));
}

void SceneViewport::TickOverlays()
{
    if (m_RotationGizmo)
        m_RotationGizmo->Tick();
    if (m_RulerOverlay)
        m_RulerOverlay->Tick();
    if (m_MeasureOverlay)
        m_MeasureOverlay->Tick();
}

const UIElement* SceneViewport::GetRotationGizmo() const
{
    return m_RotationGizmo;
}

void SceneViewport::Activate()
{
    ActivateInHost();
    // The editor's input actions drive the camera keys (WASD). A focused pane would take those keys in the
    // UI manager first. An explicit empty focus also stops the press from focusing the pane.
    if (UIManager* ui = GetOwnerManager())
        ui->SetFocusById("");
}

bool SceneViewport::HandleMeasurePointerDown(SceneViewController& controller, UIEvent& ev)
{
    if (!m_MeasureSettings.Enabled || !m_MeasureOverlay || ev.Button != kLeftButton)
        return false;

    using namespace Editor::SceneTools;
    const Mathematics::Vector2 local = ClampToPane(*this, LocalPointer(*this, ev));
    const ScenePointerEvent toolEv =
        MakePointerEvent(*this, controller, local, ev, PointerButton::Left, PointerPhase::Down);
    // A press on a measure end leaves the measure tool, so the move tool can drag the end's entity.
    if (controller.HasMeasureEndpointAtPointer(toolEv))
    {
        m_MeasureDragActive = false;
        if (m_Binding.Host)
            m_Binding.Host->ExitMeasureTool(controller);
        controller.GetToolContext().HandlePointerEvent(toolEv);
        ev.Stop();
        return true;
    }

    m_MeasureDragActive = true;
    m_MeasureDragIs2D = controller.Is2DMode();
    m_MeasureDragStart = MeasurePointFromPointer(controller, toolEv);
    m_MeasureDragEnd = m_MeasureDragStart;
    m_MeasureOverlay->BeginMeasure(local.x, local.y, m_MeasureDragStart, m_MeasureDragIs2D, toolEv.shift);
    ev.Stop();
    return true;
}

bool SceneViewport::HandleMeasurePointerMove(SceneViewController& controller, UIEvent& ev)
{
    if (!m_MeasureSettings.Enabled || !m_MeasureDragActive || !m_MeasureOverlay)
        return false;

    using namespace Editor::SceneTools;
    const Mathematics::Vector2 local = ClampToPane(*this, LocalPointer(*this, ev));
    const ScenePointerEvent toolEv =
        MakePointerEvent(*this, controller, local, ev, PointerButton::Left, PointerPhase::Move);
    m_MeasureDragEnd = MeasurePointFromPointer(controller, toolEv);
    m_MeasureOverlay->UpdateMeasure(local.x, local.y, m_MeasureDragEnd, toolEv.shift);
    ev.Stop();
    return true;
}

bool SceneViewport::HandleMeasurePointerUp(SceneViewController& controller, UIEvent& ev)
{
    if (!m_MeasureSettings.Enabled || !m_MeasureDragActive || ev.Button != kLeftButton)
        return false;

    using namespace Editor::SceneTools;
    const Mathematics::Vector2 local = ClampToPane(*this, LocalPointer(*this, ev));
    const ScenePointerEvent toolEv =
        MakePointerEvent(*this, controller, local, ev, PointerButton::Left, PointerPhase::Up);
    m_MeasureDragEnd = MeasurePointFromPointer(controller, toolEv);
    if (m_MeasureOverlay)
        m_MeasureOverlay->UpdateMeasure(local.x, local.y, m_MeasureDragEnd, toolEv.shift);
    FinishMeasureDrag(true);
    ev.Stop();
    return true;
}

void SceneViewport::FinishMeasureDrag(bool createEntity)
{
    if (!m_MeasureDragActive)
        return;
    m_MeasureDragActive = false;
    if (m_MeasureOverlay)
        m_MeasureOverlay->EndMeasure();
    ECS::World* world = GetWorld();
    if (!createEntity || !m_MeasureSettings.CreateEntities || !world)
        return;
    SceneViewportEdits edits(*world, m_Binding);
    const ECS::EntityHandle entity = edits.CreateMeasure(m_MeasureDragStart, m_MeasureDragEnd, m_MeasureDragIs2D,
                                                         Editor::SceneViewSettings::Get().GetMeasureColor());
    if (entity.IsValid())
        m_Binding.Controller->OnEntityPicked(entity);
}

} // namespace GameEngine

namespace RegisterWidgets
{
static auto s_reg_sceneViewport =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::SceneViewport>(
        "SceneViewport", []() { return std::make_unique<GameEngine::SceneViewport>(); })
        .Attr("slot", &GameEngine::SceneViewport::SetSlot)
        .TagAlias("sceneviewport");
} // namespace RegisterWidgets
