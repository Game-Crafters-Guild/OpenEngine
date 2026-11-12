#include "SceneView/SceneViewPanel.h"
#include "SceneView/SceneViewController.h"
#include "SceneView/SceneViewEvents.h"
#include "SceneView/SceneViewport/SceneViewport.h"
#include "UI/UIEvents.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Button.h"
#include "UI/EditorIcons.h"
#include "UI/StyleProperties.h"
#include "EditorContext.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Platform/ContextMenu.h"
#include "Platform/Window.h"
#include "Scripting/EditorScriptMenuRegistry.h"
#include "UI/Controls/SceneViewToolbar.h"
#include "SceneView/TransformTool.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Core/CpuProfiler.h"
#include "Core/Engine.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{
namespace
{
constexpr StringId kSharedViewportOverlayIds[] = {
    "SceneToolOverlay"_sid,
    "SceneViewFpsLabel"_sid,
    "SceneAssetPreview"_sid,
};
} // namespace

void SceneViewPanel::SetSceneControllerForSlot(ViewportSlot slot, SceneViewController* controller)
{
    const size_t index = static_cast<size_t>(slot);
    if (index >= m_SceneControllers.size())
        return;
    const bool controllerChanged = m_SceneControllers[index] != controller;
    m_SceneControllers[index] = controller;
    if (controllerChanged && controller)
        ApplyQuadViewKind(FindSlotForQuadViewKind(static_cast<QuadViewKind>(index)));

    // Panel yaw/pitch are pushed into SceneViewController every frame via SetCameraAnglesDeg.
    // When the binding changes, copy the controller pose so we do not overwrite seeded defaults
    // (e.g. Main Camera alignment) with the panel's initial 0,0.
    if (GetQuadViewKind(m_ActiveViewportSlot) == static_cast<QuadViewKind>(index) &&
        controller != nullptr && m_Controller != controller)
    {
        const SceneViewCameraPose pose = controller->GetCameraPose();
        SetYawPitch(pose.YawDeg, pose.PitchDeg);
    }

    if (GetQuadViewKind(m_ActiveViewportSlot) == static_cast<QuadViewKind>(index) || !m_Controller)
    {
        m_Controller = GetSceneControllerForSlot(m_ActiveViewportSlot);
        if (m_Toolbar)
        {
            m_Toolbar->SetSceneController(m_Controller);
        }
    }
    BindViewports();
}

SceneViewController* SceneViewPanel::GetSceneControllerForSlot(ViewportSlot slot) const
{
    return GetSceneControllerForQuadViewKind(GetQuadViewKind(slot));
}

SceneViewController* SceneViewPanel::GetSceneControllerForQuadViewKind(QuadViewKind kind) const
{
    const size_t index = static_cast<size_t>(kind);
    if (index >= m_SceneControllers.size())
        return nullptr;
    return m_SceneControllers[index];
}

SceneViewPanel::ViewportSlot SceneViewPanel::FindSlotForQuadViewKind(QuadViewKind kind) const
{
    for (size_t i = 0; i < m_QuadViewKinds.size(); ++i)
    {
        if (m_QuadViewKinds[i] == kind)
            return static_cast<ViewportSlot>(i);
    }
    return static_cast<ViewportSlot>(std::min(static_cast<size_t>(kind),
                                              static_cast<size_t>(ViewportSlot::Count) - 1));
}

bool SceneViewPanel::ToggleActiveViewport2DMode()
{
    SceneViewController* controller = GetSceneControllerForSlot(m_ActiveViewportSlot);
    if (!controller)
        return false;

    if (m_Controller != controller)
    {
        m_Controller = controller;
        const SceneViewCameraPose pose = controller->GetCameraPose();
        SetYawPitch(pose.YawDeg, pose.PitchDeg);
        if (m_Toolbar)
            m_Toolbar->SetSceneController(controller);
    }

    controller->Toggle2DMode();
    if (SceneViewport* pane = BoundViewport(m_ActiveViewportSlot))
        pane->TickOverlays();
    if (m_Toolbar)
        m_Toolbar->UpdateView2DButtonState();

    return controller->Is2DTarget();
}

