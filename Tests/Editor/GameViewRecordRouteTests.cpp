#include "GameView/GameViewController.h"

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{
using Route = GameViewController::RecordRoute;

GameViewController::RecordWindowState MakeInputs(bool panelVisible)
{
    GameViewController::RecordWindowState inputs{};
    inputs.PanelPresent = true;
    inputs.PanelVisible = panelVisible;
    return inputs;
}
} // namespace

// The default layout opens with the Scene View tab active: the hidden Game View is
// never recorded, before or after the window shows.
TEST(GameViewRecordRouteTests, AHiddenPanelIsNotRecorded)
{
    const GameViewController controller{nullptr, nullptr};
    EXPECT_EQ(controller.ChooseRecordRoute(MakeInputs(false)), Route::Quiet);
}

// A layout with the Game View tab active, or a play-mode start that shows the tab,
// records the view on its first visible frame.
TEST(GameViewRecordRouteTests, AVisiblePanelIsRecordedOnItsFirstFrame)
{
    const GameViewController controller{nullptr, nullptr};
    EXPECT_EQ(controller.ChooseRecordRoute(MakeInputs(true)), Route::Visible);
}

TEST(GameViewRecordRouteTests, NoPanelRecordsNothing)
{
    const GameViewController controller{nullptr, nullptr};
    EXPECT_EQ(controller.ChooseRecordRoute(GameViewController::RecordWindowState{}), Route::None);
}
