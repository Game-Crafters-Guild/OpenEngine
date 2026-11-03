#pragma once

#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"

#include "ECS/Entity.h"

#include <string>
#include <vector>
#include <cstdint>

namespace GameEngine::Editor
{

// Undo/Redo command that swaps the entire ECS World state using the built-in
// binary snapshot path (World::SerializeWorld / DeserializeWorld).
class WorldSnapshotCommand final : public IEditorCommand
{
  public:
    WorldSnapshotCommand(std::string name,
                         ECS::World* world,
                         EditorChangeNotifications* notifications,
                         std::vector<std::uint8_t> before,
                         std::vector<std::uint8_t> after)
        : m_Name(std::move(name)),
          m_World(world),
          m_Notifications(notifications),
          m_Before(std::move(before)),
          m_After(std::move(after))
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }
    const char* GetTypeName() const override { return "WorldSnapshotCommand"; }

    void Do() override { Redo(); }

    void Undo() override { if (TryUndo()) NotifyReplay(); }
    void Redo() override { if (TryRedo()) NotifyReplay(); }
    bool TryUndo() override { return Apply(m_Before); }
    bool TryRedo() override { return Apply(m_After); }

    void NotifyReplay() override
    {
        if (m_Notifications)
        {
            EditorChangeNotifications::WorldStructureChangedEvent e{};
            e.world = m_World;
            e.kind = EditorChangeNotifications::ChangeKind::UndoRedo;
            m_Notifications->NotifyWorldStructureChanged(e);
        }
    }

  private:
    bool Apply(const std::vector<std::uint8_t>& snapshot)
    {
        if (!m_World)
            return false;
        m_World->DeserializeWorld(snapshot);
        m_World->ProcessCommands();
        return true;
    }

    std::string m_Name;
    ECS::World* m_World = nullptr; // not owned
    EditorChangeNotifications* m_Notifications = nullptr; // not owned
    std::vector<std::uint8_t> m_Before;
    std::vector<std::uint8_t> m_After;
};

} // namespace GameEngine::Editor

