#include "EditorApplication.h"

#include "PlayMode/PlayModeManager.h"
#include "UI/Controls/EditorTopToolbar.h"

namespace GameEngine
{

void EditorApplication::RequestGameExit(int)
{
    if (!m_PlayMode || (!m_PlayMode->IsPlayingOrPaused() &&
                        m_PlayMode->GetState() != Editor::PlayModeState::EnteringPlay))
        return;

    // A script can request exit from OnCreate or OnUpdate. Restore the edit world
    // after those callbacks return, through the same transition as the Stop button.
    m_DeferredPreUiActions.EnqueueUnique("editor.game-exit", [this]()
    {
        if (!m_PlayMode || !m_PlayMode->IsPlayingOrPaused())
            return;

        m_PlayMode->ExitPlayMode();
        ExitPlayFullscreen();
        if (m_TopToolbar)
            m_TopToolbar->SetFullscreenActive(m_PlayFullscreenOnEnter);
    });
}

} // namespace GameEngine
