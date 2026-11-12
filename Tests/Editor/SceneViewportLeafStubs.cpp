// Leaf stubs for SceneViewportTests.
//
// SceneViewport.cpp calls the bound controller, the edits seam, the overlays and the render coordinator. The
// tests build no real controller, so these definitions satisfy the link. GetOverlayHoverText returns the text a test
// sets. The rotation gizmo, the ruler and the measure overlay are their own classes with these stub members: each
// writes what it reads to its record (SceneViewportLeafStubs.h).

#include "SceneViewportLeafStubs.h"

#include "SceneView/SceneViewController.h"
#include "SceneView/SceneViewEvents.h"
#include "SceneView/SceneViewRenderCoordinator.h"
#include "SceneView/SceneViewport/SceneViewportEdits.h"
#include "UI/Controls/Widgets/SceneView/SceneViewMeasureOverlay.h"
// This file defines every member of SceneViewRulerOverlay, so clang checks its private fields here.
// SceneViewRulerOverlay.cpp reads these fields, and this target does not compile that file.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#include "UI/Controls/Widgets/SceneView/SceneViewRulerOverlay.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "UI/Controls/Widgets/ViewportRotationGizmo.h"
#include "UI/Registration/ElementRegistration.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
void SceneViewController::ApplyPixelPerfectIfActive() {}
void SceneViewController::CycleEntityUnderCursor(float, float, float, float, int) {}
void SceneViewController::FrameAll() {}
void SceneViewController::FrameOrigin() {}
std::string& Editor::TestStubs::OverlayHoverText()
{
    static std::string s_Text;
    return s_Text;
}

namespace
{
std::vector<std::pair<const UIElement*, Editor::TestStubs::OverlayRecord>>& Records()
{
    static std::vector<std::pair<const UIElement*, Editor::TestStubs::OverlayRecord>> s_Records;
    return s_Records;
}

bool IsRecordOf(const std::pair<const UIElement*, Editor::TestStubs::OverlayRecord>& entry, const UIElement* overlay)
{
    return entry.first == overlay;
}
} // namespace

Editor::TestStubs::OverlayRecord& Editor::TestStubs::RecordOf(const UIElement& overlay)
{
    auto& records = Records();
    for (auto& entry : records)
    {
        if (IsRecordOf(entry, &overlay))
            return entry.second;
    }
    return records.emplace_back(&overlay, OverlayRecord{}).second;
}

const Editor::TestStubs::OverlayRecord* Editor::TestStubs::FindRecord(const UIElement* overlay)
{
    for (const auto& entry : Records())
    {
        if (IsRecordOf(entry, overlay))
            return &entry.second;
    }
    return nullptr;
}

void Editor::TestStubs::ClearRecords()
{
    Records().clear();
}

const std::string& SceneViewController::GetOverlayHoverText() const
{
    return Editor::TestStubs::OverlayHoverText();
}
void SceneViewController::HandleHoverDetection(const Editor::SceneTools::ScenePointerEvent&) {}
bool SceneViewController::HasMeasureEndpointAtPointer(const Editor::SceneTools::ScenePointerEvent&)
{
    return false;
}
Editor::SceneTools::GizmoRay SceneViewController::MakeGizmoRay(float, float, float, float) const
{
    return {};
}
void SceneViewController::OnEntityPicked(ECS::EntityHandle, bool, bool, bool) {}
void SceneViewController::PopulatePointerCameraState(Editor::SceneTools::ScenePointerEvent&) const {}
bool SceneViewController::StepCameraHistoryBack()
{
    return false;
}
bool SceneViewController::StepCameraHistoryForward()
{
    return false;
}
void SceneViewController::Update(float, bool, bool, bool, bool, bool, bool, float) {}
void SceneViewController::UpdateCamera(float, bool, bool, bool, bool, bool, bool, float) {}
void SceneViewController::UpdateDolly(float) {}
void SceneViewController::UpdateOrbit(bool) {}
void SceneViewController::UpdatePan(float, float, float) {}
bool SceneViewController::ZoomOrthographicStep(int)
{
    return false;
}
bool SceneViewController::ZoomPixelPerfectAtCursor(int, float, float, float, float)
{
    return false;
}