UIElement* SceneViewPanel::GetViewportElementForSlot(ViewportSlot slot) const
{
    const size_t index = static_cast<size_t>(slot);
    if (index >= m_ViewportSlots.size())
        return nullptr;
    return m_ViewportSlots[index];
}

SceneViewController* SceneViewPanel::GetSceneControllerForViewport(const UIElement& viewport) const
{
    for (size_t i = 0; i < m_ViewportSlots.size(); ++i)
    {
        if (m_ViewportSlots[i] == &viewport)
            return GetSceneControllerForSlot(static_cast<ViewportSlot>(i));
    }
    return nullptr;
}

void SceneViewPanel::SetActiveViewportSlot(ViewportSlot slot)
{
    const size_t index = static_cast<size_t>(slot);
    if (index >= m_ViewportSlots.size())
        return;
    SceneViewController* controller = GetSceneControllerForSlot(slot);
    if (slot == m_ActiveViewportSlot && m_Controller == controller)
        return;
    m_ActiveViewportSlot = slot;
    if (controller)
    {
        m_Controller = controller;
        const SceneViewCameraPose pose = controller->GetCameraPose();
        SetYawPitch(pose.YawDeg, pose.PitchDeg);
        if (m_Toolbar)
            m_Toolbar->SetSceneController(controller);
    }
    ApplyQuadViewLayout();
    BindViewports();
}

void SceneViewPanel::SetHoveredViewportSlot(ViewportSlot slot)
{
    const size_t index = static_cast<size_t>(slot);
    if (index >= m_ViewportSlots.size())
        return;
    m_HoveredViewportSlot = slot;
    m_HasHoveredViewportSlot = true;
}

SceneViewPanel::ViewportSlot SceneViewPanel::ResolveSpaceToggleViewportSlot(ViewportSlot fallback) const
{
    if (m_HasHoveredViewportSlot)
    {
        const size_t hoveredIndex = static_cast<size_t>(m_HoveredViewportSlot);
        if (hoveredIndex < m_ViewportSlots.size())
        {
            if (UIElement* hovered = m_ViewportSlots[hoveredIndex])
            {
                if (!hovered->HasClass("hidden") && !hovered->HasClass("quad-collapsed"))
                    return m_HoveredViewportSlot;
            }
        }
    }
    return fallback;
}

void SceneViewPanel::BeginViewportSpaceToggle(ViewportSlot fallback)
{
    const ViewportSlot slot = ResolveSpaceToggleViewportSlot(fallback);
    if (!SceneViewport::IsHostDragging(*this, nullptr))
        SetActiveViewportSlot(slot);
    m_SpaceHeld = true;

    if (!m_SpacePressedForQuadToggle)
    {
        m_SpacePressedForQuadToggle = true;
        m_SpacePanDragStarted = false;
    }
}

void SceneViewPanel::EndViewportSpaceToggle()
{
    // The release never changes the active pane, and it is no tap while a pane drags.
    if (m_SpacePressedForQuadToggle && !m_SpacePanDragStarted && !SceneViewport::IsHostDragging(*this, nullptr))
        ToggleQuadView();
    m_SpaceHeld = false;
    m_SpacePressedForQuadToggle = false;
    m_SpacePanDragStarted = false;
}

void SceneViewPanel::ToggleQuadView()
{
    m_QuadViewEnabled = !m_QuadViewEnabled;
    ApplyQuadViewLayout();
    UpdateQuadViewButtonState();
}

void SceneViewPanel::ResetQuadViewSplits()
{
    m_QuadViewEnabled = true;
    m_QuadSplitTopX = 0.5f;
    m_QuadSplitBottomX = 0.5f;
    m_QuadSplitY = 0.5f;

    ApplyQuadViewLayout();
    UpdateQuadViewButtonState();

    if (UIElement* host = FindById("SceneViewViewportHost"_sid))
    {
        host->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        host->RequestRelayout();
    }
}

const char* SceneViewPanel::GetQuadViewKindLabel(QuadViewKind kind)
{
    switch (kind)
    {
    case QuadViewKind::Perspective: return "Persp";
    case QuadViewKind::Top:         return "Top Y";
    case QuadViewKind::Front:       return "Front Z";
    case QuadViewKind::Side:        return "Side X";
    case QuadViewKind::Count:       break;
    }
    return "View";
}

