#include "SceneView/MountedViewPanels.h"

#include "EditorPanelIds.h"
#include "Panels/GameViewPanel.h"
#include "SceneView/SceneViewPanel.h"

namespace GameEngine::Editor
{
namespace
{

constexpr StringId kSceneViewMountId = HashStringId(EditorPanelIds::MountSceneView);
constexpr StringId kGameViewMountId = HashStringId(EditorPanelIds::MountGameView);

} // namespace

SceneViewPanel* MountedViewPanels::SceneView(UIManager* ui)
{
    Mount* mount = m_SceneViewMount.Find(ui, kSceneViewMountId);
    return mount ? dynamic_cast<SceneViewPanel*>(mount->GetTarget()) : nullptr;
}

GameViewPanel* MountedViewPanels::GameView(UIManager* ui)
{
    Mount* mount = m_GameViewMount.Find(ui, kGameViewMountId);
    return mount ? dynamic_cast<GameViewPanel*>(mount->GetTarget()) : nullptr;
}

} // namespace GameEngine::Editor
