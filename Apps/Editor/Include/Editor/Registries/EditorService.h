#pragma once

namespace GameEngine::ECS { class World; }

namespace GameEngine::Editor
{

/// An editor-side object that lives while the editor's system schedule runs
/// (EditorServiceRegistry). The editor never names a service.
class IEditorService
{
public:
    virtual ~IEditorService() = default;

    /// Releases what the service holds in the outgoing scene's world, at a scene swap.
    virtual void ReleaseSceneScoped(ECS::World& world) = 0;
};

} // namespace GameEngine::Editor
