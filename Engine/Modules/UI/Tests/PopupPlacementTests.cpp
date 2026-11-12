#include <gtest/gtest.h>

#include "UI/Layout/PopupPlacement.h"

#include "../Source/Layout/TooltipBubblePlacement.h"

namespace
{
using GameEngine::UI::Layout::ClampPopupToViewport;
using GameEngine::UI::Layout::PlacePopupBelowOrAbove;
using GameEngine::UI::Layout::PlaceSubmenuInViewport;
using GameEngine::UI::Layout::PlaceTooltipBubble;
using GameEngine::UI::Layout::PopupPosition;
using GameEngine::UI::Layout::PopupSide;
using GameEngine::UI::Layout::PopupRect;
using GameEngine::UI::Layout::SubmenuAnchor;
using GameEngine::UI::Layout::TooltipReach;
using TooltipPlacement = GameEngine::UIElement::TooltipPlacement;

TEST(PopupPlacementTests, ClampsRootPopupInsideRightAndBottomEdges)
{
    const PopupRect viewport{0.0f, 0.0f, 1280.0f, 800.0f};
    const auto position =
        ClampPopupToViewport(viewport, 1250.0f, 780.0f, 240.0f, 300.0f, 4.0f);

    EXPECT_FLOAT_EQ(position.X, 1036.0f);
    EXPECT_FLOAT_EQ(position.Y, 496.0f);
}

TEST(PopupPlacementTests, ClampsRootPopupInsideLeftAndTopEdges)
{
    const PopupRect viewport{20.0f, 30.0f, 800.0f, 600.0f};
    const auto position =
        ClampPopupToViewport(viewport, -50.0f, -80.0f, 180.0f, 220.0f, 4.0f);

    EXPECT_FLOAT_EQ(position.X, 24.0f);
    EXPECT_FLOAT_EQ(position.Y, 34.0f);
}

// A parent panel 180 px wide at x 300, its parent row's top at y 200; the submenu panel's first
// row sits 5 px below its top edge.
constexpr PopupRect kParentPanel{300.0f, 150.0f, 180.0f, 200.0f};
constexpr float kRowTop = 200.0f;
constexpr float kGap = 2.0f;
constexpr float kFirstRowInset = 5.0f;

bool Overlaps(const PopupRect& a, float x, float y, float width, float height)
{
    return a.X < x + width && x < a.X + a.Width && a.Y < y + height && y < a.Y + a.Height;
}

TEST(PopupPlacementTests, SubmenuOpensPastTheParentPanelsRightEdgeByTheGap)
{
    const PopupRect viewport{0.0f, 0.0f, 1280.0f, 800.0f};
    const auto position = PlaceSubmenuInViewport(
        viewport, SubmenuAnchor{kParentPanel, kRowTop}, 220.0f, 260.0f, 4.0f, kGap, kFirstRowInset);

    EXPECT_FLOAT_EQ(position.X, kParentPanel.X + kParentPanel.Width + kGap);
    EXPECT_FALSE(Overlaps(kParentPanel, position.X, position.Y, 220.0f, 260.0f));
}

TEST(PopupPlacementTests, SubmenuFirstRowIsLevelWithTheParentRow)
{
    const PopupRect viewport{0.0f, 0.0f, 1280.0f, 800.0f};
    const auto position = PlaceSubmenuInViewport(
        viewport, SubmenuAnchor{kParentPanel, kRowTop}, 220.0f, 260.0f, 4.0f, kGap, kFirstRowInset);

    EXPECT_FLOAT_EQ(position.Y + kFirstRowInset, kRowTop);
}

TEST(PopupPlacementTests, SubmenuFlipsLeftOfTheParentPanelAtTheRightEdge)
{
    const PopupRect viewport{0.0f, 0.0f, 600.0f, 800.0f};
    const auto position = PlaceSubmenuInViewport(
        viewport, SubmenuAnchor{kParentPanel, kRowTop}, 220.0f, 260.0f, 4.0f, kGap, kFirstRowInset);

    EXPECT_FLOAT_EQ(position.X + 220.0f, kParentPanel.X - kGap);
    EXPECT_FALSE(Overlaps(kParentPanel, position.X, position.Y, 220.0f, 260.0f));
}

TEST(PopupPlacementTests, SubmenuTakesTheRoomierSideWhenNeitherFitsAndStaysInTheWindow)
{
    // 260 px of room on the left, 100 on the right, a 300 px submenu: left wins and the clamp
    // keeps it inside the margin.
    const PopupRect viewport{0.0f, 0.0f, 586.0f, 800.0f};
    const PopupRect parent{266.0f, 150.0f, 216.0f, 200.0f};
    const auto position = PlaceSubmenuInViewport(
        viewport, SubmenuAnchor{parent, kRowTop}, 300.0f, 260.0f, 4.0f, kGap, kFirstRowInset);

    EXPECT_LT(position.X, parent.X);
    EXPECT_FLOAT_EQ(position.X, 4.0f);
}

TEST(PopupPlacementTests, ClampsSubmenuAboveBottomEdge)
{
    const PopupRect viewport{0.0f, 0.0f, 1280.0f, 800.0f};
    const auto position = PlaceSubmenuInViewport(
        viewport, SubmenuAnchor{kParentPanel, 760.0f}, 220.0f, 260.0f, 4.0f, kGap, kFirstRowInset);

    EXPECT_FLOAT_EQ(position.X, kParentPanel.X + kParentPanel.Width + kGap);
    EXPECT_FLOAT_EQ(position.Y, 536.0f);
}

TEST(PopupPlacementTests, OversizedPopupPinsToViewportMargin)
{
    const PopupRect viewport{0.0f, 0.0f, 100.0f, 80.0f};
    const auto position =
        ClampPopupToViewport(viewport, 50.0f, 40.0f, 200.0f, 160.0f, 4.0f);

    EXPECT_FLOAT_EQ(position.X, 4.0f);
    EXPECT_FLOAT_EQ(position.Y, 4.0f);
}

// A 600 px tall window with a 20 px button; the popup is 200 px tall, 4 px from the button and 8 px
// inside the window edge.
constexpr PopupRect kWindow{0.0f, 0.0f, 800.0f, 600.0f};
constexpr float kButtonHeight = 20.0f;
constexpr float kPopupWidth = 300.0f;
constexpr float kPopupHeight = 200.0f;
constexpr float kMargin = 8.0f;
constexpr float kAnchorGap = 4.0f;

TEST(PopupPlacementTests, APopupOpensBelowItsAnchorWhenItFits)
{
    const PopupRect button{100.0f, 50.0f, 80.0f, kButtonHeight};
    const auto placed =
        PlacePopupBelowOrAbove(kWindow, button, kPopupWidth, kPopupHeight, kMargin, kAnchorGap, std::nullopt);

    EXPECT_EQ(placed.Side, PopupSide::Below);
    EXPECT_FLOAT_EQ(placed.Position.X, button.X);
    EXPECT_FLOAT_EQ(placed.Position.Y, button.Y + kButtonHeight + kAnchorGap);
}

TEST(PopupPlacementTests, APopupOpensAboveItsAnchorAtTheWindowBottom)
{
    const PopupRect button{100.0f, 500.0f, 80.0f, kButtonHeight};
    const auto placed =
        PlacePopupBelowOrAbove(kWindow, button, kPopupWidth, kPopupHeight, kMargin, kAnchorGap, std::nullopt);

    EXPECT_EQ(placed.Side, PopupSide::Above);
    EXPECT_FLOAT_EQ(placed.Position.Y, button.Y - kAnchorGap - kPopupHeight);
}

// Neither side has room for 400 px: the popup takes the side with more room and stays in the window.
TEST(PopupPlacementTests, APopupThatFitsOnNeitherSideTakesTheRoomierSideInsideTheWindow)
{
    constexpr float kTallHeight = 400.0f;
    const PopupRect highButton{100.0f, 250.0f, 80.0f, kButtonHeight};
    const auto below =
        PlacePopupBelowOrAbove(kWindow, highButton, kPopupWidth, kTallHeight, kMargin, kAnchorGap, std::nullopt);
    EXPECT_EQ(below.Side, PopupSide::Below);
    EXPECT_FLOAT_EQ(below.Position.Y, kWindow.Height - kMargin - kTallHeight);

    const PopupRect lowButton{100.0f, 330.0f, 80.0f, kButtonHeight};
    const auto above =
        PlacePopupBelowOrAbove(kWindow, lowButton, kPopupWidth, kTallHeight, kMargin, kAnchorGap, std::nullopt);
    EXPECT_EQ(above.Side, PopupSide::Above);
    EXPECT_GE(above.Position.Y, kMargin);
}

// A popup that opened above keeps that side when it shrinks, so it does not jump below its anchor.
TEST(PopupPlacementTests, APopupKeepsTheSideItWasGiven)
{
    const PopupRect button{100.0f, 500.0f, 80.0f, kButtonHeight};
    constexpr float kShrunkHeight = 40.0f;
    const auto placed =
        PlacePopupBelowOrAbove(kWindow, button, kPopupWidth, kShrunkHeight, kMargin, kAnchorGap, PopupSide::Above);

    EXPECT_EQ(placed.Side, PopupSide::Above);
    EXPECT_FLOAT_EQ(placed.Position.Y, button.Y - kAnchorGap - kShrunkHeight);
}

// A tooltip bubble of 120 x 40 in the 800 x 600 window, 16 px from its anchor on every side.
constexpr float kBubbleWidth = 120.0f;
constexpr float kBubbleHeight = 40.0f;
constexpr TooltipReach kReach{16.0f, 16.0f, 16.0f, 16.0f};
constexpr float kEdgePad = 6.0f;

PopupPosition CenterOf(const PopupRect& rect)
{
    return {rect.X + rect.Width * 0.5f, rect.Y + rect.Height * 0.5f};
}

TEST(PopupPlacementTests, AnAboveTooltipCentersOnItsAnchorWithTheArrowOnTheAnchor)
{
    const PopupRect anchor{380.0f, 300.0f, 40.0f, 20.0f};
    const auto placed = PlaceTooltipBubble(kWindow, anchor, CenterOf(anchor), kBubbleWidth, kBubbleHeight, kReach,
                                           kEdgePad, TooltipPlacement::Above);

    EXPECT_EQ(placed.Side, TooltipPlacement::Above);
    EXPECT_FLOAT_EQ(placed.Position.X, 400.0f - kBubbleWidth * 0.5f);
    EXPECT_FLOAT_EQ(placed.Position.Y, anchor.Y - kReach.Above - kBubbleHeight);
    EXPECT_FLOAT_EQ(placed.ArrowAim, 400.0f);
}

TEST(PopupPlacementTests, AnAboveTooltipAtTheWindowsTopFlipsBelow)
{
    const PopupRect anchor{380.0f, 10.0f, 40.0f, 20.0f};
    const auto placed = PlaceTooltipBubble(kWindow, anchor, CenterOf(anchor), kBubbleWidth, kBubbleHeight, kReach,
                                           kEdgePad, TooltipPlacement::Above);

    EXPECT_EQ(placed.Side, TooltipPlacement::Below);
    EXPECT_FLOAT_EQ(placed.Position.Y, anchor.Y + anchor.Height + kReach.Below);
}

// At the window's right edge the bubble slides left; the arrow stays on the anchor.
TEST(PopupPlacementTests, ATooltipAtTheWindowsRightEdgeSlidesLeftWithTheArrowOnTheAnchor)
{
    const PopupRect anchor{770.0f, 300.0f, 20.0f, 20.0f};
    const auto placed = PlaceTooltipBubble(kWindow, anchor, CenterOf(anchor), kBubbleWidth, kBubbleHeight, kReach,
                                           kEdgePad, TooltipPlacement::Above);

    EXPECT_FLOAT_EQ(placed.Position.X, kWindow.Width - kEdgePad - kBubbleWidth);
    EXPECT_FLOAT_EQ(placed.ArrowAim, 780.0f);
}

TEST(PopupPlacementTests, ATooltipAtTheWindowsLeftEdgeSlidesRightWithTheArrowOnTheAnchor)
{
    const PopupRect anchor{10.0f, 300.0f, 20.0f, 20.0f};
    const auto placed = PlaceTooltipBubble(kWindow, anchor, CenterOf(anchor), kBubbleWidth, kBubbleHeight, kReach,
                                           kEdgePad, TooltipPlacement::Below);

    EXPECT_FLOAT_EQ(placed.Position.X, kEdgePad);
    EXPECT_FLOAT_EQ(placed.ArrowAim, 20.0f);
}

// In the window's top-right corner the bubble flips below and slides left, clear of its anchor.
TEST(PopupPlacementTests, ATooltipInTheWindowsCornerFlipsAndSlidesClearOfItsAnchor)
{
    const PopupRect anchor{770.0f, 4.0f, 26.0f, 20.0f};
    const auto placed = PlaceTooltipBubble(kWindow, anchor, CenterOf(anchor), kBubbleWidth, kBubbleHeight, kReach,
                                           kEdgePad, TooltipPlacement::Above);

    EXPECT_EQ(placed.Side, TooltipPlacement::Below);
    EXPECT_FLOAT_EQ(placed.Position.X, kWindow.Width - kEdgePad - kBubbleWidth);
    EXPECT_GE(placed.Position.Y, anchor.Y + anchor.Height) << "the bubble covers its anchor";
    EXPECT_FLOAT_EQ(placed.ArrowAim, 783.0f);
}

// A tile in the bottom-right corner of a panel, anchored to the panel: the bubble sits above
// the panel's top edge, not over the tiles above the tile, stays inside the panel's span, and
// the arrow points at the tile's column.
TEST(PopupPlacementTests, ATooltipAnchoredToAPanelSitsAboveThePanelInsideItsSpan)
{
    const PopupRect panel{560.0f, 440.0f, 230.0f, 150.0f};
    const PopupRect tile{740.0f, 540.0f, 40.0f, 40.0f};
    constexpr float kWideBubble = 200.0f;
    const auto placed =
        PlaceTooltipBubble(kWindow, panel, CenterOf(tile), kWideBubble, kBubbleHeight, kReach, kEdgePad,
                           TooltipPlacement::Above);

    EXPECT_EQ(placed.Side, TooltipPlacement::Above);
    EXPECT_FLOAT_EQ(placed.Position.Y, panel.Y - kReach.Above - kBubbleHeight);
    EXPECT_FLOAT_EQ(placed.Position.X, panel.X + panel.Width - kWideBubble) << "the bubble leaves the panel's span";
    EXPECT_FLOAT_EQ(placed.ArrowAim, 760.0f);
}

// A bubble wider than its anchor's span centers on the span and stays inside the window.
TEST(PopupPlacementTests, ATooltipWiderThanItsAnchorStaysInsideTheWindow)
{
    const PopupRect panel{560.0f, 440.0f, 230.0f, 150.0f};
    const PopupRect tile{740.0f, 540.0f, 40.0f, 40.0f};
    constexpr float kWiderThanThePanel = 300.0f;
    const auto placed = PlaceTooltipBubble(kWindow, panel, CenterOf(tile), kWiderThanThePanel, kBubbleHeight, kReach,
                                           kEdgePad, TooltipPlacement::Above);

    EXPECT_FLOAT_EQ(placed.Position.X, kWindow.Width - kEdgePad - kWiderThanThePanel);
    EXPECT_FLOAT_EQ(placed.ArrowAim, 760.0f);
}

// Auto sits below; at the window's bottom it sits above; with room on neither side it takes the roomier one.
TEST(PopupPlacementTests, AnAutoTooltipSitsBelowElseAboveElseOnTheRoomierSide)
{
    const PopupRect middle{380.0f, 300.0f, 40.0f, 20.0f};
    EXPECT_EQ(PlaceTooltipBubble(kWindow, middle, CenterOf(middle), kBubbleWidth, kBubbleHeight, kReach, kEdgePad,
                                 TooltipPlacement::Auto)
                  .Side,
              TooltipPlacement::Below);

    const PopupRect bottom{380.0f, 570.0f, 40.0f, 20.0f};
    EXPECT_EQ(PlaceTooltipBubble(kWindow, bottom, CenterOf(bottom), kBubbleWidth, kBubbleHeight, kReach, kEdgePad,
                                 TooltipPlacement::Auto)
                  .Side,
              TooltipPlacement::Above);

    constexpr float kTallBubble = 320.0f;
    const PopupRect low{380.0f, 340.0f, 40.0f, 20.0f};
    const auto placed = PlaceTooltipBubble(kWindow, low, CenterOf(low), kBubbleWidth, kTallBubble, kReach, kEdgePad,
                                           TooltipPlacement::Auto);
    EXPECT_EQ(placed.Side, TooltipPlacement::Above);
    EXPECT_FLOAT_EQ(placed.Position.Y, kEdgePad);
}

TEST(PopupPlacementTests, ARightTooltipAtTheWindowsRightEdgeFlipsLeft)
{
    const PopupRect anchor{720.0f, 300.0f, 40.0f, 20.0f};
    const auto placed = PlaceTooltipBubble(kWindow, anchor, CenterOf(anchor), kBubbleWidth, kBubbleHeight, kReach,
                                           kEdgePad, TooltipPlacement::Right);

    EXPECT_EQ(placed.Side, TooltipPlacement::Left);
    EXPECT_FLOAT_EQ(placed.Position.X, anchor.X - kReach.Left - kBubbleWidth);
    EXPECT_FLOAT_EQ(placed.Position.Y, 310.0f - kBubbleHeight * 0.5f);
    EXPECT_FLOAT_EQ(placed.ArrowAim, 310.0f);
}

// A bubble of fractional width at its panel's right bound starts on the last whole pixel that keeps
// its right edge inside the panel.
TEST(PopupPlacementTests, ATooltipAtItsPanelsRightBoundStartsOnAWholePixelInsideThePanel)
{
    const PopupRect panel{300.0f, 400.0f, 200.0f, 150.0f};
    const PopupRect tile{480.0f, 500.0f, 20.0f, 20.0f};
    constexpr float kFractionalWidth = 120.3f;
    const auto placed = PlaceTooltipBubble(kWindow, panel, CenterOf(tile), kFractionalWidth, kBubbleHeight, kReach,
                                           kEdgePad, TooltipPlacement::Above);

    EXPECT_FLOAT_EQ(placed.Position.X, 379.0f);
    EXPECT_FLOAT_EQ(placed.ArrowAim, 490.0f);
}

// A scaled layout puts the panel on a fractional x. A bubble as wide as the panel starts on the
// nearest whole pixel of the panel's start for every tile, and the arrow points at each tile.
TEST(PopupPlacementTests, ATooltipAsWideAsItsPanelAtAFractionalXKeepsThePanelsSpanForEveryTile)
{
    const PopupRect panel{139.4f, 440.0f, 210.0f, 150.0f};
    const PopupRect firstTile{144.4f, 450.0f, 40.0f, 40.0f};
    const PopupRect lastTile{304.4f, 450.0f, 40.0f, 40.0f};
    const float bubbleWidth = panel.Width;
    const auto overFirst = PlaceTooltipBubble(kWindow, panel, CenterOf(firstTile), bubbleWidth, kBubbleHeight, kReach,
                                              kEdgePad, TooltipPlacement::Above);
    const auto overLast = PlaceTooltipBubble(kWindow, panel, CenterOf(lastTile), bubbleWidth, kBubbleHeight, kReach,
                                             kEdgePad, TooltipPlacement::Above);

    EXPECT_FLOAT_EQ(overFirst.Position.X, 139.0f) << "the bubble leaves the panel's span";
    EXPECT_FLOAT_EQ(overLast.Position.X, 139.0f) << "the bubble leaves the panel's span";
    EXPECT_FLOAT_EQ(overFirst.ArrowAim, 164.4f);
    EXPECT_FLOAT_EQ(overLast.ArrowAim, 324.4f);
}

// A bubble one pixel wider than its panel centers on the panel for every tile, not on the tile.
TEST(PopupPlacementTests, ATooltipOnePixelWiderThanItsPanelCentersOnThePanelForEveryTile)
{
    const PopupRect panel{300.0f, 440.0f, 200.0f, 150.0f};
    const PopupRect firstTile{300.0f, 450.0f, 20.0f, 20.0f};
    const PopupRect lastTile{480.0f, 450.0f, 20.0f, 20.0f};
    const float bubbleWidth = panel.Width + 1.0f;
    const auto overFirst = PlaceTooltipBubble(kWindow, panel, CenterOf(firstTile), bubbleWidth, kBubbleHeight, kReach,
                                              kEdgePad, TooltipPlacement::Above);
    const auto overLast = PlaceTooltipBubble(kWindow, panel, CenterOf(lastTile), bubbleWidth, kBubbleHeight, kReach,
                                             kEdgePad, TooltipPlacement::Above);

    EXPECT_NEAR(overFirst.Position.X + bubbleWidth * 0.5f, 400.0f, 0.5f) << "the bubble centers on the tile";
    EXPECT_FLOAT_EQ(overLast.Position.X, overFirst.Position.X) << "the bubble moves with the tile";
    EXPECT_FLOAT_EQ(overFirst.ArrowAim, 310.0f);
    EXPECT_FLOAT_EQ(overLast.ArrowAim, 490.0f);
}

// A bubble twice as wide as its panel centers on the panel for every tile.
TEST(PopupPlacementTests, ATooltipTwiceAsWideAsItsPanelCentersOnThePanelForEveryTile)
{
    const PopupRect panel{300.0f, 440.0f, 100.0f, 150.0f};
    const float bubbleWidth = panel.Width * 2.0f;
    for (const float tileX : {300.0f, 380.0f})
    {
        const PopupRect tile{tileX, 450.0f, 20.0f, 20.0f};
        const auto placed = PlaceTooltipBubble(kWindow, panel, CenterOf(tile), bubbleWidth, kBubbleHeight, kReach,
                                               kEdgePad, TooltipPlacement::Above);
        EXPECT_FLOAT_EQ(placed.Position.X, 250.0f) << "the bubble leaves the center of the panel";
    }
}

// A Left tooltip anchored to a tall panel stays inside the panel's vertical span.
TEST(PopupPlacementTests, ALeftTooltipAnchoredToATallPanelStaysInsideItsSpan)
{
    const PopupRect panel{600.0f, 100.0f, 150.0f, 400.0f};
    const PopupRect tile{610.0f, 480.0f, 20.0f, 20.0f};
    const auto placed = PlaceTooltipBubble(kWindow, panel, CenterOf(tile), kBubbleWidth, kBubbleHeight, kReach,
                                           kEdgePad, TooltipPlacement::Left);

    EXPECT_EQ(placed.Side, TooltipPlacement::Left);
    EXPECT_FLOAT_EQ(placed.Position.X, panel.X - kReach.Left - kBubbleWidth);
    EXPECT_FLOAT_EQ(placed.Position.Y, panel.Y + panel.Height - kBubbleHeight) << "the bubble leaves the panel's span";
    EXPECT_FLOAT_EQ(placed.ArrowAim, 490.0f);
}

} // namespace
