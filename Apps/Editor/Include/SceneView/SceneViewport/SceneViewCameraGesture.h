#pragma once

#include "Input/KeyCodes.h"
#include "Mathematics/Vector2.h"

namespace GameEngine
{

/// The camera gesture of one Scene View pane: the look angles and the mouse drag that moves the camera.
/// The pane's pointer handlers write it; SceneViewport::Tick reads it once per frame and drives the controller.
/// Buttons are Input::kMouseButton_* values.
struct SceneViewCameraGesture
{
    /// The pointer travel in pixels at which a right press becomes a drag. A drag opens no context menu.
    static constexpr float kMenuClickTravelPx = 4.0f;

    float YawDeg = 0.0f;
    float PitchDeg = 0.0f;
    bool Dragging = false;
    int DragButton = -1;      // an Input::kMouseButton_* value, -1 without a drag
    bool AltOrbiting = false; // Alt + LMB in a free view
    bool Panning = false;     // Alt + Ctrl/Cmd + LMB, the pan hold + LMB, Alt + LMB or RMB or MMB in a fixed view
    bool Dollying = false;    // Alt + RMB
    bool WasOrbiting = false; // the orbit state of the previous frame
    bool ToolDrag = false;    // the press is a LMB press for the scene tools, not a camera gesture
    bool MenuCandidate = false; // the press is a right press that opens the context menu if it does not drag
    Mathematics::Vector2 PressPosition; // window pixels

    bool IsLooking() const { return Dragging && DragButton == Input::kMouseButton_Right && !AltOrbiting && !Panning && !Dollying; }
    bool IsOrbiting() const { return (Dragging && DragButton == Input::kMouseButton_Middle && !Panning) || AltOrbiting; }
    bool IsInteracting() const { return IsLooking() || IsOrbiting() || Panning || Dollying; }

    /// Starts the drag of a press. `fixedControls` is true in a 2D or fixed-axis view, where the pan replaces
    /// the orbit; `panHold` is true while the pan hold key (Space) is down.
    void BeginPress(int button, const Mathematics::Vector2& position, bool alt, bool primaryModifier,
                    bool fixedControls, bool panHold)
    {
        Dragging = true;
        DragButton = button;
        PressPosition = position;
        const bool left = button == Input::kMouseButton_Left;
        const bool right = button == Input::kMouseButton_Right;
        const bool middle = button == Input::kMouseButton_Middle;
        AltOrbiting = left && alt && !primaryModifier && !fixedControls;
        Panning = (left && alt && primaryModifier) || (left && alt && !primaryModifier && fixedControls) ||
                  (left && panHold) || (right && fixedControls && !alt) || (middle && fixedControls && !alt);
        Dollying = right && alt;
        MenuCandidate = right && !Dollying && !Panning;
        ToolDrag = left && !AltOrbiting && !Panning;
    }

    /// Follows a modifier pressed or released during the drag. A tool drag keeps its mode, so a marquee or a
    /// gizmo drag does not turn into a camera gesture.
    void UpdateModifiers(bool alt, bool primaryModifier, bool fixedControls, bool panHold)
    {
        if (!Dragging || ToolDrag)
            return;
        if (DragButton == Input::kMouseButton_Left)
        {
            AltOrbiting = alt && !primaryModifier && !fixedControls;
            Panning = (alt && primaryModifier) || (alt && !primaryModifier && fixedControls) || panHold;
        }
        else if (DragButton == Input::kMouseButton_Right)
        {
            Dollying = alt;
            Panning = !alt && fixedControls;
        }
        else if (DragButton == Input::kMouseButton_Middle)
        {
            AltOrbiting = false;
            Panning = fixedControls && !alt;
        }
    }

    /// Cancels the menu candidate once the pointer travels past kMenuClickTravelPx from the press.
    void TrackPointer(const Mathematics::Vector2& position)
    {
        if (MenuCandidate && Dragging && DragButton == Input::kMouseButton_Right &&
            (position - PressPosition).Length() > kMenuClickTravelPx)
            MenuCandidate = false;
    }

    /// Ends the drag when `button` is the drag's button. Returns true when the release ends a right click that
    /// did not drag: the release opens the context menu.
    bool EndPress(int button)
    {
        const bool menuClick = button == Input::kMouseButton_Right && MenuCandidate && Dragging &&
                               DragButton == Input::kMouseButton_Right;
        if (Dragging && button == DragButton)
        {
            Dragging = false;
            DragButton = -1;
            AltOrbiting = false;
            Panning = false;
            Dollying = false;
        }
        MenuCandidate = false;
        return menuClick;
    }

    /// True on the first frame of an orbit. Call once per frame: it records the orbit state for the next frame.
    bool ConsumeFirstOrbitFrame()
    {
        const bool orbiting = IsOrbiting();
        const bool firstFrame = orbiting && !WasOrbiting;
        WasOrbiting = orbiting;
        return firstFrame;
    }
};

} // namespace GameEngine
