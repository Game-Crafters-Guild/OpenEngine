// The Scene View camera gesture rules every pane shares: which press orbits, pans, dollies or looks, which press
// goes to the scene tools, when a right press opens the context menu, and how a modifier changes a drag.

#include <gtest/gtest.h>

#include "SceneView/SceneViewport/SceneViewCameraGesture.h"

namespace
{
using GameEngine::SceneViewCameraGesture;
using GameEngine::Mathematics::Vector2;

constexpr int kLeft = 0;
constexpr int kRight = 1;
constexpr int kMiddle = 2;
const Vector2 kPressPosition(100.0f, 100.0f);

struct PressModifiers
{
    bool Alt = false;
    bool Primary = false;
    bool FixedControls = false;
    bool PanHold = false;
};

SceneViewCameraGesture Press(int button, PressModifiers modifiers)
{
    SceneViewCameraGesture gesture;
    gesture.BeginPress(button, kPressPosition, modifiers.Alt, modifiers.Primary, modifiers.FixedControls,
                       modifiers.PanHold);
    return gesture;
}

TEST(SceneViewCameraGestureTests, AltLeftOrbitsInAFreeViewAndPansInAFixedView)
{
    const SceneViewCameraGesture free = Press(kLeft, {.Alt = true});
    EXPECT_TRUE(free.AltOrbiting);
    EXPECT_FALSE(free.Panning);
    EXPECT_TRUE(free.IsOrbiting());
    EXPECT_FALSE(free.ToolDrag);

    const SceneViewCameraGesture fixed = Press(kLeft, {.Alt = true, .FixedControls = true});
    EXPECT_FALSE(fixed.AltOrbiting);
    EXPECT_TRUE(fixed.Panning);
    EXPECT_FALSE(fixed.ToolDrag);
}

TEST(SceneViewCameraGestureTests, AltPrimaryLeftPans)
{
    const SceneViewCameraGesture gesture = Press(kLeft, {.Alt = true, .Primary = true});
    EXPECT_TRUE(gesture.Panning);
    EXPECT_FALSE(gesture.AltOrbiting);
}

TEST(SceneViewCameraGestureTests, PanHoldLeftPans)
{
    const SceneViewCameraGesture gesture = Press(kLeft, {.PanHold = true});
    EXPECT_TRUE(gesture.Panning);
    EXPECT_FALSE(gesture.ToolDrag);
}

TEST(SceneViewCameraGestureTests, PlainLeftIsAToolPress)
{
    const SceneViewCameraGesture gesture = Press(kLeft, {});
    EXPECT_TRUE(gesture.ToolDrag);
    EXPECT_FALSE(gesture.IsInteracting());
    EXPECT_FALSE(gesture.MenuCandidate);
}

TEST(SceneViewCameraGestureTests, RightPansInAFixedViewAndIsNotAMenuCandidate)
{
    const SceneViewCameraGesture gesture = Press(kRight, {.FixedControls = true});
    EXPECT_TRUE(gesture.Panning);
    EXPECT_FALSE(gesture.MenuCandidate);
}

TEST(SceneViewCameraGestureTests, PlainRightLooksAndIsAMenuCandidate)
{
    const SceneViewCameraGesture gesture = Press(kRight, {});
    EXPECT_TRUE(gesture.IsLooking());
    EXPECT_TRUE(gesture.MenuCandidate);
    EXPECT_FALSE(gesture.ToolDrag);
}

TEST(SceneViewCameraGestureTests, AltRightDolliesAndIsNotAMenuCandidate)
{
    const SceneViewCameraGesture gesture = Press(kRight, {.Alt = true});
    EXPECT_TRUE(gesture.Dollying);
    EXPECT_FALSE(gesture.IsLooking());
    EXPECT_FALSE(gesture.MenuCandidate);
}

TEST(SceneViewCameraGestureTests, MiddleOrbitsInAFreeViewAndPansInAFixedView)
{
    EXPECT_TRUE(Press(kMiddle, {}).IsOrbiting());
    const SceneViewCameraGesture fixed = Press(kMiddle, {.FixedControls = true});
    EXPECT_TRUE(fixed.Panning);
    EXPECT_FALSE(fixed.IsOrbiting());
}

TEST(SceneViewCameraGestureTests, AltDuringAToolDragChangesNothing)
{
    SceneViewCameraGesture gesture = Press(kLeft, {});
    gesture.UpdateModifiers(true, false, false, false);
    EXPECT_FALSE(gesture.AltOrbiting);
    EXPECT_FALSE(gesture.Panning);
    EXPECT_TRUE(gesture.ToolDrag);
}

TEST(SceneViewCameraGestureTests, ModifiersChangeACameraDrag)
{
    SceneViewCameraGesture orbit = Press(kLeft, {.Alt = true});
    orbit.UpdateModifiers(true, true, false, false);
    EXPECT_TRUE(orbit.Panning);
    EXPECT_FALSE(orbit.AltOrbiting);

    SceneViewCameraGesture look = Press(kRight, {});
    look.UpdateModifiers(true, false, false, false);
    EXPECT_TRUE(look.Dollying);
    EXPECT_FALSE(look.IsLooking());
}

TEST(SceneViewCameraGestureTests, TheMenuCandidateEndsPastTheClickTravel)
{
    SceneViewCameraGesture still = Press(kRight, {});
    still.TrackPointer(kPressPosition + Vector2(SceneViewCameraGesture::kMenuClickTravelPx, 0.0f));
    EXPECT_TRUE(still.MenuCandidate);
    EXPECT_TRUE(still.EndPress(kRight));
    EXPECT_FALSE(still.Dragging);

    SceneViewCameraGesture dragged = Press(kRight, {});
    dragged.TrackPointer(kPressPosition + Vector2(SceneViewCameraGesture::kMenuClickTravelPx + 1.0f, 0.0f));
    EXPECT_FALSE(dragged.MenuCandidate);
    EXPECT_FALSE(dragged.EndPress(kRight));
}

TEST(SceneViewCameraGestureTests, AReleaseOfAnotherButtonKeepsTheDrag)
{
    SceneViewCameraGesture gesture = Press(kRight, {});
    EXPECT_FALSE(gesture.EndPress(kLeft));
    EXPECT_TRUE(gesture.Dragging);
    EXPECT_TRUE(gesture.IsLooking());
}

} // namespace
