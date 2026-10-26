#pragma once

// The prefab library: one private world that holds a template of every resident prefab, resolved
// once from its file with its nesting and variant chain applied. A placed instance and a spawn are
// batch clones of a template (World::CreateBatchFromWorld). The engine owns one (EngineCore,
// beside its AssetManager); a scene load off the main thread or with no engine builds one of its
// own for that load.
//
// Thread rule: written on the main thread only, at scene load, at PrefabRef resolution and at
// asset reload completion, never while systems run. A spawn reads it with no lock.

#include "AssetCore/GUID.h"
#include "ECS/ClonePlan.h"
#include "ECS/ECS.h"
#include "Events/Event.h"
#include "Scene/SceneIO.h"
#include "Types/Types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Scene
{

// One prefab resolved into the library world.
struct PrefabTemplate
{
    // A load-report row of the template's build, replayed for every placed instance: its entity
    // id is relative to the instance root, and EntityIndex names the template entity it was
    // recorded on (-1 for none).
    struct Row
    {
        SceneLoadSkip Skip;
        std::int32_t EntityIndex = -1;
    };
    // Where an entity is authored: Files[File], at the entity's header line.
    struct Origin
    {
        std::uint32_t File = 0;
        int Line = 0;
    };

    std::filesystem::path Path; // absolute, lexically normal
    GUID Source{};              // the prefab asset's GUID; null when no resolver named it
    std::uint64_t Generation = 0;

    // The template's entities in template order (file order), the root first, with the address of
    // each under the root (the root's is empty) and where each is authored.
    std::vector<ECS::EntityHandle> Entities;
    std::vector<std::string> Addresses;
    std::vector<Origin> Origins;
    std::vector<Row> Rows;

    // The clone plan of Entities, built when the generation is inserted: every placement and
    // spawn of this generation copies through it.
    ECS::ClonePlan Plan;

    // Every file the build read, Path first, then its nested prefabs and variant bases: a change
    // to any of them rebuilds the template.
    std::vector<std::filesystem::path> Files;

    // The root has no authored Name: a placed instance names it after the instance id.
    bool RootTakesFallbackName = false;
};

class PrefabLibrary
{
  public:
    /// A library for one scene load.
    PrefabLibrary();
    /// The engine's library: it resolves prefab GUIDs through `assetManager` and rebuilds a
    /// resident template when a file it was built from changes.
    explicit PrefabLibrary(AssetManager& assetManager);
    ~PrefabLibrary();
    PrefabLibrary(const PrefabLibrary&) = delete;
    PrefabLibrary& operator=(const PrefabLibrary&) = delete;

    /// The world the templates live in. Templates are built in it directly.
    ECS::World& GetWorld() { return *m_World; }
    const ECS::World& GetWorld() const { return *m_World; }

    /// The newest generation of the prefab at `path` (absolute, lexically normal), or null.
    const PrefabTemplate* FindByPath(const std::filesystem::path& path) const;
    /// The newest generation of the prefab asset `source`, or null when it is not resident.
    const PrefabTemplate* FindByGuid(const GUID& source) const;

    /// Adds a template built in GetWorld() and plans its clone. A prefab already resident gets a
    /// new generation; the old one stays valid until the next CollectAtFrameBoundary.
    const PrefabTemplate& Insert(PrefabTemplate built);

    /// Keeps `prefab` resident while `world` lives and has not been cleared since.
    void Hold(const ECS::World& world, const PrefabTemplate& prefab);
    /// Drops every hold of `world`.
    void ReleaseWorld(const ECS::World& world);

    /// The frame boundary: drops the holds of destroyed or cleared worlds, frees retired
    /// generations (no holder of a retired generation exists) and evicts every prefab no world holds.
    void CollectAtFrameBoundary();

    /// Rebuilds, as a new generation, every resident template built from a file `changed` names.
    void RebuildChanged(std::span<const GUID> changed);

    /// Logs, once per GUID, that a spawn named the prefab `source` while it is not resident, with
    /// the fix. Any thread.
    void ReportNotResident(const GUID& source) const;

    /// The number of worlds that hold the prefab at `path`.
    std::uint32_t GetReferenceCount(const std::filesystem::path& path) const;
    std::size_t GetResidentCount() const { return m_Records.size(); }
    std::size_t GetRetiredCount() const;

  private:
    struct Record
    {
        std::unique_ptr<PrefabTemplate> Current;
        std::vector<std::unique_ptr<PrefabTemplate>> Retired;
        std::uint32_t References = 0;
    };
    struct WorldHolds
    {
        std::uint64_t Epoch = 0;
        std::vector<std::string> Keys;
    };

    void AssertWritable() const;
    void ReleaseHolds(WorldHolds& holds);
    void DestroyTemplate(const PrefabTemplate& prefab);

    std::unique_ptr<ECS::World> m_World;
    FastHashMap<std::string, std::unique_ptr<Record>> m_Records; // by Path.generic_string()
    FastHashMap<GUID, std::string> m_KeyBySource;
    FastHashMap<std::uint64_t, WorldHolds> m_HoldsByWorld;

    AssetManager* m_AssetManager = nullptr; // set for the engine's library
    EventSubscription m_FilesChanged;

    mutable std::mutex m_NotResidentMutex;
    mutable FastHashSet<GUID> m_ReportedNotResident;
};

} // namespace GameEngine::Scene
