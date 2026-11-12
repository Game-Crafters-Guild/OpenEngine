#pragma once

#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"

#include "ECS/Entity.h"
#include "ECS/World.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

/// Undo step for a measure the Scene View created: the measure root and its two endpoints.
/// Records the entities after they exist; Undo destroys them and Redo revives them with their captured components.
class CreateMeasureEntitiesCommand final : public IEditorCommand
{
public:
    CreateMeasureEntitiesCommand(ECS::World* world,
                                 EditorChangeNotifications* notifications,
                                 std::vector<ECS::EntityHandle> entities)
        : m_World(world)
        , m_Notifications(notifications)
        , m_Entities(std::move(entities))
    {
        CaptureSnapshots();
    }

    const char* GetName() const override { return "Create Measure"; }
    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World)
            return;
        for (std::size_t i = m_Snapshots.size(); i-- > 0;)
        {
            if (m_Snapshots[i].Entity.IsValid())
                m_World->DestroyEntityImmediatePreserveHandle(m_Snapshots[i].Entity);
        }
        NotifyWorldStructure(m_Notifications, m_World);
    }

    void Redo() override
    {
        if (!m_World)
            return;
        for (const EntitySnapshot& snap : m_Snapshots)
        {
            if (!snap.Entity.IsValid())
                continue;
            if (!m_World->IsValid(snap.Entity))
                (void)m_World->ReviveEntityImmediatePreserveHandle(snap.Entity);
            if (!m_World->IsValid(snap.Entity))
                continue;
            for (const auto& [typeId, bytes] : snap.Components)
                (void)m_World->ApplyComponentBytesImmediate(snap.Entity, typeId, bytes);
        }
        NotifyWorldStructure(m_Notifications, m_World);
    }

private:
    struct EntitySnapshot
    {
        ECS::EntityHandle Entity{};
        std::vector<std::pair<ECS::ComponentTypeId, std::vector<std::uint8_t>>> Components;
    };

    void CaptureSnapshots()
    {
        m_Snapshots.clear();
        if (!m_World)
            return;

        for (ECS::EntityHandle entity : m_Entities)
        {
            if (!entity.IsValid() || !m_World->IsValid(entity))
                continue;

            ECS::Archetype* archetype = m_World->GetEntityArchetype(entity);
            if (!archetype)
                continue;

            EntitySnapshot snap{};
            snap.Entity = entity;
            const auto typeIds = archetype->GetSignature().GetComponents();
            snap.Components.reserve(typeIds.size());
            for (ECS::ComponentTypeId typeId : typeIds)
            {
                std::vector<std::uint8_t> bytes;
                if (m_World->CaptureComponentBytes(entity, typeId, bytes))
                    snap.Components.emplace_back(typeId, std::move(bytes));
            }
            m_Snapshots.push_back(std::move(snap));
        }
    }

    ECS::World* m_World = nullptr;
    EditorChangeNotifications* m_Notifications = nullptr;
    std::vector<ECS::EntityHandle> m_Entities;
    std::vector<EntitySnapshot> m_Snapshots;
};

} // namespace GameEngine::Editor