const char* SceneViewPanel::GetQuadViewKindMenuLabel(QuadViewKind kind)
{
    switch (kind)
    {
    case QuadViewKind::Perspective: return "Perspective";
    case QuadViewKind::Top:         return "Top Y";
    case QuadViewKind::Front:       return "Front Z";
    case QuadViewKind::Side:        return "Side X";
    case QuadViewKind::Count:       break;
    }
    return "View";
}

const char* SceneViewPanel::GetViewportSlotMenuLabel(ViewportSlot slot)
{
    switch (slot)
    {
    case ViewportSlot::Top:         return "Top Left";
    case ViewportSlot::Front:       return "Top Right";
    case ViewportSlot::Side:        return "Bottom Left";
    case ViewportSlot::Perspective: return "Bottom Right";
    case ViewportSlot::Count:       break;
    }
    return "Split";
}

void SceneViewPanel::GetQuadCorner(ViewportSlot slot, bool& leftColumn, bool& topRow)
{
    switch (slot)
    {
    case ViewportSlot::Top:
        leftColumn = true;
        topRow = true;
        return;
    case ViewportSlot::Front:
        leftColumn = false;
        topRow = true;
        return;
    case ViewportSlot::Side:
        leftColumn = true;
        topRow = false;
        return;
    case ViewportSlot::Perspective:
    case ViewportSlot::Count:
        break;
    }
    leftColumn = false;
    topRow = false;
}

bool SceneViewPanel::IsQuadViewKindAllowedForSlot(ViewportSlot /*slot*/, QuadViewKind kind)
{
    return kind != QuadViewKind::Count;
}

SceneViewPanel::QuadViewKind SceneViewPanel::GetQuadViewKind(ViewportSlot slot) const
{
    const size_t index = static_cast<size_t>(slot);
    if (index >= m_QuadViewKinds.size())
        return QuadViewKind::Perspective;
    return m_QuadViewKinds[index];
}

void SceneViewPanel::ApplyQuadViewKind(ViewportSlot slot)
{
    const size_t index = static_cast<size_t>(slot);
    if (index >= m_QuadViewKinds.size())
        return;

    SceneViewController* controller = GetSceneControllerForSlot(slot);
    if (!controller)
    {
        UpdateQuadViewLabel(slot);
        return;
    }

    const QuadViewKind kind = GetQuadViewKind(slot);
    using FixedView = SceneViewController::FixedViewOrientation;
    switch (kind)
    {
    case QuadViewKind::Perspective:
    {
        controller->SetFixedViewOrientation(FixedView::Free);
        controller->Set2DMode(false);
        controller->SetOrthographic(false);
        break;
    }
    case QuadViewKind::Top:
        controller->SetFixedViewOrientation(FixedView::Top);
        break;
    case QuadViewKind::Front:
        controller->SetFixedViewOrientation(FixedView::Front);
        break;
    case QuadViewKind::Side:
        controller->SetFixedViewOrientation(FixedView::Side);
        break;
    case QuadViewKind::Count:
        break;
    }

    UpdateQuadViewLabel(slot);

    if (slot == m_ActiveViewportSlot)
    {
        m_Controller = controller;
        const SceneViewCameraPose pose = controller->GetCameraPose();
        SetYawPitch(pose.YawDeg, pose.PitchDeg);
        if (m_Toolbar)
            m_Toolbar->SetSceneController(controller);
    }
    BindViewports();
}

void SceneViewPanel::UpdateQuadViewLabel(ViewportSlot slot)
{
    const size_t index = static_cast<size_t>(slot);
    if (index >= m_ViewportLabels.size())
        return;
    if (Label* label = m_ViewportLabels[index])
        label->SetText(GetQuadViewKindLabel(GetQuadViewKind(slot)));
}

