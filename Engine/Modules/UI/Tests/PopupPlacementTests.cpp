#include <gtest/gtest.h>

#include "UI/Layout/PopupPlacement.h"

namespace
{
using GameEngine::UI::Layout::ClampPopupToViewport;
using GameEngine::UI::Layout::PlaceSubmenuInViewport;
using GameEngine::UI::Layout::PopupRect;
using GameEngine::UI::Layout::SubmenuAnchor;

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

} // namespace
