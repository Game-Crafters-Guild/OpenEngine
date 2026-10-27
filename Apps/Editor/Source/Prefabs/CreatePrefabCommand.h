#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "Scene/PrefabAuthoring.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;

/// The undo step of Create Prefab: the file written from a subtree, and the subtree made an instance in
/// place. Undo reverts the instance and deletes the file, keeps it when a scene saved since uses it, or
/// writes back the file it replaced. Redo writes the file again under the same GUID.
class CreatePrefabCommand final : public IEditorCommand
{
    // Only Create can name this type, so only Create constructs a command.
    struct ConstructionKey
    {
        explicit ConstructionKey() = default;
    };

  public:
    /// Creates the prefab of the subtree under `root` at `prefabFile`. A file at that path is
    /// replaced only when `replace` is true and no instance in `world` uses it; its bytes are kept for
    /// the undo. Null, with `outError` stating the cause and the fix, when nothing was created.
    static std::unique_ptr<CreatePrefabCommand> Create(ECS::World& world, EditorChangeNotifications* notifications,
                                                       ECS::EntityHandle root, const std::filesystem::path& prefabFile,
                                                       bool replace, std::string& outError);

    /// Why the file at `prefabFile` cannot be replaced: the instances in `world` that use it, with the
    /// fix. Empty when a Replace can overwrite it.
    static std::string DescribeReplaceRefusal(ECS::World& world, const std::filesystem::path& prefabFile);

    const char* GetName() const override { return m_Name.c_str(); }
    const char* GetTypeName() const override { return "CreatePrefabCommand"; }
    void Do() override {}
    void Undo() override;
    void Redo() override;

    const GUID& GetPrefab() const { return m_Prefab; }

    CreatePrefabCommand(ConstructionKey key, ECS::World& world, EditorChangeNotifications* notifications,
                        ECS::EntityHandle root, std::filesystem::path prefabFile);

  private:
    // Removes the file at the prefab's path and keeps its bytes in m_ReplacedBytes. False, with
    // `outError`, when the file cannot be read or removed.
    bool SetReplacedFileAside(std::string& outError);
    void WriteReplacedFileBack() const;
    // Deletes the new file, or keeps it with a notice when a scene saved since the Create uses it.
    void RemoveFileUnlessASceneUsesIt();

    std::string m_Name;
    ECS::World& m_World;
    EditorChangeNotifications* m_Notifications = nullptr;
    ECS::EntityHandle m_Root;
    std::filesystem::path m_PrefabFile;
    GUID m_Prefab;
    std::vector<Scene::PrefabIdentityBefore> m_MembersBefore;
    std::optional<std::string> m_ReplacedBytes;
    std::filesystem::file_time_type m_WrittenAt{};
    bool m_FileKept = false;
};

} // namespace GameEngine::Editor
