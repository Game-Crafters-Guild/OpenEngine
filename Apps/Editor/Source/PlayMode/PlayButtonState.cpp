#include "PlayMode/PlayButtonState.h"

#include "PlayMode/PlayModeManager.h"
#include "UI/Controls/Button.h"

namespace GameEngine::Editor
{

void ApplyPlayButtonState(Button& playButton, PlayModeState state)
{
    const bool clickable =
        state == PlayModeState::Edit || state == PlayModeState::Play || state == PlayModeState::Paused;
    const bool playActive = state == PlayModeState::Play || state == PlayModeState::Paused ||
                            state == PlayModeState::EnteringPlay || state == PlayModeState::ExitingPlay;
    playButton.SetDisabled(!clickable);
    if (playActive)
        playButton.AddClass(kPlayButtonActiveClass);
    else
        playButton.RemoveClass(kPlayButtonActiveClass);
}

} // namespace GameEngine::Editor
