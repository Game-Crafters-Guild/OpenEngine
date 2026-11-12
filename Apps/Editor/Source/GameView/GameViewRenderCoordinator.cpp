#include "GameView/GameViewRenderCoordinator.h"

#include "Core/Engine.h"
#include "GameView/GameViewController.h"
#include "Panels/GameViewPanel.h"
#include "SceneView/MountedViewPanels.h"
#include "SceneView/SceneViewRenderCoordinator.h"

#include <cstdint>

namespace GameEngine::Editor
{

void GameViewRenderCoordinator::RequestFrameBeforeExtraction(EditorWindowContext& ctx)
{
    if (!ctx.gameView)
        return;
    const auto& movie = ctx.gameView->GetMovieCapture();
    uint32_t width = 0;
    uint32_t height = 0;
    bool active = movie.IsSessionOpen();
    if (active)
    {
        width = movie.GetWidth();
        height = movie.GetHeight();
    }
    else if (GameViewPanel* panel = ctx.mountedViewPanels.GameView(ctx.ui.get()))
    {
        active = SceneViewRenderCoordinator::TryResolveRenderableViewportSize(
            ctx.ui.get(), panel->GetViewportElement(), width, height);
    }
    ctx.gameView->RequestFrameWhenActive(EngineCore::GetInstance().EnsurePrimaryWorld(), active,
                                         width, height);
}

} // namespace GameEngine::Editor