void SceneViewPanel::NotifyRotationGizmoOrbit(SceneViewport& pane)
{
    const std::optional<ViewportSlot> paneSlot = FindViewportSlot(pane);
    if (!paneSlot)
        return;
    const ViewportSlot slot = *paneSlot;
    const size_t viewportIndex = static_cast<size_t>(slot);
    // Dragging the rotation widget in a free orthographic view must keep
    // its projection. Fixed-axis views retain their explicit escape to 3D.
    if (auto* controller = GetSceneControllerForSlot(slot);
        controller && controller->IsOrthographic() && !controller->UsesFixedViewPanControls())
        return;
    const bool primaryFullView = !IsQuadViewEnabled() && slot == ViewportSlot::Perspective;
    if (primaryFullView)
    {
        if (Label* label = m_ViewportLabels[viewportIndex])
            label->SetText(GetQuadViewKindLabel(QuadViewKind::Perspective));
        if (SceneViewController* controller = GetSceneControllerForSlot(slot))
        {
            controller->SetFixedViewOrientation(SceneViewController::FixedViewOrientation::Free);
            controller->Set2DMode(false);
            controller->SetOrthographic(false);
        }
        return;
    }

    m_QuadViewKinds[viewportIndex] = QuadViewKind::Perspective;
    if (SceneViewController* controller = GetSceneControllerForSlot(slot))
    {
        controller->SetFixedViewOrientation(SceneViewController::FixedViewOrientation::Free);
        controller->Set2DMode(false);
        controller->SetOrthographic(false);
    }
    UpdateQuadViewLabel(slot);
}

void SceneViewPanel::NotifyRotationGizmoAxisSnap(SceneViewport& pane, int axis)
{
    const std::optional<ViewportSlot> paneSlot = FindViewportSlot(pane);
    if (!paneSlot)
        return;
    const size_t viewportIndex = static_cast<size_t>(*paneSlot);

    QuadViewKind kind = QuadViewKind::Perspective;
    switch (axis)
    {
    case 0:
    case 3:
        kind = QuadViewKind::Side;
        break;
    case 1:
    case 4:
        kind = QuadViewKind::Top;
        break;
    case 2:
    case 5:
        kind = QuadViewKind::Front;
        break;
    default:
        return;
    }

    const auto slot = static_cast<ViewportSlot>(viewportIndex);
    if (!IsQuadViewEnabled() && slot == ViewportSlot::Perspective)
    {
        if (Label* label = m_ViewportLabels[viewportIndex])
            label->SetText(GetQuadViewKindLabel(kind));
        return;
    }

    m_QuadViewKinds[viewportIndex] = kind;
    UpdateQuadViewLabel(slot);
}

UIElement* SceneViewPanel::GetActiveViewportElement() const
{
    if (UIElement* viewport = GetViewportElementForSlot(m_ActiveViewportSlot))
        return viewport;
    return m_Viewport;
}

UIElement* SceneViewPanel::GetViewportForOverlay() const
{
    return GetActiveViewportElement();
}

void SceneViewPanel::RefreshOverlayElementPointers()
{
    m_FpsLabel = dynamic_cast<Label*>(FindViewportOverlayById("SceneViewFpsLabel"_sid));
}

UIElement* SceneViewPanel::FindViewportOverlayById(StringId id) const
{
    if (id == 0)
        return nullptr;

    const auto findIn = [id](UIElement* root) -> UIElement*
    {
        return root ? root->FindById(id) : nullptr;
    };

    if (UIElement* element = findIn(m_OverlayViewport))
        return element;

    if (UIElement* active = GetViewportForOverlay())
    {
        if (active != m_OverlayViewport)
        {
            if (UIElement* element = findIn(active))
                return element;
        }
    }

    for (UIElement* viewport : m_ViewportSlots)
    {
        if (!viewport || viewport == m_OverlayViewport)
            continue;
        if (UIElement* element = findIn(viewport))
            return element;
    }

    return const_cast<SceneViewPanel*>(this)->FindById(id);
}

UIElement* SceneViewPanel::HeldSharedViewportOverlay(StringId id) const
{
    static_assert(std::size(kSharedViewportOverlayIds) == std::tuple_size_v<decltype(m_HeldSharedViewportOverlays)>,
                  "m_HeldSharedViewportOverlays has one hold for each id in kSharedViewportOverlayIds");
    const auto it = std::find(std::begin(kSharedViewportOverlayIds), std::end(kSharedViewportOverlayIds), id);
    if (it == std::end(kSharedViewportOverlayIds))
        return FindViewportOverlayById(id);
    UIElement::WeakRef<UIElement>& held =
        m_HeldSharedViewportOverlays[static_cast<size_t>(it - std::begin(kSharedViewportOverlayIds))];
    if (UIElement* overlay = held.Get())
        return overlay;
    UIElement* overlay = FindViewportOverlayById(id);
    held = UIElement::MakeWeakRef(overlay);
    return overlay;
}

