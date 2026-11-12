#pragma once

// The fields the user set on the entities of prefab instances, kept for each world: a recorded field is
// an override even when its value equals the prefab's (design prefabs-a 2.6). Private to the scene IO
// sources.

#include "ECS/ECS.h"
#include "Types/Types.h"

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Scene
{

// A record is the key of a "Component.field" line of the scene saver, compared without case. A clear of
// the world drops its records. Any thread.
class PrefabOverrideRecords
{
  public:
    static PrefabOverrideRecords& Get();

    /// Records the field `field` of the component `component` (its scene token) on `entity`. False when the record
    /// was there before.
    bool Record(const ECS::World& world, ECS::EntityHandle entity, std::string_view component, std::string_view field);
    /// Drops the record of `field` of `component` on `entity`. An empty `field` drops every field of `component`; an
    /// empty `component` drops every record of `entity`. False when no record was dropped.
    bool Forget(const ECS::World& world, ECS::EntityHandle entity, std::string_view component, std::string_view field);
    /// The line keys recorded on `entity`; empty when it has none.
    std::vector<std::string> Find(const ECS::World& world, ECS::EntityHandle entity) const;

    /// The key of a saver line "Component.field = value": the text before the first '=', without the spaces
    /// around it. A key has no '=', so a value with one keeps its key.
    static std::string_view LineKey(std::string_view line);
    /// True when `keys` (from Find) holds the key of the saver line `line`.
    static bool HoldsLine(std::span<const std::string> keys, std::string_view line);

  private:
    struct WorldRecords
    {
        std::uint64_t Epoch = 0;
        FastHashMap<ECS::EntityHandle, std::vector<std::string>, ECS::EntityHandleHash> KeysByEntity;
    };

    // The records of `world` for its current epoch, made empty when the world was cleared. Caller holds the lock.
    WorldRecords& RecordsOf(const ECS::World& world);

    mutable std::mutex m_Mutex;
    FastHashMap<std::uint64_t, WorldRecords> m_ByWorld; // by world id
};

} // namespace GameEngine::Scene
