#pragma once

#include <functional>
#include <memory>
#include <vector>

namespace GameEngine::ECS { class SystemManager; class World; }

namespace GameEngine::Editor
{

class IEditorService;
class UndoRedoService;

/// What a service factory gets when the editor's system schedule starts.
struct EditorServiceContext
{
    UndoRedoService* Undo = nullptr;
    ECS::SystemManager& Systems;
};

/// The editor services: each is made when the system schedule starts, released at each scene
/// swap and destroyed, in reverse order, before the schedule stops. It starts with the editor's own.
class EditorServiceRegistry
{
public:
    /// Returns the service, or null when the process has nothing for it to serve.
    using Factory = std::function<std::unique_ptr<IEditorService>(const EditorServiceContext&)>;

    EditorServiceRegistry();
    ~EditorServiceRegistry();
    EditorServiceRegistry(const EditorServiceRegistry&) = delete;
    EditorServiceRegistry& operator=(const EditorServiceRegistry&) = delete;

    void Start(const EditorServiceContext& context);
    void ReleaseSceneScoped(ECS::World& world);
    void Stop();

private:
    void Register(Factory factory);

    std::vector<Factory> m_Factories;
    std::vector<std::unique_ptr<IEditorService>> m_Services;
};

} // namespace GameEngine::Editor