bool SceneViewPanel::ViewportOverlaysNeedSync() const
{
    UIElement* target = GetViewportForOverlay();
    if (!target)
        return false;

    if (m_OverlayViewport != target)
        return true;

    for (StringId id : kSharedViewportOverlayIds)
    {
        UIElement* element = HeldSharedViewportOverlay(id);
        if (element && element->GetParent() != target)
            return true;
    }

    return false;
}

void SceneViewPanel::RequestViewportOverlaySync()
{
    if (m_ViewportOverlaySyncPosted)
        return;
    m_ViewportOverlaySyncPosted = true;
    PostSafeAction([this]()
    {
        m_ViewportOverlaySyncPosted = false;
        SyncViewportOverlays();
    });
}

void SceneViewPanel::SyncViewportOverlays()
{
    UIElement* target = GetViewportForOverlay();
    if (!target)
        return;

    if (UIElement::IsInEventDispatch())
    {
        RequestViewportOverlaySync();
        return;
    }

    // Shared editor chrome follows the active pane: the tools apply to the focused view.
    // Orientation gizmos stay parented to each viewport: one view cube per pane.
    for (StringId id : kSharedViewportOverlayIds)
    {
        UIElement* element = HeldSharedViewportOverlay(id);
        if (!element || element->GetParent() == target)
            continue;
        UIElement* parent = element->GetParent();
        if (!parent)
            continue;
        std::unique_ptr<UIElement> owned = parent->TakeChild(element);
        if (owned)
            target->AddChild(std::move(owned));
        else
            RequestViewportOverlaySync();
    }

    m_OverlayViewport = target;
    RefreshOverlayElementPointers();
    target->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty | UIElement::ChildrenDirty);
    target->RequestRelayout();
}

void SceneViewPanel::SetQuadViewKind(ViewportSlot slot, QuadViewKind kind)
{
    const size_t slotIndex = static_cast<size_t>(slot);
    const size_t kindIndex = static_cast<size_t>(kind);
    if (slotIndex >= m_QuadViewKinds.size() || kindIndex >= static_cast<size_t>(QuadViewKind::Count))
        return;
    if (!IsQuadViewKindAllowedForSlot(slot, kind))
        return;
    const QuadViewKind previousKind = m_QuadViewKinds[slotIndex];
    if (previousKind == kind)
        return;

    const ViewportSlot previousKindSlot = FindSlotForQuadViewKind(kind);
    const size_t previousKindSlotIndex = static_cast<size_t>(previousKindSlot);
    m_QuadViewKinds[slotIndex] = kind;
    if (previousKindSlot != slot &&
        previousKindSlotIndex < m_QuadViewKinds.size() &&
        m_QuadViewKinds[previousKindSlotIndex] == kind)
    {
        m_QuadViewKinds[previousKindSlotIndex] = previousKind;
        ApplyQuadViewKind(previousKindSlot);
    }
    ApplyQuadViewKind(slot);
    SyncViewportOverlays();
    if (UIElement* host = FindById("SceneViewViewportHost"_sid))
    {
        host->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        host->RequestRelayout();
    }
}

