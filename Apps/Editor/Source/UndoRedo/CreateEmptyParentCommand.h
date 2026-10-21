#pragma once

#include <string>
#include <utility>
#include <vector>

#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/ReparentReorderEntitiesCommand.h"

#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

namespace GameEngine::Editor
{

// Undo of "Create Empty Parent": one new entity, and the hierarchy moves that put
// it in place and the grouped entities under it. The moves' states include the
// new parent's own Parent, HierarchyOrder and Transform, so redo needs only to
// revive it with its name before re-applying them.
class CreateEmptyParentCommand final : public IEditorCommand
{
  public:
    CreateEmptyParentCommand(std::string name,
                             ECS::World* world,
                             EditorChangeNotifications* notifications,
                             ECS::EntityHandle newParent,
                             Components::Name parentName,
                             std::vector<HierarchyEntityState> before,
                             std::vector<HierarchyEntityState> after)
        : m_Name(name)
        , m_World(world)
        , m_Notifications(notifications)
        , m_NewParent(newParent)
        , m_ParentName(parentName)
        , m_Moves(std::move(name), world, notifications, std::move(before), std::move(after))
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World || !m_World->IsValid(m_NewParent))
            return;
        // The grouped entities go back first so none references the parent being destroyed.
        m_Moves.Undo();
        m_World->DestroyEntityImmediatePreserveHandle(m_NewParent);
        NotifyWorldStructure(m_Notifications, m_World);
    }

    void Redo() override
    {
        if (!m_World || !m_World->ReviveEntityImmediatePreserveHandle(m_NewParent))
            return;
        m_World->AddComponentImmediate(m_NewParent, m_ParentName);
        m_Moves.Redo();
    }

  private:
    std::string m_Name;
    ECS::World* m_World = nullptr;                       // not owned
    EditorChangeNotifications* m_Notifications = nullptr; // not owned
    ECS::EntityHandle m_NewParent{};
    Components::Name m_ParentName{};
    ReparentReorderEntitiesCommand m_Moves;
};

} // namespace GameEngine::Editor
