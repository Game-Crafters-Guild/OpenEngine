#include "AiGroup/AiGroupStore.h"

#include "Types/FlatMap.h"

#include <memory>
#include <mutex>
#include <utility>

namespace GameEngine::AIECS
{

namespace
{

// The stores of the worlds by world id. A world id is never used again, so a key names one world.
// The lock covers only the map. AiDecideSystem finds its world's store once per update, so the
// tasks read a store with no lock.
struct WorldStores
{
    std::mutex Mutex;
    FlatMap<uint64, std::unique_ptr<AiGroupStore>> Stores;
};

WorldStores& GetWorldStores()
{
    static WorldStores stores;
    return stores;
}

// The store of the world, made when missing. The caller holds worlds.Mutex.
AiGroupStore& FindOrMakeLocked(WorldStores& worlds, uint64 worldId)
{
    std::unique_ptr<AiGroupStore>* found = worlds.Stores.Find(worldId);
    if (found == nullptr)
        found = &worlds.Stores.InsertOrAssign(worldId, std::make_unique<AiGroupStore>());
    return **found;
}

} // namespace

AiGroupStore* AiGroupStore::FindForWorld(uint64 worldId)
{
    WorldStores& worlds = GetWorldStores();
    std::lock_guard lock(worlds.Mutex);
    std::unique_ptr<AiGroupStore>* found = worlds.Stores.Find(worldId);
    return found != nullptr ? found->get() : nullptr;
}

AiGroupStore& AiGroupStore::HoldForWorld(uint64 worldId)
{
    WorldStores& worlds = GetWorldStores();
    std::lock_guard lock(worlds.Mutex);
    AiGroupStore& store = FindOrMakeLocked(worlds, worldId);
    store.m_Held = true;
    return store;
}

void AiGroupStore::ReturnForWorld(uint64 worldId)
{
    WorldStores& worlds = GetWorldStores();
    std::lock_guard lock(worlds.Mutex);
    std::unique_ptr<AiGroupStore>* found = worlds.Stores.Find(worldId);
    if (found == nullptr)
        return;
    (*found)->m_Held = false;
    if ((*found)->m_Groups.Empty())
        worlds.Stores.Erase(worldId);
}

void AiGroupStore::ReleaseInWorld(uint64 worldId, ECS::EntityHandle entity, GenerationalVector::Handle knowledge)
{
    WorldStores& worlds = GetWorldStores();
    std::lock_guard lock(worlds.Mutex);
    std::unique_ptr<AiGroupStore>* found = worlds.Stores.Find(worldId);
    if (found == nullptr || (*found)->Find(entity, knowledge) == nullptr)
        return;
    (*found)->m_Groups.Destroy(knowledge);
    if ((*found)->m_Groups.Empty() && !(*found)->m_Held)
        worlds.Stores.Erase(worldId);
}

GenerationalVector::Handle AiGroupStore::CreateInWorld(uint64 worldId, ECS::EntityHandle owner)
{
    WorldStores& worlds = GetWorldStores();
    std::lock_guard lock(worlds.Mutex);
    return FindOrMakeLocked(worlds, worldId).Create(owner);
}

GenerationalVector::Handle AiGroupStore::Create(ECS::EntityHandle owner)
{
    Group group;
    group.Owner = owner;
    return m_Groups.Create(std::move(group));
}

AiGroupStore::Group* AiGroupStore::Find(ECS::EntityHandle entity, GenerationalVector::Handle knowledge)
{
    Group* group = m_Groups.Get(knowledge);
    return group != nullptr && group->Owner == entity ? group : nullptr;
}

const AiGroupStore::Group* AiGroupStore::Find(ECS::EntityHandle entity, GenerationalVector::Handle knowledge) const
{
    const Group* group = m_Groups.Get(knowledge);
    return group != nullptr && group->Owner == entity ? group : nullptr;
}

} // namespace GameEngine::AIECS