SceneViewportEdits::SceneViewportEdits(ECS::World& world, const SceneViewportBinding& binding)
    : m_World(world), m_Binding(binding)
{
}
ECS::EntityHandle SceneViewportEdits::CreateMeasure(const Mathematics::Vector3&, const Mathematics::Vector3&, bool,
                                                    std::uint32_t)
{
    return {};
}

SceneViewMeasureOverlay::SceneViewMeasureOverlay() = default;
void SceneViewMeasureOverlay::Tick()
{
    Editor::TestStubs::OverlayRecord& record = Editor::TestStubs::RecordOf(*this);
    ++record.Ticks;
    record.Controller = m_Controller;
}
void SceneViewMeasureOverlay::OnGeneratePrimitives(UI::PrimitiveEmitContext&, const ResolvedStyle&, float, float, float,
                                                   float)
{
}
void SceneViewMeasureOverlay::SetMeasureToolEnabled(bool enabled)
{
    Editor::TestStubs::RecordOf(*this).MeasureEnabled = enabled;
}
void SceneViewMeasureOverlay::SetUnitSystem(UnitSystem units)
{
    Editor::TestStubs::RecordOf(*this).Units = units;
}
void SceneViewMeasureOverlay::SetTwoDMode(TwoDMode mode)
{
    Editor::TestStubs::RecordOf(*this).TwoDMode = mode;
}
void SceneViewMeasureOverlay::BeginMeasure(float, float, const Mathematics::Vector3&, bool, bool) {}
void SceneViewMeasureOverlay::UpdateMeasure(float, float, const Mathematics::Vector3&, bool) {}
void SceneViewMeasureOverlay::EndMeasure() {}
void SceneViewMeasureOverlay::CancelMeasure() {}
SceneViewRulerOverlay::SceneViewRulerOverlay() = default;
void SceneViewRulerOverlay::Tick()
{
    Editor::TestStubs::OverlayRecord& record = Editor::TestStubs::RecordOf(*this);
    ++record.Ticks;
    record.Controller = m_Controller;
}
void SceneViewRulerOverlay::SetCursor(float localX, float localY, bool inside)
{
    Editor::TestStubs::OverlayRecord& record = Editor::TestStubs::RecordOf(*this);
    record.Cursor = Mathematics::Vector2(localX, localY);
    record.CursorInside = inside;
}
void SceneViewRulerOverlay::OnGeneratePrimitives(UI::PrimitiveEmitContext&, const ResolvedStyle&, float, float, float,
                                                 float)
{
}

ViewportRotationGizmo::ViewportRotationGizmo() = default;
void ViewportRotationGizmo::Tick()
{
    Editor::TestStubs::OverlayRecord& record = Editor::TestStubs::RecordOf(*this);
    ++record.Ticks;
    record.Controller = m_Controller;
    record.Pane = m_Pane;
    record.ViewportIndex = m_ViewportIndex;
}
void ViewportRotationGizmo::OnGeneratePrimitives(UI::PrimitiveEmitContext&, const ResolvedStyle&, float, float, float,
                                                 float)
{
}

namespace Editor
{
bool SceneViewRenderCoordinator::TryResolveRenderableViewportSize(const UIManager*, UIElement*, std::uint32_t&,
                                                                  std::uint32_t&)
{
    return false;
}

namespace SceneTools
{
void PopulateScenePointerMods(ScenePointerEvent&, std::uint32_t) {}
} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine

namespace
{
// The overlay tags of the pane's layout build the stub classes above.
auto s_RegisterViewportRotationGizmo = GameEngine::UIRegistration::RegisterWithFactory<GameEngine::ViewportRotationGizmo>(
    "ViewportRotationGizmo", []() { return std::make_unique<GameEngine::ViewportRotationGizmo>(); });
auto s_RegisterSceneViewRulerOverlay = GameEngine::UIRegistration::RegisterWithFactory<GameEngine::SceneViewRulerOverlay>(
    "SceneViewRulerOverlay", []() { return std::make_unique<GameEngine::SceneViewRulerOverlay>(); });
auto s_RegisterSceneViewMeasureOverlay =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::SceneViewMeasureOverlay>(
        "SceneViewMeasureOverlay", []() { return std::make_unique<GameEngine::SceneViewMeasureOverlay>(); });
} // namespace