void SceneViewPanel::ResetQuadViewAssignments()
{
    constexpr std::array<QuadViewKind, static_cast<size_t>(ViewportSlot::Count)> kDefaults{
        QuadViewKind::Perspective,
        QuadViewKind::Top,
        QuadViewKind::Front,
        QuadViewKind::Side
    };
    m_QuadViewKinds = kDefaults;
    for (size_t i = 0; i < m_QuadViewKinds.size(); ++i)
        ApplyQuadViewKind(static_cast<ViewportSlot>(i));
    SyncViewportOverlays();

    if (UIElement* host = FindById("SceneViewViewportHost"_sid))
    {
        host->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        host->RequestRelayout();
    }
}
void SceneViewPanel::ApplyQuadViewLayout()
{
    UIElement* host = FindById("SceneViewViewportHost"_sid);
    const float hostW = host ? std::max(1.0f, host->GetLayoutWidth()) : 1.0f;
    const float hostH = host ? std::max(1.0f, host->GetLayoutHeight()) : 1.0f;
    if (host)
    {
        if (m_QuadViewEnabled)
            host->AddClass("quad-layout");
        else
            host->RemoveClass("quad-layout");
    }

    for (size_t i = 0; i < m_ViewportSlots.size(); ++i)
    {
        UIElement* vp = m_ViewportSlots[i];
        if (!vp)
            continue;
        const bool active = (i == static_cast<size_t>(m_ActiveViewportSlot));
        auto& style = vp->Style();
        if (active)
            vp->AddClass("active-viewport");
        else
            vp->RemoveClass("active-viewport");

        if (!m_QuadViewEnabled && !active)
            vp->AddClass("hidden");
        else
            vp->RemoveClass("hidden");

        if (m_QuadViewEnabled)
        {
            const auto slot = static_cast<ViewportSlot>(i);
            bool leftColumn = false;
            bool topRow = false;
            GetQuadCorner(slot, leftColumn, topRow);
            const float rowSplitX = topRow ? m_QuadSplitTopX : m_QuadSplitBottomX;
            const float leftPct = (leftColumn ? 0.0f : rowSplitX) * 100.0f;
            const float topPct = (topRow ? 0.0f : m_QuadSplitY) * 100.0f;
            const float widthPct = (leftColumn ? rowSplitX : 1.0f - rowSplitX) * 100.0f;
            const float heightPct = (topRow ? m_QuadSplitY : 1.0f - m_QuadSplitY) * 100.0f;
            const bool collapsed = (widthPct <= 0.001f) || (heightPct <= 0.001f);
            if (collapsed)
                vp->AddClass("quad-collapsed");
            else
                vp->RemoveClass("quad-collapsed");
            style.Set(Style::Position, PositionType::Absolute)
                 .Set(Style::PositionLeft, StyleLength::Percent(leftPct))
                 .Set(Style::PositionTop, StyleLength::Percent(topPct))
                 .Set(Style::FlexGrow, 0.0f)
                 .Set(Style::FlexShrink, 0.0f)
                 .Set(Style::FlexBasis, StyleLength::Percent(widthPct))
                 .Set(Style::Width, StyleLength::Percent(widthPct))
                 .Set(Style::Height, StyleLength::Percent(heightPct));
        }
        else
        {
            vp->RemoveClass("quad-collapsed");
            style.Reset(Style::FlexGrow);
            style.Reset(Style::FlexShrink);
            style.Reset(Style::FlexBasis);
            style.Reset(Style::Position);
            style.Reset(Style::PositionLeft);
            style.Reset(Style::PositionTop);
            style.Reset(Style::Width);
            style.Reset(Style::Height);
        }
    }

    constexpr float kSplitLinePx = 2.0f;
    constexpr float kSplitHandleHitPx = 12.0f;
    const auto handleLeftPx = [hostW](float splitX)
    {
        const float splitPx = std::clamp(splitX * hostW, 0.0f, hostW);
        return std::clamp(splitPx - (kSplitHandleHitPx * 0.5f), 0.0f, std::max(0.0f, hostW - kSplitHandleHitPx));
    };
    const auto handleTopPx = [hostH](float splitY)
    {
        const float splitPx = std::clamp(splitY * hostH, 0.0f, hostH);
        return std::clamp(splitPx - (kSplitHandleHitPx * 0.5f), 0.0f, std::max(0.0f, hostH - kSplitHandleHitPx));
    };

    const auto applyVerticalHandle = [this, hostW, hostH, handleLeftPx](UIElement* handle, const char* lineId, float splitX, float topY, float height)
    {
        if (!handle)
            return;
        if (m_QuadViewEnabled)
        {
            handle->RemoveClass("hidden");
            const float leftPx = handleLeftPx(splitX);
            const float splitPx = std::clamp(splitX * hostW, 0.0f, hostW);
            auto& style = handle->Style();
            style.Set(Style::Position, PositionType::Absolute)
                 .Set(Style::PositionLeft, StyleLength::Px(leftPx))
                 .Set(Style::PositionTop, StyleLength::Px(topY * hostH))
                 .Set(Style::Width, StyleLength::Px(kSplitHandleHitPx))
                 .Set(Style::Height, StyleLength::Px(std::max(0.0f, height * hostH)));
            if (UIElement* line = handle->FindById(HashStringId(lineId)))
            {
                const float lineLeftPx = std::clamp(splitPx - leftPx, 0.0f, kSplitHandleHitPx - kSplitLinePx);
                line->Style()
                    .Set(Style::Position, PositionType::Absolute)
                    .Set(Style::PositionLeft, StyleLength::Px(lineLeftPx))
                    .Set(Style::PositionTop, StyleLength::Px(0.0f))
                    .Set(Style::Width, StyleLength::Px(kSplitLinePx))
                    .Set(Style::Height, StyleLength::Percent(100.0f));
            }
        }
        else
        {
            handle->AddClass("hidden");
        }
    };
    const auto applyHorizontalHandle = [this, hostH, handleTopPx](UIElement* handle)
    {
        if (!handle)
            return;
        if (m_QuadViewEnabled)
        {
            handle->RemoveClass("hidden");
            const float topPx = handleTopPx(m_QuadSplitY);
            const float splitPx = std::clamp(m_QuadSplitY * hostH, 0.0f, hostH);
            auto& style = handle->Style();
            style.Set(Style::Position, PositionType::Absolute)
                 .Set(Style::PositionLeft, StyleLength::Px(0.0f))
                 .Set(Style::PositionTop, StyleLength::Px(topPx))
                 .Set(Style::Width, StyleLength::Percent(100.0f))
                 .Set(Style::Height, StyleLength::Px(kSplitHandleHitPx));
            if (UIElement* line = handle->FindById("SceneViewQuadSplitHorizontalLine"_sid))
            {
                const float lineTopPx = std::clamp(splitPx - topPx, 0.0f, kSplitHandleHitPx - kSplitLinePx);
                line->Style()
                    .Set(Style::Position, PositionType::Absolute)
                    .Set(Style::PositionLeft, StyleLength::Px(0.0f))
                    .Set(Style::PositionTop, StyleLength::Px(lineTopPx))
                    .Set(Style::Width, StyleLength::Percent(100.0f))
                    .Set(Style::Height, StyleLength::Px(kSplitLinePx));
            }
        }
        else
        {
            handle->AddClass("hidden");
        }
    };
    applyVerticalHandle(m_QuadSplitVerticalTop, "SceneViewQuadSplitVerticalTopLine", m_QuadSplitTopX, 0.0f, m_QuadSplitY);
    applyVerticalHandle(m_QuadSplitVerticalBottom, "SceneViewQuadSplitVerticalBottomLine", m_QuadSplitBottomX, m_QuadSplitY, 1.0f - m_QuadSplitY);
    applyHorizontalHandle(m_QuadSplitHorizontal);
    SyncViewportOverlays();
}

