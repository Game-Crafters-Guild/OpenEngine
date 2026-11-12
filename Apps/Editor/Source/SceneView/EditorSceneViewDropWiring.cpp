#include "SceneView/EditorSceneViewDropWiring.h"

#include "Panels/HierarchyPanel.h"
#include "SceneView/SceneViewPanel.h"
#include "SceneView/SceneViewport/SceneViewportEdits.h"

#include "ECS/Entity.h"

#include <utility>
#include <vector>

namespace GameEngine::Editor
{

void WireSceneViewAssetDropUndo(SceneViewPanel* sceneViewPanel, HierarchyPanel* hierarchyPanel)
{
    if (!sceneViewPanel || !hierarchyPanel)
        return;

    sceneViewPanel->SetCaptureHierarchySelectionForDrop(
        [hierarchyPanel]()
        {
            return std::make_pair(hierarchyPanel->GetSelectionItemIds(), hierarchyPanel->GetSelectionAnchor());
        });

    sceneViewPanel->SetOnSceneAssetDropComplete(
        [hierarchyPanel](SceneViewportEdits& edits,
                         const std::vector<ECS::EntityHandle>& droppedRoots,
                         const std::vector<UI::Interaction::ItemId>& selBefore,
                         UI::Interaction::ItemId anchorBefore)
        {
            if (droppedRoots.empty())
                return;

            hierarchyPanel->Refresh();
            hierarchyPanel->SelectEntities(droppedRoots);

            const SceneViewportEdits::DropSelection before{selBefore, anchorBefore};
            const SceneViewportEdits::DropSelection after{hierarchyPanel->GetSelectionItemIds(),
                                                          hierarchyPanel->GetSelectionAnchor()};
            edits.RecordDrop(droppedRoots,
                             before,
                             after,
                             [hierarchyPanel](const std::vector<UI::Interaction::ItemId>& ids, UI::Interaction::ItemId anchor)
                             { hierarchyPanel->ApplySelectionProgrammatic(ids, anchor); });
        });
}

} // namespace GameEngine::Editor
