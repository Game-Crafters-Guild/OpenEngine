#pragma once

#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupRequests.h"
#include "SceneViewController.h"

#include <memory>

namespace GameEngine::Editor
{
class EditorChangeNotifications;

// The editor's mark-ups: the bridge (the system clock, the open scene's file from
// `scenePath`, installed as TryGet's; the project's state is loaded once the engine knows
// the project, LoadProjectState) and what it registers with the editor's registries: the
// Scene View tool strip entry with its Activity badge, the Mark-ups panel, the
// Markup inspector section, the Scene View's labels (MarkupLabelOverlay) and the mark-up
// requests package modules call (Editor/Registries/EditorMarkupRequests.h).
std::unique_ptr<MarkupEditorBridge> CreateMarkupEditorBridge(EditorChangeNotifications& notifications,
                                                             MarkupEditorBridge::ScenePath scenePath);

// What the mark-up requests act on now: the primary world, the installed bridge with its undo
// history and notifications, whether the editor plays, and the main Scene View's field of
// view. The debug server's markup_* methods and the package requests both build it here.
MarkupRequestContext CurrentMarkupRequestContext();

// Installs the editor's mark-up requests for package modules (EditorMarkupRequests): list,
// query and comment answer as markup_list, markup_get and markup_comment do on
// CurrentMarkupRequestContext(); frame is FrameMarkupInSceneView. CreateMarkupEditorBridge
// installs them.
void InstallMarkupRequests();

// What FrameMarkupInSceneView did.
enum class MarkupFrameOutcome
{
    Framed,
    NoSceneView, // no installed bridge or no main Scene View: nothing done
    NoMarkup,    // `entity` is not a mark-up of the primary world: nothing done
};

// Selects the mark-up `entity` of the primary world and frames it in the main Scene View: what a
// double-click on its Mark-ups panel row or a click on its Activity row does. It fits the shape
// markup_frame fits (ReadMarkupFrameVolume), seen along the view's heading and looking down at
// least kMarkupFrameMinDownPitchDeg, so a region's outline shows; a mark-up with no shape to fit
// (a region of fewer than three points) is framed at its origin.
MarkupFrameOutcome FrameMarkupInSceneView(ECS::EntityHandle entity);

// The least a framed view looks down, in degrees: a level view of an area of ground shows its
// walls edge-on and not its outline.
inline constexpr float kMarkupFrameMinDownPitchDeg = 30.0f;

// The pose FrameMarkupInSceneView sets for `volume` from the view's `current` pose: the same
// heading, pitched down at least kMarkupFrameMinDownPitchDeg, far enough that the volume's
// bounding sphere fits the field of view whose narrower half angle has tangent `tanHalfFov`.
SceneViewCameraPose MarkupFramePose(const MarkupWorldVolume& volume, const SceneViewCameraPose& current,
                                    float tanHalfFov);

} // namespace GameEngine::Editor
