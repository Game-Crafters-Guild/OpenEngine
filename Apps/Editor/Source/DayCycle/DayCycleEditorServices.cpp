#include "DayCycle/DayCycleEditorServices.h"

#include "Core/Engine.h"
#include "ECS/Systems.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"

#include <utility>

namespace GameEngine::Editor
{

DayCycleEditorServices EditorDayCycleServices(UndoRedoService* undo, EditorChangeNotifications* notifications,
                                              std::function<std::vector<ECS::EntityHandle>()> selection,
                                              std::function<bool()> isPlaying)
{
    DayCycleEditorServices services;
    services.Assets = EngineCore::GetInstance().TryGetAssetManager();
    services.Undo = undo;
    services.Notifications = notifications;
    services.World = []() { return EngineCore::GetInstance().GetPrimaryWorld(); };
    services.Render = []() { return EngineCore::GetInstance().GetRenderServices(); };
    services.Sky = []() -> Engine::Renderer::SkyEnvironmentSystem* {
        auto* loop = EngineCore::GetInstance().GetRenderingLoop();
        auto* systems = loop ? loop->GetSystemManager() : nullptr;
        return systems ? systems->GetSystem<Engine::Renderer::SkyEnvironmentSystem>() : nullptr;
    };
    services.Selection = std::move(selection);
    services.IsPlaying = std::move(isPlaying);
    return services;
}

} // namespace GameEngine::Editor
