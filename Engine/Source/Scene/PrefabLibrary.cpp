#include "Scene/PrefabLibrary.h"

#include "AssetCore/PathNormalization.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Scene/AssetManagerSceneResolver.h"
#include "Scene/PrefabPlacement.h"
#include "Scene/SceneEngineEmbedMaterializer.h"
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
    const auto it = m_Records.find(path.generic_string());
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
    const std::string key = built.Path.generic_string();
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

void PrefabLibrary::Hold(const ECS::World& world, const PrefabTemplate& prefab)
{
    AssertWritable();
    WorldHolds& holds = m_HoldsByWorld[world.GetWorldId()];
    if (holds.Epoch != world.GetEntityHandleEpoch())
    {
        ReleaseHolds(holds);
        holds.Epoch = world.GetEntityHandleEpoch();
    }
    std::string key = prefab.Path.generic_string();
    if (std::find(holds.Keys.begin(), holds.Keys.end(), key) != holds.Keys.end())
        return;
    const auto it = m_Records.find(key);
    assert(it != m_Records.end() && "Hold names a resident prefab");
    if (it == m_Records.end())
        return;
    ++it->second->References;
    holds.Keys.push_back(std::move(key));
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
    for (const std::string& key : holds.Keys)
    {
        const auto it = m_Records.find(key);
        if (it != m_Records.end() && it->second->References > 0)
            --it->second->References;
    }
    holds.Keys.clear();
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

    for (auto it = m_Records.begin(); it != m_Records.end();)
    {
        Record& record = *it->second;
        for (const auto& retired : record.Retired)
            DestroyTemplate(*retired);
        record.Retired.clear();
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
    std::vector<const PrefabTemplate*> stale;
    for (const auto& [key, record] : m_Records)
    {
        if (ReadsAnyOf(*record->Current, changedFiles))
            stale.push_back(record->Current.get());
    }
    if (stale.empty())
        return;

    EngineCore& engine = EngineCore::GetInstance();
    AssetManagerSceneResolver resolver(*m_AssetManager, engine.GetResolvedAssetRoot(),
                                       AssetManagerSceneResolver::UnknownFile::AssignGuid);
    SceneEngineEmbedMaterializer materializer(*m_AssetManager);
    LoadOptions options;
    options.assetResolver = &resolver;
    options.assetRootOverride = resolver.GetAssetRoot();
    options.projectRoot = engine.GetWorkspaceRoot();
    options.embedMaterializer = &materializer;
    for (const PrefabTemplate* prefab : stale)
    {
        // A rebuild retires the old generation, which stays alive until the frame boundary.
        const std::filesystem::path& path = prefab->Path;
        std::string error;
        if (!BuildTemplateFromFile(*this, path, prefab->Source, options, error))
            Logger::Log::Error("Prefab {} changed but could not be rebuilt, so spawns and loads still use the "
                               "previous version: {}",
                               path.filename().string(), error);
    }
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
    const auto it = m_Records.find(path.generic_string());
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
