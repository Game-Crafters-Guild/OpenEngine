#pragma once

#include "AI/RecordStore.h"
#include "ECS/ECS.h"
#include "GenerationalVector/GenerationalVector.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::AIECS
{

/// The records and plan cells of the AI groups of one world, keyed by AiGroup::Knowledge. Only the
/// world's main thread changes it; AiDecideSystem's tasks read it on job threads while no other
/// system runs. It lives from the world's first group to its last.
class AiGroupStore
{
public:
    struct Group
    {
        /// The group entity that owns the entry. A copy of its AiGroup names the entry but does not own it.
        ECS::EntityHandle Owner;
        AI::RecordStore Records;
        std::vector<uint32> Plan;
    };

    /// Holds the store of a world for its lifetime: HoldForWorld when made, ReturnForWorld when it
    /// goes out of scope, also when a sensor throws.
    class WorldHold
    {
    public:
        explicit WorldHold(uint64 worldId) : m_WorldId(worldId), m_Store(HoldForWorld(worldId)) {}
        ~WorldHold() { ReturnForWorld(m_WorldId); }
        WorldHold(const WorldHold&) = delete;
        WorldHold& operator=(const WorldHold&) = delete;

        AiGroupStore& GetStore() const { return m_Store; }

    private:
        uint64 m_WorldId = 0;
        AiGroupStore& m_Store;
    };

    AiGroupStore() = default;
    AiGroupStore(const AiGroupStore&) = delete;
    AiGroupStore& operator=(const AiGroupStore&) = delete;

    /// The store of the world, or null when the world has no group knowledge.
    static AiGroupStore* FindForWorld(uint64 worldId);
    /// Frees the entry when `entity` owns it. The last entry of a world frees the store unless it is held.
    static void ReleaseInWorld(uint64 worldId, ECS::EntityHandle entity, GenerationalVector::Handle knowledge);

    /// Adds an empty entry that `owner` owns.
    GenerationalVector::Handle Create(ECS::EntityHandle owner);
    /// Adds an empty entry that `owner` owns to the store of the world, and makes the store when it is missing.
    static GenerationalVector::Handle CreateInWorld(uint64 worldId, ECS::EntityHandle owner);
    /// The entry `knowledge` names when `entity` owns it, else null.
    Group* Find(ECS::EntityHandle entity, GenerationalVector::Handle knowledge);
    const Group* Find(ECS::EntityHandle entity, GenerationalVector::Handle knowledge) const;
    /// The number of entries.
    uint32 GetCount() const { return static_cast<uint32>(m_Groups.Size()); }

private:
    // The store of the world, created when missing. It stays until ReturnForWorld, also with no entry.
    static AiGroupStore& HoldForWorld(uint64 worldId);
    // Ends the hold of HoldForWorld. A store with no entry is freed.
    static void ReturnForWorld(uint64 worldId);

    GenerationalVector::GenerationalVector<Group> m_Groups;
    bool m_Held = false;
};

} // namespace GameEngine::AIECS
