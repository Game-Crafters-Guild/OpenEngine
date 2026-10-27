#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "ECS/ECS.h"
#include "Scene/PrefabAuthoring.h"

#include <memory>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;

/// The undo step of Unpack and Unpack Completely: the instance at a root made plain entities with the
/// values it shows. Undo gives back the instance marker and the scene identities; Redo unpacks again.
class UnpackPrefabCommand final : public IEditorCommand
{
    // Only Create can name this type, so only Create constructs a command.
    struct ConstructionKey
    {
        explicit ConstructionKey() = default;
    };

  public:
    /// Unpacks the instance at `root` to `depth`. Null, with `outError` stating the cause and the fix,
    /// when nothing was unpacked.
    static std::unique_ptr<UnpackPrefabCommand> Create(ECS::World& world, EditorChangeNotifications* notifications,
                                                       ECS::EntityHandle root, Scene::PrefabUnpackDepth depth,
                                                       std::string& outError);

    const char* GetName() const override { return m_Name.c_str(); }
    const char* GetTypeName() const override { return "UnpackPrefabCommand"; }
    void Do() override {}
    void Undo() override;
    void Redo() override;

    UnpackPrefabCommand(ConstructionKey key, ECS::World& world, EditorChangeNotifications* notifications,
                        ECS::EntityHandle root, Scene::PrefabUnpackDepth depth);

  private:
    std::string m_Name;
    ECS::World& m_World;
    EditorChangeNotifications* m_Notifications = nullptr;
    ECS::EntityHandle m_Root;
    Scene::PrefabUnpackDepth m_Depth = Scene::PrefabUnpackDepth::Outer;
    std::vector<Scene::PrefabIdentityBefore> m_Before;
};

} // namespace GameEngine::Editor
