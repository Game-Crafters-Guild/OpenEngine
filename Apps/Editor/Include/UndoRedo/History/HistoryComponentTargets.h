#pragma once

#include "EditorChangeNotifications.h"
#include "UndoRedo/History/HistorySnapshotTarget.h"

#include "ECS/Entity.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
/// Main-thread component targets. Every destination is validated before restoration writes any bytes.
class HistoryComponentTargets
{
public:
    using Snapshot = HistorySnapshotTarget::Snapshot;
    HistoryComponentTargets(std::function<ECS::World*()> resolveWorld, std::vector<ECS::EntityHandle> entities,
                             ECS::ComponentTypeId typeId, EditorChangeNotifications* notifications)
        : m_ResolveWorld(std::move(resolveWorld)), m_Entities(std::move(entities)),
          m_TypeId(typeId), m_Notifications(notifications)
    {
        ECS::World* world = m_ResolveWorld ? m_ResolveWorld() : nullptr;
        if (!world)
        {
            m_Entities.clear();
            return;
        }
        std::sort(m_Entities.begin(), m_Entities.end());
        m_Entities.erase(std::unique(m_Entities.begin(), m_Entities.end()), m_Entities.end());
        std::erase_if(m_Entities, [world, typeId](auto entity) { return !world->HasComponent(entity, typeId); });
        m_WorldId = world->GetWorldId();
        m_HandleEpoch = world->GetEntityHandleEpoch();
        m_Identity = {m_WorldId, m_HandleEpoch, typeId};
        for (const auto entity : m_Entities)
            m_Identity.push_back(entity.id);
    }
    const std::vector<std::uint64_t>& GetIdentity() const { return m_Identity; }
    bool IsValid() const
    {
        ECS::World* world = ResolveWorld();
        return world && !m_Entities.empty() && std::all_of(m_Entities.begin(), m_Entities.end(), [this, world](auto entity)
        {
            return world->IsValid(entity) && world->HasComponent(entity, m_TypeId);
        });
    }
    bool Capture(Snapshot& output) const
    {
        ECS::World* world = ResolveWorld();
        if (!IsValid())
            return false;
        output.clear();
        const auto count = static_cast<std::uint32_t>(m_Entities.size());
        output.resize(sizeof(count));
        std::memcpy(output.data(), &count, sizeof(count));
        for (const auto entity : m_Entities)
        {
            Snapshot bytes;
            if (!world->CaptureComponentBytes(entity, m_TypeId, bytes))
                return false;
            const auto size = static_cast<std::uint32_t>(bytes.size());
            const std::size_t offset = output.size();
            output.resize(offset + sizeof(size) + bytes.size());
            std::memcpy(output.data() + offset, &size, sizeof(size));
            std::memcpy(output.data() + offset + sizeof(size), bytes.data(), bytes.size());
        }
        return true;
    }
    bool Restore(const Snapshot& input) const
    {
        ECS::World* world = ResolveWorld();
        if (!world || input.size() < sizeof(std::uint32_t))
            return false;
        std::uint32_t count = 0;
        std::memcpy(&count, input.data(), sizeof(count));
        if (count != m_Entities.size())
            return false;
        std::size_t offset = sizeof(count);
        std::vector<Snapshot> before(count);
        std::vector<Snapshot> after(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            if (input.size() - offset < sizeof(std::uint32_t))
                return false;
            std::uint32_t size = 0;
            std::memcpy(&size, input.data() + offset, sizeof(size));
            offset += sizeof(size);
            if (size > input.size() - offset ||
                !world->CaptureComponentBytes(m_Entities[index], m_TypeId, before[index]) ||
                before[index].size() != size)
                return false;
            after[index].assign(input.begin() + static_cast<std::ptrdiff_t>(offset),
                input.begin() + static_cast<std::ptrdiff_t>(offset + size));
            offset += size;
        }
        if (offset != input.size())
            return false;
        for (std::size_t index = 0; index < count; ++index)
        {
            if (world->ApplyComponentBytesImmediate(m_Entities[index], m_TypeId, after[index]))
                continue;
            while (index > 0)
            {
                --index;
                world->ApplyComponentBytesImmediate(m_Entities[index], m_TypeId, before[index]);
            }
            return false;
        }
        return true;
    }
    void Notify(EditChangeKind kind) const
    {
        ECS::World* world = ResolveWorld();
        if (world && m_Notifications)
            for (const auto entity : m_Entities)
                m_Notifications->NotifyComponentChanged({world, entity, m_TypeId, kind});
    }

private:
    ECS::World* ResolveWorld() const
    {
        const auto epoch = ECS::World::FindLiveEntityHandleEpoch(m_WorldId);
        if (!epoch || *epoch != m_HandleEpoch)
            return nullptr;
        return m_ResolveWorld ? m_ResolveWorld() : nullptr;
    }
    std::uint64_t m_WorldId = 0;
    std::uint64_t m_HandleEpoch = 0;
    std::function<ECS::World*()> m_ResolveWorld;
    std::vector<ECS::EntityHandle> m_Entities;
    ECS::ComponentTypeId m_TypeId;
    EditorChangeNotifications* m_Notifications;
    std::vector<std::uint64_t> m_Identity;
};
} // namespace GameEngine::Editor
