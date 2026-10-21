#include "SceneView/MountedViewPanels.h"

#include "EditorPanelIds.h"
#include "Panels/GameViewPanel.h"
#include "Panels/SceneViewPanel.h"
#include "UI/Controls/Mount.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

namespace GameEngine::Editor
{
namespace
{

template <typename TPanel>
TPanel* MountedPanel(UIManager* ui, const char* mountId)
{
    if (!ui)
        return nullptr;
    UIElement* root = ui->GetRootElement();
    if (!root)
        return nullptr;
    if (auto* mount = dynamic_cast<Mount*>(root->FindById(HashStringId(mountId))))
        return dynamic_cast<TPanel*>(mount->GetTarget());
    return nullptr;
}

} // namespace

SceneViewPanel* MountedViewPanels::SceneView(UIManager* ui)
{
    return MountedPanel<SceneViewPanel>(ui, EditorPanelIds::MountSceneView);
}

GameViewPanel* MountedViewPanels::GameView(UIManager* ui)
{
    return MountedPanel<GameViewPanel>(ui, EditorPanelIds::MountGameView);
}

} // namespace GameEngine::Editor
