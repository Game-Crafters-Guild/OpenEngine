#pragma once

// Commonly used editor icon image paths ("editor:" resolves in the editor
// asset mount). Add an entry here when an icon is shared across
// panels/inspectors instead of repeating the path per call site.
namespace GameEngine::EditorIcons
{
    // Clipboard and history.
    inline constexpr const char* kCopy = "editor:Icons/copy.svg";
    inline constexpr const char* kPaste = "editor:Icons/paste.svg";
    inline constexpr const char* kUndo = "editor:Icons/undo.svg";
    // Destructive.
    inline constexpr const char* kTrash = "editor:Icons/trash.svg";
    inline constexpr const char* kClose = "editor:Icons/xclose.svg";
    // Restore-shaped actions.
    inline constexpr const char* kReset = "editor:Icons/ResetReload.svg";
    // Creation.
    inline constexpr const char* kPlus = "editor:Icons/plus.svg";
    // Files and navigation.
    inline constexpr const char* kSave = "editor:Icons/save.svg";
    inline constexpr const char* kFolderOpen = "editor:Icons/folder_open.svg";
    inline constexpr const char* kFolder = "editor:Icons/Folder@64px.svg";
    inline constexpr const char* kSearch = "editor:Icons/search.svg";
    inline constexpr const char* kBookmark = "editor:Icons/book.svg";
    inline constexpr const char* kCloud = "editor:Icons/cloud.svg";
    inline constexpr const char* kInfo = "editor:Icons/info.svg";
    inline constexpr const char* kLink = "editor:Icons/link.svg";
    inline constexpr const char* kStats = "editor:Icons/stats.svg";
    // Settings-shaped.
    inline constexpr const char* kSettings = "editor:Icons/settings.svg";
    inline constexpr const char* kBrush = "editor:Icons/brush.svg";
    inline constexpr const char* kPencil = "editor:Icons/draw_curve.svg";
    // Visibility and ordering.
    inline constexpr const char* kEye = "editor:Icons/eye.svg";
    inline constexpr const char* kSortList = "editor:Icons/list.svg";
    inline constexpr const char* kArrowUp = "editor:Icons/ArrowUp.svg";
    inline constexpr const char* kArrowDown = "editor:Icons/ArrowDown.svg";
    // Scene content.
    inline constexpr const char* kScene = "editor:Icons/scene.svg";
    inline constexpr const char* kCamera = "editor:Icons/camera.svg";
    inline constexpr const char* kLight = "editor:Icons/PointLight.svg";
    inline constexpr const char* kMaterial = "editor:Icons/material.svg";
    inline constexpr const char* kTerrain = "editor:Icons/terrain.svg";
    inline constexpr const char* kOcean = "editor:Icons/ocean.svg";
    inline constexpr const char* kWind = "editor:Icons/wind.svg";
    inline constexpr const char* kProbe = "editor:Icons/probe.svg";
    inline constexpr const char* kDDGIVolume = "editor:Icons/ddgivolume.svg";
    inline constexpr const char* kTree = "editor:Icons/SettingsTrees.svg";
    inline constexpr const char* kPulse = "editor:Icons/pulse.svg";
    inline constexpr const char* kMarkup = "editor:Icons/markup.svg";
    // A mark-up's shapes, the same glyphs the Mark-ups panel shows beside each row.
    inline constexpr const char* kMarkupBox = "editor:Icons/Cube.svg";
    inline constexpr const char* kMarkupSphere = "editor:Icons/markup-sphere.svg";
    inline constexpr const char* kMarkupRegion = "editor:Icons/markup-region.svg";
    inline constexpr const char* kActivity = "editor:Icons/activity.svg";
    inline constexpr const char* kSpline = "editor:Icons/GizmoSpline.svg";
    inline constexpr const char* kNavGrid = "editor:Icons/grid.svg";
    inline constexpr const char* kNode = "editor:Icons/node.svg";
    inline constexpr const char* kBox = "editor:Icons/box.svg";
    inline constexpr const char* kCube = "editor:Icons/Cube.svg";
    inline constexpr const char* kSphere = "editor:Icons/Sphere.svg";
    inline constexpr const char* kPlane = "editor:Icons/Plane.svg";
    inline constexpr const char* kCapsule = "editor:Icons/Capsule.svg";
    inline constexpr const char* kScript = "editor:Icons/log.svg";
    inline constexpr const char* kTag = "editor:Icons/SettingsTags.svg";
    inline constexpr const char* kVcs = "editor:Icons/SettingsVersionControl.svg";
    inline constexpr const char* kLockClosed = "editor:Icons/LockClosed.svg";
    inline constexpr const char* kLockOpen = "editor:Icons/LockOpen.svg";
    // Post-processing stack.
    inline constexpr const char* kPostFx = "editor:Icons/postfx.svg";
    inline constexpr const char* kColorFilter = "editor:Icons/color-filter.svg";
    // Animation editing.
    inline constexpr const char* kFilm = "editor:Icons/film.svg";
    inline constexpr const char* kPlay = "editor:Icons/play.svg";
    inline constexpr const char* kAddKeyMarker = "editor:Icons/add_key_marker.svg";
    inline constexpr const char* kAddMarker = "editor:Icons/marker.svg";
    inline constexpr const char* kBreakTangents = "editor:Icons/break_tangent.svg";
    inline constexpr const char* kUnifyTangents = "editor:Icons/unify_tangent.svg";
    inline constexpr const char* kLinearCurve = "editor:Icons/linear_curve.svg";
    inline constexpr const char* kEase = "editor:Icons/ease_ease.svg";
    inline constexpr const char* kSplineCurve = "editor:Icons/spline_curve.svg";
    inline constexpr const char* kSteppedCurve = "editor:Icons/stepped_curve.svg";
    inline constexpr const char* kDrawCurve = kPencil; // curve-tool name for the same pencil glyph
    inline constexpr const char* kLoop = "editor:Icons/looptime.svg";
    inline constexpr const char* kLattice = "editor:Icons/lattice.svg";
    // Component / entity types.
    inline constexpr const char* kVideoCam = "editor:Icons/videocam.svg";
    inline constexpr const char* kSkyEnvironment = "editor:Icons/skyenvironment.svg";
    inline constexpr const char* kSparkles = "editor:Icons/sparkles.svg";
    inline constexpr const char* kAudioEnabled = "editor:Icons/audio-on-speaker.svg";
    inline constexpr const char* kAudioMuted = "editor:Icons/audio-muted-speaker.svg";
    inline constexpr const char* kMusicNote = "editor:Icons/music-note-icon@32px.svg";
    inline constexpr const char* kPhysics = "editor:Icons/PhysicsSettings.svg";
    // Tools.
    inline constexpr const char* kMove = "editor:Icons/GiszmoTranslate.svg";
    inline constexpr const char* kAlarm = "editor:Icons/alarm.svg";
    inline constexpr const char* kPointer = "editor:Icons/arrowmouse.svg";
    inline constexpr const char* kRuler = "editor:Icons/Ruler.svg";
    inline constexpr const char* kColorPicker = "editor:Icons/colorpicker.svg";
    inline constexpr const char* k2D = "editor:Icons/2d.svg";
    inline constexpr const char* kDot = "editor:Icons/dot.svg";
    inline constexpr const char* kMagnet = "editor:Icons/magnet.svg";
    inline constexpr const char* kNavGizmo = "editor:Icons/navgizmo.svg";
} // namespace GameEngine::EditorIcons