void SceneViewPanel::RegisterQuadSplitHandleEvents()
{
    if (m_QuadSplitHandleEventsBound)
        return;
    if (!m_QuadSplitVerticalTop || !m_QuadSplitVerticalBottom || !m_QuadSplitHorizontal)
        return;

    const auto bindHandle = [this](UIElement* handle, QuadSplitDragAxis axis)
    {
        if (!handle)
            return;
        handle->RegisterEventHandler(kEventMouseDown, [this, handle, axis](UIEvent& ev) {
            BeginQuadSplitDrag(handle, axis, ev);
        });
        handle->RegisterEventHandler(kEventMouseMove, [this](UIEvent& ev) {
            UpdateQuadSplitDrag(ev);
        });
        handle->RegisterEventHandler(kEventMouseUp, [this](UIEvent& ev) {
            EndQuadSplitDrag(ev);
        });
    };

    bindHandle(m_QuadSplitVerticalTop, QuadSplitDragAxis::VerticalTop);
    bindHandle(m_QuadSplitVerticalBottom, QuadSplitDragAxis::VerticalBottom);
    bindHandle(m_QuadSplitHorizontal, QuadSplitDragAxis::Horizontal);
    m_QuadSplitHandleEventsBound = true;
}

void SceneViewPanel::BeginQuadSplitDrag(UIElement* handle, QuadSplitDragAxis axis, UIEvent& ev)
{
    if (!m_QuadViewEnabled || !handle || ev.Button != 0 || axis == QuadSplitDragAxis::None)
        return;
    m_QuadSplitDragAxis = axis;
    ev.Capture(handle);
    ev.Stop();
}

