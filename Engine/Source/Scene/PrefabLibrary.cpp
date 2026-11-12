#include "Scene/PrefabLibrary.h"

#include "AssetCore/PathNormalization.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "ECS/UnresolvedComponentStore.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Scene/AssetManagerSceneResolver.h"
#include "Scene/PrefabPlacement.h"
#include "Scene/SceneEngineEmbedMaterializer.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scripting/AbiMainThread.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::Scene
{

namespace
{
// True when `prefab` read one of `changedFiles`, which are registry keys.
bool ReadsAnyOf(const PrefabTemplate& prefab, std::span<const std::string> changedFiles)
{
    for (const std::filesystem::path& file : prefab.Files)
    {
        const std::string key = AssetPaths::NormalizeForRegistryKey(file);
        if (std::find(changedFiles.begin(), changedFiles.end(), key) != changedFiles.end())
            return true;
    }
    return false;
}

// True when an entity of `prefab` keeps, in `kept`, a component of a type that is registered now.
bool KeepsARegisteredType(const PrefabTemplate& prefab, const ECS::UnresolvedComponentStore& kept)
{
    for (const ECS::EntityHandle entity : prefab.Entities)
    {
        const auto it = kept.Map().find(entity);
        if (it == kept.Map().end())
            continue;
        for (const ECS::PreservedComponent& component : it->second)
        {
            if (SceneSchemaRegistry::Find(component.Name))
                return true;
        }
    }
    return false;
}

// The record key of a prefab file: the registry's folded key, so every spelling of one file finds one record.
std::string RecordKey(const std::filesystem::path& path)
{
    return AssetPaths::NormalizeForRegistryKey(path);
}
} // namespace

PrefabLibrary::PrefabLibrary()
    : m_World(std::make_unique<ECS::World>())
{
}

PrefabLibrary::PrefabLibrary(AssetManager& assetManager)
    : m_World(std::make_unique<ECS::World>()), m_AssetManager(&assetManager)
{
    m_FilesChanged = assetManager.FilesChanged.Subscribe([this](std::span<const GUID> changed)
                                                         { RebuildChanged(changed); });
}

PrefabLibrary::~PrefabLibrary() = default;

void PrefabLibrary::AssertWritable() const
{
    assert((!m_AssetManager || ScriptingAbi::IsMainThreadOutsideJobFor(EngineCore::GetInstance())) &&
           "The engine's prefab library is written on the main thread only, never while systems run");
}

const PrefabTemplate* PrefabLibrary::FindByPath(const std::filesystem::path& path) const
{
    const auto it = m_Records.find(RecordKey(path));
    return it == m_Records.end() ? nullptr : it->second->Current.get();
}

const PrefabTemplate* PrefabLibrary::FindByGuid(const GUID& source) const
{
    if (source.IsNull())
        return nullptr;
    const auto key = m_KeyBySource.find(source);
    if (key == m_KeyBySource.end())
        return nullptr;
    const auto it = m_Records.find(key->second);
    return it == m_Records.end() ? nullptr : it->second->Current.get();
}

const PrefabTemplate& PrefabLibrary::Insert(PrefabTemplate built)
{
    AssertWritable();
    assert(!built.Entities.empty() && built.Entities.size() == built.Addresses.size() &&
           "A template has its root and an address per entity");
    const std::string key = RecordKey(built.Path);
    std::unique_ptr<Record>& record = m_Records[key];
    if (!record)
        record = std::make_unique<Record>();
    if (record->Current)
    {
        built.Generation = record->Current->Generation + 1;
        record->Retired.push_back(std::move(record->Current));
    }
    if (!built.Source.IsNull())
        m_KeyBySource[built.Source] = key;
    [[maybe_unused]] const bool planned = m_World->PlanClone(built.Entities, built.Plan);
    assert(planned && "A template's entities are alive in the library world");
    record->Current = std::make_unique<PrefabTemplate>(std::move(built));
    return *record->Current;
}

void PrefabLibrary::Hold(ECS::World& world, const PrefabTemplate& prefab)
{
    AssertWritable();
    WorldHolds& holds = m_HoldsByWorld[world.GetWorldId()];
    holds.World = &world;
    if (holds.Epoch != world.GetEntityHandleEpoch())
    {
        ReleaseHolds(holds);
        holds.Epoch = world.GetEntityHandleEpoch();
    }
    std::string key = RecordKey(prefab.Path);
    if (std::any_of(holds.Prefabs.begin(), holds.Prefabs.end(),
                    [&key](const HeldPrefab& held)
                    { return held.Key == key; }))
        return;
    const auto it = m_Records.find(key);
    assert(it != m_Records.end() && "Hold names a resident prefab");
    if (it == m_Records.end())
        return;
    ++it->second->References;
    holds.Prefabs.push_back({std::move(key), &prefab});
}

void PrefabLibrary::ReleaseWorld(const ECS::World& world)
{
    AssertWritable();
    const auto it = m_HoldsByWorld.find(world.GetWorldId());
    if (it == m_HoldsByWorld.end())
        return;
    ReleaseHolds(it->second);
    m_HoldsByWorld.erase(it);
}

void PrefabLibrary::ReleaseHolds(WorldHolds& holds)
{
    for (const HeldPrefab& held : holds.Prefabs)
    {
        const auto it = m_Records.find(held.Key);
        if (it != m_Records.end() && it->second->References > 0)
            --it->second->References;
    }
    holds.Prefabs.clear();
}

void PrefabLibrary::CollectAtFrameBoundary()
{
    AssertWritable();
    for (auto it = m_HoldsByWorld.begin(); it != m_HoldsByWorld.end();)
    {
        const std::optional<std::uint64_t> epoch = ECS::World::FindLiveEntityHandleEpoch(it->first);
        if (epoch && *epoch == it->second.Epoch)
        {
            ++it;
            continue;
        }
        ReleaseHolds(it->second);
        it = m_HoldsByWorld.erase(it);
    }
    std::vector<std::uint64_t> destroyed;
    for (const std::uint64_t worldId : m_DeferredWorlds)
    {
        if (!ECS::World::FindLiveEntityHandleEpoch(worldId))
            destroyed.push_back(worldId);
    }
    for (const std::uint64_t worldId : destroyed)
        m_DeferredWorlds.Erase(worldId);

    // A deferred world's instances still show a retired generation: its resume compares with it.
    // A frame with no retired generation builds no set.
    FastHashSet<const PrefabTemplate*> shown;
    const bool anyRetired = std::any_of(m_Records.begin(), m_Records.end(),
                                        [](const auto& entry)
                                        { return !entry.second->Retired.empty(); });
    if (anyRetired)
    {
        for (const auto& [worldId, holds] : m_HoldsByWorld)
        {
            for (const HeldPrefab& held : holds.Prefabs)
                shown.insert(held.Shown);
        }
    }
    for (auto it = m_Records.begin(); it != m_Records.end();)
    {
        Record& record = *it->second;
        std::erase_if(record.Retired,
                      [this, &shown](const std::unique_ptr<PrefabTemplate>& retired)
                      {
                          if (shown.contains(retired.get()))
                              return false;
                          DestroyTemplate(*retired);
                          return true;
                      });
        if (record.References > 0)
        {
            ++it;
            continue;
        }
        if (!record.Current->Source.IsNull())
            m_KeyBySource.erase(record.Current->Source);
        DestroyTemplate(*record.Current);
        it = m_Records.erase(it);
    }
}

void PrefabLibrary::RebuildChanged(std::span<const GUID> changed)
{
    AssertWritable();
    if (!m_AssetManager || m_Records.empty())
        return;
    AssetRegistry& registry = m_AssetManager->GetRegistry();
    std::vector<std::string> changedFiles;
    for (const GUID& guid : changed)
    {
        AssetMetadata metadata{};
        if (registry.TryGetAssetMetadata(guid, metadata))
            changedFiles.push_back(AssetPaths::NormalizeForRegistryKey(metadata.Path));
    }
    RebuildKeys(changedFiles);
}

void PrefabLibrary::RebuildFiles(std::span<const std::filesystem::path> files)
{
    AssertWritable();
    if (!m_AssetManager || m_Records.empty())
        return;
    std::vector<std::string> changedFiles;
    for (const std::filesystem::path& file : files)
        changedFiles.push_back(AssetPaths::NormalizeForRegistryKey(file));
    RebuildKeys(changedFiles);
}

void PrefabLibrary::RebuildForRegisteredTypes()
{
    AssertWritable();
    const ECS::UnresolvedComponentStore* kept = m_World->TryGetUnresolvedComponents();
    if (!m_AssetManager || !kept || kept->Empty())
        return;
    std::vector<const PrefabTemplate*> stale;
    for (const auto& [key, record] : m_Records)
    {
        if (KeepsARegisteredType(*record->Current, *kept))
            stale.push_back(record->Current.get());
    }
    RebuildTemplates(stale);
}

void PrefabLibrary::SetPropagationDeferred(ECS::World& world, bool deferred)
{
    AssertWritable();
    if (deferred)
    {
        m_DeferredWorlds.Insert(world.GetWorldId());
        return;
    }
    m_DeferredWorlds.Erase(world.GetWorldId());
    const auto it = m_HoldsByWorld.find(world.GetWorldId());
    if (it == m_HoldsByWorld.end())
        return;
    it->second.World = &world;
    if (PropagateToHolder(it->first, it->second))
        InstancesChanged.Invoke(world);
}

bool PrefabLibrary::PropagateToHolder(std::uint64_t worldId, WorldHolds& holds)
{
    if (ECS::World::FindLiveEntityHandleEpoch(worldId) != holds.Epoch)
        return false;
    bool changed = false;
    for (std::size_t index = 0; index < holds.Prefabs.size(); ++index)
    {
        const auto record = m_Records.find(holds.Prefabs[index].Key);
        if (record == m_Records.end() || holds.Prefabs[index].Shown == record->second->Current.get())
            continue;
        const PrefabTemplate& before = *holds.Prefabs[index].Shown;
        const PrefabTemplate& after = *record->second->Current;
        holds.Prefabs[index].Shown = &after;
        changed = PropagatePrefabGeneration(*holds.World, *this, before, after) || changed;
    }
    return changed;
}

void PrefabLibrary::RebuildKeys(std::span<const std::string> changedFiles)
{
    std::vector<const PrefabTemplate*> stale;
    for (const auto& [key, record] : m_Records)
    {
        if (ReadsAnyOf(*record->Current, changedFiles))
            stale.push_back(record->Current.get());
    }
    RebuildTemplates(stale);
}

void PrefabLibrary::RebuildTemplates(std::span<const PrefabTemplate* const> stale)
{
    if (stale.empty())
        return;

    EngineCore& engine = EngineCore::GetInstance();
    AssetManagerSceneResolver resolver(*m_AssetManager, engine.GetResolvedAssetRoot(),
                                       AssetManagerSceneResolver::UnknownFile::AssignGuid);
    SceneEngineEmbedMaterializer materializer(*m_AssetManager);
    SceneLoadOptions options;
    options.Read.AssetResolver = &resolver;
    options.Read.AssetRootOverride = resolver.GetAssetRoot();
    options.Read.ProjectRoot = engine.GetWorkspaceRoot();
    options.EmbedMaterializer = &materializer;
    for (const PrefabTemplate* prefab : stale)
    {
        // A rebuild retires the old generation, which stays alive while a world's instances show it.
        const std::filesystem::path& path = prefab->Path;
        std::string error;
        if (!BuildTemplateFromFile(*this, path, prefab->Source, options, error))
            Logger::Log::Error("Prefab {} could not be rebuilt, so spawns and loads still use the previous version: {}",
                               path.filename().string(), error);
    }

    // A propagation can hold prefabs, which adds holders: walk a copy of the world ids.
    std::vector<std::uint64_t> worldIds;
    worldIds.reserve(m_HoldsByWorld.size());
    for (const auto& [worldId, holds] : m_HoldsByWorld)
    {
        if (!m_DeferredWorlds.Contains(worldId))
            worldIds.push_back(worldId);
    }
    std::vector<ECS::World*> changedWorlds;
    for (const std::uint64_t worldId : worldIds)
    {
        const auto it = m_HoldsByWorld.find(worldId);
        if (it != m_HoldsByWorld.end() && PropagateToHolder(worldId, it->second))
            changedWorlds.push_back(it->second.World);
    }
    for (ECS::World* world : changedWorlds)
        InstancesChanged.Invoke(*world);
}

void PrefabLibrary::ReportNotResident(const GUID& source) const
{
    {
        std::lock_guard<std::mutex> lock(m_NotResidentMutex);
        if (!m_ReportedNotResident.insert(source).second)
            return;
    }
    std::string name = source.ToString();
    AssetMetadata metadata{};
    if (m_AssetManager && m_AssetManager->GetRegistry().TryGetAssetMetadata(source, metadata))
        name = PathAsSpelledOnDisk(metadata.Path, EngineCore::GetInstance().GetResolvedAssetRoot());
    Logger::Log::Error("Prefab {} is not loaded. Reference it from a PrefabRef field of a component in the scene, "
                       "or place an instance in the scene.",
                       name);
}

std::uint32_t PrefabLibrary::GetReferenceCount(const std::filesystem::path& path) const
{
    const auto it = m_Records.find(RecordKey(path));
    return it == m_Records.end() ? 0u : it->second->References;
}

std::size_t PrefabLibrary::GetRetiredCount() const
{
    std::size_t count = 0;
    for (const auto& [key, record] : m_Records)
        count += record->Retired.size();
    return count;
}

void PrefabLibrary::DestroyTemplate(const PrefabTemplate& prefab)
{
    for (const ECS::EntityHandle entity : prefab.Entities)
    {
        if (m_World->IsValid(entity))
            m_World->DestroyEntityImmediate(entity);
    }
}

} // namespace GameEngine::Scene
