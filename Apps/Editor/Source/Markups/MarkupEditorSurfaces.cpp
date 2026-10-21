#include "Markups/MarkupEditorSurfaces.h"

#include "Components/Markup/Markup.h"
#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "ECS/ECSTemplates.h"
#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "Editor/Registries/EditorMarkupRequests.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupInspector.h"
#include "Markups/MarkupKind.h"
#include "Markups/MarkupLabelOverlay.h"
#include "Markups/MarkupRequestGate.h"
#include "Markups/MarkupTool.h"
#include "Markups/MarkupsPanel.h"
#include "SceneView/SceneViewCameraRig.h"
#include "SceneView/SceneViewFraming.h"
#include "SceneView/SceneViewToolStripRegistry.h"
#include "SceneView/SplineOwnerQuery.h"
#include "UI/EditorIcons.h"
#include "UI/UIElement.h"
#include "UI/ViewOverlayHost.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

int64 SystemUnixSeconds()
{
    return static_cast<int64>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

void RegisterMarkupsPanel(MarkupEditorBridge& bridge)
{
    EditorPanelDescriptor descriptor;
    descriptor.PanelId = MarkupsPanel::kPanelId;
    descriptor.Title = "Mark-ups";
    descriptor.Factory = [&bridge]() -> std::unique_ptr<UIElement> {
        return std::make_unique<MarkupsPanel>(bridge);
    };
    EditorPanelRegistry::Get().RegisterPanel(std::move(descriptor));
    // Attached at the UI root: the inspector section lives outside the panel.
    EditorPanelRegistry::Get().RegisterEditorStyleSheet({"editor", "UI/panels/MarkupsPanel.css"});
}

void RegisterMarkupToolStripEntry(MarkupEditorBridge& bridge)
{
    SceneViewToolStripEntry entry;
    entry.Id = "markups";
    entry.Tooltip = "Mark-up: click the world to mark a place for the agent; right-click for the shape (box, "
                    "sphere, or a region you draw)";
    entry.Icon = EditorIcons::kMarkup;
    entry.ContextMenuItems = &BuildMarkupToolMenuItems;
    entry.CreateTool = [&bridge](SceneViewController& owner) -> std::unique_ptr<SceneTools::ISceneTool> {
        return std::make_unique<MarkupTool>(owner, bridge);
    };
    entry.BadgeCount = [&bridge]() -> std::size_t {
        ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
        return world ? bridge.CountUnseen(*world) : 0;
    };
    entry.BadgeButtonTooltip = "Activity: what changed in the mark-ups";
    entry.BadgeButtonIcon = EditorIcons::kActivity;
    entry.OnBadgeButton = [](UIElement&) {
        EditorPanelRegistry::Get().OpenPanel(MarkupsPanel::kPanelId);
        if (MarkupsPanel* panel = MarkupsPanel::TryGetOpen())
            panel->ShowTab(MarkupsPanel::Tab::Activity);
    };
    SceneViewToolStripRegistry::Get().Register(std::move(entry));
}

// A request's JSON answer as the package seam carries it: a refusal's reason, else the result.
MarkupRequestAnswer ToAnswer(nlohmann::json result)
{
    if (IsRefusal(result))
        return MarkupRequestAnswer{{}, RefusalReason(result)};
    return MarkupRequestAnswer{std::move(result), {}};
}

// The seam's Frame: refuses as the other requests do (play mode, no scene, no such mark-up), then
// when there is no Scene View to frame in.
MarkupRequestAnswer FrameRequest(const nlohmann::json& params)
{
    ECS::EntityHandle entity{};
    if (std::optional<nlohmann::json> refusal = ResolveMarkupRequest(CurrentMarkupRequestContext(), params, entity))
        return ToAnswer(std::move(*refusal));
    if (FrameMarkupInSceneView(entity) != MarkupFrameOutcome::Framed)
        return MarkupRequestAnswer{{}, "Open a Scene View to frame the mark-up"};
    return MarkupRequestAnswer{nlohmann::json::object(), {}};
}

} // namespace