void SceneViewPanel::UpdateQuadSplitDrag(UIEvent& ev)
{
    if (m_QuadSplitDragAxis == QuadSplitDragAxis::None)
        return;

    UIElement* host = FindById("SceneViewViewportHost"_sid);
    if (!host)
        return;

    constexpr float kMinSplit = 0.0f;
    constexpr float kMaxSplit = 1.0f;
    if (m_QuadSplitDragAxis == QuadSplitDragAxis::VerticalTop ||
        m_QuadSplitDragAxis == QuadSplitDragAxis::VerticalBottom)
    {
        const float width = std::max(1.0f, host->GetLayoutWidth());
        const float splitX = std::clamp((ev.X - host->GetLayoutX()) / width, kMinSplit, kMaxSplit);
        if (m_QuadSplitDragAxis == QuadSplitDragAxis::VerticalTop)
            m_QuadSplitTopX = splitX;
        else
            m_QuadSplitBottomX = splitX;
    }
    else
    {
        const float height = std::max(1.0f, host->GetLayoutHeight());
        m_QuadSplitY = std::clamp((ev.Y - host->GetLayoutY()) / height, kMinSplit, kMaxSplit);
    }
    ApplyQuadViewLayout();
    host->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    host->RequestRelayout();
    ev.Stop();
}

void SceneViewPanel::EndQuadSplitDrag(UIEvent& ev)
{
    if (m_QuadSplitDragAxis == QuadSplitDragAxis::None)
        return;
    m_QuadSplitDragAxis = QuadSplitDragAxis::None;
    ev.Stop();
}
std::vector<ContextMenuManipulator::Item> SceneViewPanel::BuildQuadViewMenuItems()
{
    std::vector<ContextMenuManipulator::Item> items;
    items.push_back({.Path = "Reset to 4 Split",
                     .IconPath = EditorIcons::kReset,
                     .OnActivate = [this] { ResetQuadViewSplits(); }});
    items.push_back({.Path = "Reset Views to Default",
                     .IconPath = EditorIcons::kReset,
                     .OnActivate = [this] { ResetQuadViewAssignments(); }});
    items.push_back({.Separator = true});

    constexpr uint32_t slotCount = static_cast<uint32_t>(ViewportSlot::Count);
    constexpr uint32_t viewCount = static_cast<uint32_t>(QuadViewKind::Count);
    for (uint32_t slotIndex = 0; slotIndex < slotCount; ++slotIndex)
    {
        const auto slot = static_cast<ViewportSlot>(slotIndex);
        const std::string submenu = std::string(GetViewportSlotMenuLabel(slot)) + " View";
        items.push_back({.Path = submenu, .IconPath = EditorIcons::kCamera});
        for (uint32_t viewIndex = 0; viewIndex < viewCount; ++viewIndex)
        {
            const auto kind = static_cast<QuadViewKind>(viewIndex);
            if (!IsQuadViewKindAllowedForSlot(slot, kind))
                continue;
            items.push_back(
                {.Path = submenu + "/" + GetQuadViewKindMenuLabel(kind),
                 .IconPath = EditorIcons::kCamera,
                 .Flags = MenuItemFlag_Radio |
                          (GetQuadViewKind(slot) == kind ? MenuItemFlag_Checked : MenuItemFlag_None),
                 .OnActivate = [this, slot, kind] { SetQuadViewKind(slot, kind); }});
        }
    }
    return items;
}

void SceneViewPanel::UpdateQuadViewButtonState()
{
    if (auto* btn = dynamic_cast<Button*>(FindById("QuadViewToggle"_sid)))
    {
        if (m_QuadViewEnabled)
            btn->AddClass("icon-active");
        else
            btn->RemoveClass("icon-active");
    }
}

} // namespace GameEngine
