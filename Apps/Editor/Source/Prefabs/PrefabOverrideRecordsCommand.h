#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "Scene/PrefabAuthoring.h"

#include <span>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;

/// The record half of an operation on prefab overrides: the records it added and the records it dropped. Undo drops
/// the added records and gives back the dropped ones; Redo does the reverse. Each announces the components of the
/// fields as an UndoRedo change. The operation's component edit is a step of its own.
class PrefabOverrideRecordsCommand final : public IEditorCommand
{
  public:
    /// The records are already added and dropped (Scene::RecordPrefabOverrides and Scene::ForgetPrefabOverrides
    /// return them).
    PrefabOverrideRecordsCommand(ECS::World& world, EditorChangeNotifications* notifications,
                                 std::span<const Scene::PrefabOverrideTarget> added,
                                 std::span<const Scene::PrefabOverrideTarget> dropped, std::string name);

    const char* GetName() const override { return m_Name.c_str(); }
    const char* GetTypeName() const override { return "PrefabOverrideRecordsCommand"; }
    void Do() override {}
    void Undo() override;
    void Redo() override;

  private:
    void NotifyTargets(std::span<const Scene::PrefabOverrideTarget> targets) const;

    std::string m_Name;
    ECS::World& m_World;
    EditorChangeNotifications* m_Notifications = nullptr;
    std::vector<Scene::PrefabOverrideTarget> m_Added;
    std::vector<Scene::PrefabOverrideTarget> m_Dropped;
};

} // namespace GameEngine::Editor
