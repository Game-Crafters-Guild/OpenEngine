#include "Scene/PrefabOverrideRecords.h"

#include "ECS/Entity.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <optional>

namespace GameEngine::Scene
{

namespace
{

std::string LineKeyOf(std::string_view component, std::string_view field)
{
    std::string key(component);
    return key.append(1, '.').append(field);
}

} // namespace

PrefabOverrideRecords& PrefabOverrideRecords::Get()
{
    static PrefabOverrideRecords s_Records;
    return s_Records;
}

PrefabOverrideRecords::WorldRecords& PrefabOverrideRecords::RecordsOf(const ECS::World& world)
{
    const std::uint64_t worldId = world.GetWorldId();
    if (!m_ByWorld.contains(worldId))
    {
        // A new world: drop the records of every world that is gone or was cleared.
        std::erase_if(m_ByWorld,
                      [](const auto& entry)
                      {
                          const std::optional<std::uint64_t> epoch = ECS::World::FindLiveEntityHandleEpoch(entry.first);
                          return !epoch || *epoch != entry.second.Epoch;
                      });
    }
    WorldRecords& records = m_ByWorld[worldId];
    if (records.Epoch != world.GetEntityHandleEpoch())
    {
        records.Epoch = world.GetEntityHandleEpoch();
        records.KeysByEntity.clear();
    }
    return records;
}

bool PrefabOverrideRecords::Record(const ECS::World& world, ECS::EntityHandle entity, std::string_view component,
                                   std::string_view field)
{
    std::string key = LineKeyOf(component, field);
    std::lock_guard lock(m_Mutex);
    std::vector<std::string>& keys = RecordsOf(world).KeysByEntity[entity];
    if (std::any_of(keys.begin(), keys.end(), [&key](const std::string& held) { return EqualsIgnoreCase(held, key); }))
        return false;
    keys.push_back(std::move(key));
    return true;
}

bool PrefabOverrideRecords::Forget(const ECS::World& world, ECS::EntityHandle entity, std::string_view component,
                                   std::string_view field)
{
    const std::string key = field.empty() ? std::string(component) + "." : LineKeyOf(component, field);
    std::lock_guard lock(m_Mutex);
    WorldRecords& records = RecordsOf(world);
    const auto it = records.KeysByEntity.find(entity);
    if (it == records.KeysByEntity.end())
        return false;
    if (component.empty())
    {
        records.KeysByEntity.erase(it);
        return true;
    }
    const std::size_t dropped =
        std::erase_if(it->second, [&key, &field](const std::string& held)
                      { return field.empty() ? StartsWithIgnoreCase(held, key) : EqualsIgnoreCase(held, key); });
    if (it->second.empty())
        records.KeysByEntity.erase(it);
    return dropped > 0;
}

std::vector<std::string> PrefabOverrideRecords::Find(const ECS::World& world, ECS::EntityHandle entity) const
{
    std::lock_guard lock(m_Mutex);
    const auto worldIt = m_ByWorld.find(world.GetWorldId());
    if (worldIt == m_ByWorld.end() || worldIt->second.Epoch != world.GetEntityHandleEpoch())
        return {};
    const auto it = worldIt->second.KeysByEntity.find(entity);
    return it == worldIt->second.KeysByEntity.end() ? std::vector<std::string>{} : it->second;
}

std::string_view PrefabOverrideRecords::LineKey(std::string_view line)
{
    const std::vector<std::string_view> fields = Split(line, '=');
    return fields.empty() ? std::string_view() : TrimWhitespaceView(fields.front());
}

bool PrefabOverrideRecords::HoldsLine(std::span<const std::string> keys, std::string_view line)
{
    if (keys.empty())
        return false;
    const std::string_view key = LineKey(line);
    return std::any_of(keys.begin(), keys.end(), [key](const std::string& held) { return EqualsIgnoreCase(held, key); });
}

} // namespace GameEngine::Scene
