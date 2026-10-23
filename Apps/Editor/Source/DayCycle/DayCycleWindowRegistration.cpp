#include "DayCycle/DayCycleWindowRegistration.h"

#include "DayCycle/DayCycleEditorServices.h"
#include "DayCycle/DayCycleWindow.h"
#include "DayCycle/DayCycleWorkingCopy.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "EditorChangeNotifications.h"

#include <memory>
#include <utility>

namespace GameEngine::Editor
{

void RegisterDayCycleWindow(UndoRedoService* undo, EditorChangeNotifications& notifications,
                            std::function<std::vector<ECS::EntityHandle>()> selection,
                            std::function<bool()> isPlaying)
{
    // The editor's notifications outlive every registration: the subscription is never ended.
    DayCycleWorkingCopy::CloseAllOnProjectSwitch(notifications);

    EditorPanelDescriptor descriptor;
    descriptor.PanelId = kDayCycleWindowPanelId;
    descriptor.Title = "Day Cycle";
    descriptor.Factory = [undo, notifications = &notifications, selection = std::move(selection),
                          isPlaying = std::move(isPlaying)]() -> std::unique_ptr<UIElement> {
        return std::make_unique<DayCycleWindow>(EditorDayCycleServices(undo, notifications, selection, isPlaying));
    };
    EditorPanelRegistry::Get().RegisterPanel(std::move(descriptor));
}

} // namespace GameEngine::Editor
