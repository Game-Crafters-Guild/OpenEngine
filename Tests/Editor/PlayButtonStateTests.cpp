// The top toolbar Play button keeps its play-active look through a click during play.
// It stays clickable while play mode runs, so its right-click menu (Audio Enabled) is
// reachable, and Button adds and removes `pressed` on every left click: the active
// state rides a class of its own, or the first click during play turned the green
// icon off while the game kept running.

#include <gtest/gtest.h>

#include "PlayMode/PlayButtonState.h"
#include "PlayMode/PlayModeManager.h"
#include "UI/Controls/Button.h"
#include "UI/UIEvents.h"

using namespace GameEngine;

namespace
{

void LeftClick(Button& button)
{
    UIEvent down{};
    down.Id = kEventMouseDown;
    down.Button = 0;
    button.OnEvent(down);
    UIEvent up{};
    up.Id = kEventMouseUp;
    up.Button = 0;
    button.OnEvent(up);
}

} // namespace

TEST(PlayButtonState, TheActiveLookSurvivesALeftClickDuringPlay)
{
    Button play;
    Editor::ApplyPlayButtonState(play, Editor::PlayModeState::Play);
    ASSERT_TRUE(play.HasClass(Editor::kPlayButtonActiveClass));
    ASSERT_TRUE(play.IsEnabled()) << "the button stays clickable during play so its menu is reachable";

    LeftClick(play);

    EXPECT_TRUE(play.HasClass(Editor::kPlayButtonActiveClass))
        << "a click during play turned the Play button's active look off while play mode runs";
}

TEST(PlayButtonState, EditModeClearsTheActiveLook)
{
    Button play;
    Editor::ApplyPlayButtonState(play, Editor::PlayModeState::Paused);
    ASSERT_TRUE(play.HasClass(Editor::kPlayButtonActiveClass));

    Editor::ApplyPlayButtonState(play, Editor::PlayModeState::Edit);

    EXPECT_FALSE(play.HasClass(Editor::kPlayButtonActiveClass));
    EXPECT_TRUE(play.IsEnabled());
}
