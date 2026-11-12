#include "Systems/AiKnowledgeSystem.h"

#include "AIECS/AiRegistry.h"
#include "AIECS/Components/AiClock.h"
#include "AiGroup/AiGroupStore.h"

#include <algorithm>

namespace GameEngine::AIECS
{

namespace
{

// The group's records, or null when a sensor removed the group. Found again before each use,
// because a removal can free the entry.
AI::RecordStore* FindRecords(AiGroupStore& store, ECS::EntityHandle entity, const Components::AiGroup& group)
{
    AiGroupStore::Group* found = store.Find(entity, group.Knowledge);
    return found != nullptr ? &found->Records : nullptr;
}

} // namespace

void AiKnowledgeSystem::Update(ECS::World& world, float32)
{
    const Components::AiClock* clock = world.GetSingleton<Components::AiClock>();
    if (clock == nullptr || !clock->Due)
        return;

    // A sensor may change the world, so the groups are collected before any sensor runs.
    m_Entries.clear();
    world.Query<ECS::Read<Components::AiGroup>>().Each(
        [this](ECS::EntityHandle entity, const Components::AiGroup& group) { m_Entries.push_back({entity, group}); });
    if (m_Entries.empty())
        return;
    std::sort(m_Entries.begin(), m_Entries.end(),
              [](const GroupEntry& left, const GroupEntry& right) { return left.Entity < right.Entity; });
    // The hold keeps the store when a sensor removes the last group.
    const AiGroupStore::WorldHold hold(world.GetWorldId());
    AiGroupStore& store = hold.GetStore();
    PrepareGroups(world, store);

    for (const GroupEntry& entry : m_Entries)
    {
        for (AiSensorFunction sensor : m_Registry.GetSensors())
        {
            if (sensor == nullptr)
                continue;
            AI::RecordStore* records = FindRecords(store, entry.Entity, entry.Group);
            if (records == nullptr)
                break;
            sensor(world, entry.Entity, *records, clock->Tick);
        }
        if (AI::RecordStore* records = FindRecords(store, entry.Entity, entry.Group))
            records->Forget(clock->Tick);
    }
}

void AiKnowledgeSystem::PrepareGroups(ECS::World& world, AiGroupStore& store)
{
    for (GroupEntry& entry : m_Entries)
    {
        if (store.Find(entry.Entity, entry.Group.Knowledge) == nullptr)
        {
            entry.Group.Knowledge = store.Create(entry.Entity);
            world.GetComponentForWrite<Components::AiGroup>(entry.Entity)->Knowledge = entry.Group.Knowledge;
        }
        AiGroupStore::Group& group = *store.Find(entry.Entity, entry.Group.Knowledge);
        group.Records.Capacity = entry.Group.RecordCapacity;
        group.Records.ForgetTicks = entry.Group.ForgetTicks;
        group.Records.ClearOnSight = entry.Group.ClearOnSight;
        group.Plan.resize(entry.Group.PlanCells, 0u);
    }
}

} // namespace GameEngine::AIECS