MarkupRequestContext CurrentMarkupRequestContext()
{
    MarkupRequestContext context;
    context.World = EngineCore::GetInstance().GetPrimaryWorld();
    context.Bridge = MarkupEditorBridge::TryGet();
    if (!context.Bridge)
        return context;
    context.Undo = context.Bridge->GetUndoService();
    context.Notifications = &context.Bridge->GetNotifications();
    context.PlayMode = context.Bridge->IsInPlayMode();
    context.FrameTanHalfFov = MarkupFrameTanHalfFov(context.Bridge->GetSceneView());
    return context;
}

MarkupFrameOutcome FrameMarkupInSceneView(ECS::EntityHandle entity)
{
    MarkupEditorBridge* bridge = MarkupEditorBridge::TryGet();
    if (!bridge || !bridge->GetSceneView())
        return MarkupFrameOutcome::NoSceneView;
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world || !world->IsValid(entity) || !world->GetComponent<Components::Markup>(entity))
        return MarkupFrameOutcome::NoMarkup;
    bridge->SelectMarkup(*world, entity);
    SceneViewController& view = *bridge->GetSceneView();
    const std::optional<MarkupWorldVolume> volume = ReadMarkupFrameVolume(CurrentMarkupRequestContext(), entity);
    if (!volume)
    {
        view.FrameOrigin();
        return MarkupFrameOutcome::Framed;
    }
    // A tween, as the Scene View's own framing moves: it carries the new pitch into the panel's
    // angles, which a pose set directly would be overwritten by on the next frame.
    view.StartCameraTween(MarkupFramePose(*volume, view.GetCameraPose(), MarkupFrameTanHalfFov(&view)));
    return MarkupFrameOutcome::Framed;
}

SceneViewCameraPose MarkupFramePose(const MarkupWorldVolume& volume, const SceneViewCameraPose& current,
                                    float tanHalfFov)
{
    const double pitch = std::min(static_cast<double>(current.PitchDeg), -static_cast<double>(kMarkupFrameMinDownPitchDeg));
    const CameraRig::Vec3d look = CameraRig::LookDirection(current.YawDeg, pitch);
    const Mathematics::Vector3 direction(static_cast<float>(look.X), static_cast<float>(look.Y),
                                         static_cast<float>(look.Z));
    SceneViewCameraPose pose = current;
    ComputeLookAtPose(volume.Center, direction, MarkupFrameDistance(volume, tanHalfFov), pose);
    return pose;
}

void InstallMarkupRequests()
{
    EditorMarkupRequests::Handlers handlers;
    handlers.List = [](const nlohmann::json& params) {
        return ToAnswer(ListMarkups(CurrentMarkupRequestContext(), params));
    };
    handlers.Query = [](const nlohmann::json& params) {
        return ToAnswer(GetMarkup(CurrentMarkupRequestContext(), params));
    };
    handlers.Comment = [](const nlohmann::json& params) {
        return ToAnswer(CommentOnMarkup(CurrentMarkupRequestContext(), params));
    };
    handlers.Frame = &FrameRequest;
    EditorMarkupRequests::Get().Install(std::move(handlers));
}

std::unique_ptr<MarkupEditorBridge> CreateMarkupEditorBridge(EditorChangeNotifications& notifications,
                                                             MarkupEditorBridge::ScenePath scenePath)
{
    auto bridge = std::make_unique<MarkupEditorBridge>(notifications, &SystemUnixSeconds, std::move(scenePath));
    MarkupEditorBridge::Install(bridge.get());
    RegisterMarkupRequestGate(DebugRequestGateRegistry::Get());
    RegisterMarkupToolStripEntry(*bridge);
    RegisterMarkupsPanel(*bridge);
    RegisterMarkupInspector(*bridge);
    SetSplineOwnerQuery(&QueryMarkupSpline);
    ViewOverlayHost::Get().Register(std::make_unique<MarkupLabelOverlay>());
    InstallMarkupRequests();
    return bridge;
}

} // namespace GameEngine::Editor
