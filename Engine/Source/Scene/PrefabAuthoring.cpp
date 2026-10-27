#include "Scene/PrefabAuthoring.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/PrefabInstance.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ClonePlan.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/EntityRemap.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Scene/AssetManagerSceneResolver.h"
#include "Scene/PrefabLibrary.h"
#include "Scene/PrefabPlacement.h"
#include "Scene/SceneDoc.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scripting/AbiMainThread.h"
#include "Types/FlatSet.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine::Scene
{

namespace
{

// One entity of the subtree that becomes the prefab.
struct SubtreeEntity
{
    ECS::EntityHandle Handle;
    // The scene identity the entity has in the prefab file: its id, or for a member of an
    // instance inside the subtree, its path under that instance's id.
    std::string FilePath;
    // The entity's address under the prefab's root; the root's is empty.
    std::string Address;
};

// The children of each entity of a world, disabled branches included.
using ChildrenByParent = std::unordered_map<ECS::EntityId, std::vector<ECS::EntityHandle>>;

// What ReidentifySubtrees does with the entities of a subtree.
enum class Reidentify
{
    Unpack,
    UnpackCompletely,
    Copy,
};

// An entity of the subtree that ReidentifySubtrees walks: the id of the instance it is a member of in
// the new state, empty when it is a member of none, and its address under that instance.
struct ReidentifiedPlace
{
    ECS::EntityHandle Entity;
    std::string InstanceId;
    std::string Address;
};

} // namespace

// The display name of `entity` for a message: its Name, or its scene identity.
static std::string DisplayNameOf(const ECS::World& world, ECS::EntityHandle entity)
{
    if (const auto* name = world.GetComponent<Components::Name>(entity); name && !name->View().empty())
        return std::string(name->View());
    if (const auto* tag = world.GetComponent<Components::SceneEntityTag>(entity))
        return std::string(tag->View());
    return "the entity";
}

// The nearest prefab instance root above `entity`, or the invalid handle.
static ECS::EntityHandle FindEnclosingInstance(const ECS::World& world, ECS::EntityHandle entity)
{
    for (const auto* parent = world.GetComponent<Components::Parent>(entity); parent && parent->Entity.IsValid();
         parent = world.GetComponent<Components::Parent>(parent->Entity))
    {
        if (world.GetComponent<Components::PrefabInstance>(parent->Entity))
            return parent->Entity;
    }
    return {};
}

static ChildrenByParent CollectChildrenByParent(ECS::World& world)
{
    ChildrenByParent childrenOf;
    world.Query<ECS::Read<Components::Parent>>().IncludeDisabled().Each(
        [&childrenOf](ECS::EntityHandle child, const Components::Parent& parent)
        {
            if (parent.Entity.IsValid())
                childrenOf[parent.Entity.id].push_back(child);
        });
    return childrenOf;
}

// A new id in the prefab file from `preferred`, at most kMaxPrefabIdLength characters.
static std::string AllocatePrefabId(std::string preferred, std::unordered_set<std::string>& used)
{
    if (preferred.size() > kMaxPrefabIdLength)
        preferred.resize(kMaxPrefabIdLength);
    std::string id = AllocateUniqueSceneEntityId(preferred, used);
    while (id.size() > kMaxPrefabIdLength)
    {
        used.erase(id);
        preferred.resize(preferred.size() - (id.size() - kMaxPrefabIdLength));
        id = AllocateUniqueSceneEntityId(preferred, used);
    }
    return id;
}

// The subtree under `root`, parents before children, RuntimeOnlyEntity branches left out, with the
// file path and the address of each entity. An entity under an instance inside the subtree that has
// a member tag keeps its last id, so the file places that instance and the entity stays its member.
static std::vector<SubtreeEntity> CollectSubtree(ECS::World& world, ECS::EntityHandle root)
{
    const ChildrenByParent childrenOf = CollectChildrenByParent(world);
    std::vector<SubtreeEntity> subtree;
    std::vector<bool> underInstance; // true below an instance root inside the subtree
    subtree.push_back({root, std::string(kPrefabRootId), {}});
    underInstance.push_back(false);
    std::unordered_set<std::string> usedIds{std::string(kPrefabRootId)};
    for (std::size_t index = 0; index < subtree.size(); ++index)
    {
        const auto children = childrenOf.find(subtree[index].Handle.id);
        if (children == childrenOf.end())
            continue;
        const bool childrenUnderInstance =
            underInstance[index] || (index != 0 && world.GetComponent<Components::PrefabInstance>(subtree[index].Handle));
        for (const ECS::EntityHandle child : children->second)
        {
            if (world.GetComponent<Components::RuntimeOnlyEntity>(child))
                continue;
            const auto* tag = world.GetComponent<Components::SceneEntityTag>(child);
            const bool member = childrenUnderInstance && tag && IsMemberTag(tag->View());
            SubtreeEntity entity{child, {}, {}};
            std::string segment;
            if (member)
            {
                segment = std::string(LastIdOf(tag->View()));
                entity.FilePath = JoinPath(subtree[index].FilePath, segment);
            }
            else
            {
                const auto* name = world.GetComponent<Components::Name>(child);
                std::string preferred = SlugifyDisplayNameForSceneEntityId(name ? name->View() : std::string_view{});
                if (preferred.empty() && tag)
                    preferred = SlugifyDisplayNameForSceneEntityId(LastIdOf(tag->View()));
                segment = AllocatePrefabId(std::move(preferred), usedIds);
                entity.FilePath = segment;
            }
            entity.Address = JoinPath(subtree[index].Address, segment);
            subtree.push_back(std::move(entity));
            underInstance.push_back(childrenUnderInstance);
        }
    }
    return subtree;
}

// Writes the prefab file of `subtree`: a copy in a scratch world with the file's identities, the
// root without a parent and at the identity transform. Entity fields that leave the subtree are
// copied as the invalid handle.
static bool WritePrefabFile(ECS::World& world, std::span<const SubtreeEntity> subtree,
                            const std::filesystem::path& prefabFile, ISceneAssetResolver& resolver,
                            std::string& outError)
{
    std::vector<ECS::EntityHandle> handles;
    handles.reserve(subtree.size());
    for (const SubtreeEntity& entity : subtree)
        handles.push_back(entity.Handle);
    ECS::ClonePlan plan;
    ECS::World scratch;
    ECS::EntityRemap remap;
    if (!world.PlanClone(handles, plan) || !scratch.CreateBatchFromWorld(world, plan, remap))
    {
        outError = "Cannot create the prefab: an entity of the selection is no longer alive. Select it again.";
        return false;
    }
    const std::span<const ECS::EntityHandle> copies = remap.GetTargets();
    for (std::size_t i = 0; i < copies.size(); ++i)
        scratch.AddComponentImmediate(copies[i], MakeTag(SceneTagForPath(subtree[i].FilePath)));
    scratch.RemoveComponentImmediate<Components::Parent>(copies.front());
    scratch.AddComponentImmediate(copies.front(), Components::Transform{});

    SaveOptions options;
    options.writePrefabHeader = true;
    options.assetResolver = &resolver;
    if (!SaveSceneToFile(scratch, prefabFile, options))
    {
        outError = "Cannot write the prefab " + prefabFile.string() + ": " + GetLastSceneIOError().message;
        return false;
    }
    return true;
}

// The resident template of `prefab`, built from its file through `options.assetResolver` when it is
// not resident. Null, with the cause in `outError`, when the file cannot be placed; when the project
// has no such prefab, `missingFix` follows the cause.
static const PrefabTemplate* ResolveForPlacement(PrefabLibrary& library, const GUID& prefab, const LoadOptions& options,
                                                 std::string_view missingFix, std::string& outError)
{
    if (const PrefabTemplate* resident = library.FindByGuid(prefab))
        return resident;
    ISceneAssetResolver& resolver = *options.assetResolver;
    std::filesystem::path path;
    AssetType type = AssetType::Unknown;
    if (!resolver.TryGetPathAndType(resolver.ResolveGuid(prefab), path, type) || type != AssetType::Prefab)
    {
        outError = std::format("the project has no prefab asset {}. {}", prefab.ToString(), missingFix);
        return nullptr;
    }
    return BuildTemplateFromFile(library, path.lexically_normal(), prefab, options, outError);
}

ECS::EntityHandle PlacePrefab(ECS::World& world, const GUID& prefab, const Components::Transform& at,
                              ECS::EntityHandle parent)
{
    EngineCore& engine = EngineCore::GetInstance();
    PrefabLibrary* library = engine.TryGetPrefabLibrary();
    if (!library)
    {
        Logger::Log::Error("Scene::PlacePrefab needs an initialized engine: call EngineCore::Initialize first.");
        return {};
    }
    assert(ScriptingAbi::IsMainThreadOutsideJobFor(engine) &&
           "Scene::PlacePrefab runs on the main thread only, outside any job");
    AssetManagerSceneResolver resolver(engine.GetAssetManager(), engine.GetResolvedAssetRoot(),
                                       AssetManagerSceneResolver::UnknownFile::Register);
    LoadOptions options{LoadMode::Additive};
    options.assetResolver = &resolver;
    std::string error;
    const PrefabTemplate* prefabTemplate =
        ResolveForPlacement(*library, prefab, options, "Place a .prefab file from the project.", error);
    if (!prefabTemplate)
    {
        Logger::Log::Error("Scene::PlacePrefab: {}", error);
        return {};
    }
    library->Hold(world, *prefabTemplate);

    thread_local ECS::EntityRemap remap;
    const ECS::EntityHandle root = SpawnTemplate(world, *library, *prefabTemplate, prefab, at, parent, remap);
    if (!root.IsValid())
        return {};
    // The registry's path is case-folded; its Name is the file name as spelled on disk.
    AssetMetadata metadata;
    const std::string stem = engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(prefab, metadata)
                                 ? metadata.Name
                                 : prefabTemplate->Path.stem().string();
    if (prefabTemplate->RootTakesFallbackName)
    {
        Components::Name fallback{};
        stem.copy(fallback.value, sizeof(fallback.value) - 1);
        world.AddComponentImmediate(root, fallback);
    }

    const auto* name = world.GetComponent<Components::Name>(root);
    std::string preferred = SlugifyDisplayNameForSceneEntityId(name ? name->View() : std::string_view(stem));
    std::unordered_set<std::string> used = CollectSceneIdentities(world);
    const std::string instanceId = AllocateUniqueSceneEntityId(preferred, used);
    const std::span<const ECS::EntityHandle> targets = remap.GetTargets();
    for (std::size_t i = 0; i < targets.size(); ++i)
        world.AddComponentImmediate(targets[i],
                                    MakeTag(SceneTagForPath(JoinPath(instanceId, prefabTemplate->Addresses[i]))));
    HoldReferencedPrefabs(*library, world, options, targets);
    return root;
}

void RebuildPrefab(const GUID& prefab)
{
    PrefabLibrary* library = EngineCore::GetInstance().TryGetPrefabLibrary();
    if (!library)
    {
        Logger::Log::Error("Scene::RebuildPrefab needs an initialized engine: call EngineCore::Initialize first.");
        return;
    }
    library->RebuildChanged(std::span(&prefab, 1));
}

std::size_t CountPrefabEntities(ECS::World& world, ECS::EntityHandle root)
{
    return world.IsValid(root) ? CollectSubtree(world, root).size() : 0;
}

std::vector<OutgoingReference> CollectOutgoingReferences(ECS::World& world, ECS::EntityHandle root)
{
    if (!world.IsValid(root))
        return {};
    const std::vector<SubtreeEntity> subtree = CollectSubtree(world, root);
    FlatSet<ECS::EntityId> members;
    for (const SubtreeEntity& entity : subtree)
        members.Insert(entity.Handle.id);
    // The root's parent leaves the subtree by design: a prefab root has no parent.
    const ECS::ComponentTypeId parentType = ECS::GetComponentTypeId<Components::Parent>();
    std::vector<OutgoingReference> references;
    std::vector<std::uint8_t> bytes;
    for (const ECS::ComponentTypeId typeId : SceneSchemaRegistry::GetSavedComponentTypes())
    {
        if (typeId == parentType)
            continue;
        for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(typeId))
        {
            if (field.Type != ECS::FieldTypeId::EntityHandle)
                continue;
            for (const SubtreeEntity& entity : subtree)
            {
                if (!world.HasComponent(entity.Handle, typeId) ||
                    !world.CaptureComponentBytes(entity.Handle, typeId, bytes))
                    continue;
                const std::size_t end = std::min<std::size_t>(field.Offset + field.Size, bytes.size());
                for (std::size_t offset = field.Offset; offset + sizeof(ECS::EntityHandle) <= end;
                     offset += sizeof(ECS::EntityHandle))
                {
                    ECS::EntityHandle target;
                    std::memcpy(&target, bytes.data() + offset, sizeof(target));
                    if (world.IsValid(target) && !members.Contains(target.id))
                        references.push_back({entity.Handle, typeId, std::string(field.Name), target});
                }
            }
        }
    }
    return references;
}

// The scene identity and the instance marker `entity` has now.
static PrefabIdentityBefore CaptureIdentity(const ECS::World& world, ECS::EntityHandle entity)
{
    PrefabIdentityBefore identity{entity, std::nullopt, std::nullopt};
    if (const auto* tag = world.GetComponent<Components::SceneEntityTag>(entity))
        identity.Tag = *tag;
    if (const auto* marker = world.GetComponent<Components::PrefabInstance>(entity))
        identity.Marker = *marker;
    return identity;
}

// A new plain scene identity for `entity`, from its name, or from the last id of its tag.
static std::string AllocatePlainIdentity(const ECS::World& world, ECS::EntityHandle entity,
                                         std::unordered_set<std::string>& used)
{
    std::string preferred;
    if (const auto* name = world.GetComponent<Components::Name>(entity))
        preferred = SlugifyDisplayNameForSceneEntityId(name->View());
    const auto* tag = world.GetComponent<Components::SceneEntityTag>(entity);
    if (preferred.empty() && tag)
        preferred = SlugifyDisplayNameForSceneEntityId(LastIdOf(tag->View()));
    return AllocateUniqueSceneEntityId(preferred, used);
}

// Makes the prefab of each instance in `sources` resident and holds it for `world`, as Place does. A
// prefab that cannot be read is logged; its instance shows as missing.
static void HoldInstancePrefabs(ECS::World& world, std::span<const GUID> sources, std::string_view operation)
{
    EngineCore& engine = EngineCore::GetInstance();
    PrefabLibrary* library = engine.TryGetPrefabLibrary();
    if (!library || sources.empty())
        return;
    AssetManagerSceneResolver resolver(engine.GetAssetManager(), engine.GetResolvedAssetRoot(),
                                       AssetManagerSceneResolver::UnknownFile::Register);
    LoadOptions options{LoadMode::Additive};
    options.assetResolver = &resolver;
    for (const GUID& source : sources)
    {
        std::string error;
        const PrefabTemplate* prefab = ResolveForPlacement(
            *library, source, options, "The instance keeps its marker; restore the .prefab file to show it.", error);
        if (prefab)
            library->Hold(world, *prefab);
        else
            Logger::Log::Error("Scene::{}: {}", operation, error);
    }
}

// Gives the subtrees under `roots` new scene identities as `mode` says, parents first. A member tag under
// an instance becomes the identity of its address there; a marker with no instance above it starts an
// instance with a new id and holds its prefab; any other entity is plain. Returns what changed.
static std::vector<PrefabIdentityBefore> ReidentifySubtrees(ECS::World& world, std::span<const ECS::EntityHandle> roots,
                                                             Reidentify mode)
{
    const ChildrenByParent childrenOf = CollectChildrenByParent(world);
    auto used = CollectSceneIdentities(world);
    std::vector<PrefabIdentityBefore> changed;
    std::vector<GUID> startedSources;
    std::vector<ReidentifiedPlace> places;
    std::vector<std::size_t> parentOf;
    for (const ECS::EntityHandle root : roots)
    {
        places.push_back({root, {}, {}});
        parentOf.push_back(places.size() - 1);
    }
    for (std::size_t index = 0; index < places.size(); ++index)
    {
        const ECS::EntityHandle entity = places[index].Entity;
        const PrefabIdentityBefore before = CaptureIdentity(world, entity);
        const bool isRoot = index < roots.size();
        const ReidentifiedPlace* parent = isRoot ? nullptr : &places[parentOf[index]];
        const bool member = parent && !parent->InstanceId.empty() && before.Tag && IsMemberTag(before.Tag->View());
        const bool startsInstance = !member && before.Marker && mode != Reidentify::UnpackCompletely &&
                                    !(isRoot && mode == Reidentify::Unpack);
        std::optional<std::string> tag;
        if (before.Tag)
            tag = std::string(before.Tag->View());
        if (member)
        {
            places[index].Address = JoinPath(parent->Address, LastIdOf(before.Tag->View()));
            places[index].InstanceId = parent->InstanceId;
            tag = SceneTagForPath(JoinPath(parent->InstanceId, places[index].Address));
        }
        else if (startsInstance)
        {
            places[index].InstanceId = AllocatePlainIdentity(world, entity, used);
            tag = places[index].InstanceId;
            startedSources.push_back(before.Marker->Source);
        }
        else if (mode == Reidentify::Copy)
            tag.reset();
        else if (tag && IsMemberTag(*tag))
            tag = AllocatePlainIdentity(world, entity, used);

        const bool removesMarker = before.Marker && !member && !startsInstance;
        const bool tagChanged = tag ? !before.Tag || before.Tag->View() != *tag : before.Tag.has_value();
        if (removesMarker)
            world.RemoveComponentImmediate<Components::PrefabInstance>(entity);
        if (tagChanged && tag)
            world.AddComponentImmediate(entity, MakeTag(*tag));
        else if (tagChanged)
            world.RemoveComponentImmediate<Components::SceneEntityTag>(entity);
        if (tagChanged || removesMarker)
            changed.push_back(before);

        const auto children = childrenOf.find(entity.id);
        if (children == childrenOf.end())
            continue;
        for (const ECS::EntityHandle child : children->second)
        {
            places.push_back({child, {}, {}});
            parentOf.push_back(index);
        }
    }
    HoldInstancePrefabs(world, startedSources,
                        mode == Reidentify::Copy ? "GiveCopySceneIdentities" : "UnpackPrefabInstance");
    return changed;
}

std::vector<PrefabIdentityBefore> UnpackPrefabInstance(ECS::World& world, ECS::EntityHandle root,
                                                       PrefabUnpackDepth depth, std::string& outError)
{
    assert(ScriptingAbi::IsMainThreadOutsideJobFor(EngineCore::GetInstance()) &&
           "Scene::UnpackPrefabInstance runs on the main thread only, outside any job");
    if (!world.IsValid(root) || !world.GetComponent<Components::PrefabInstance>(root))
    {
        outError = "Cannot unpack: the entity is not a prefab instance. Select the root of an instance.";
        return {};
    }
    if (const ECS::EntityHandle instance = FindEnclosingInstance(world, root); instance.IsValid())
    {
        const std::string outer = DisplayNameOf(world, instance);
        outError = "Cannot unpack \"" + DisplayNameOf(world, root) + "\": it is inside the prefab instance \"" + outer +
                   "\". Unpack \"" + outer + "\" first.";
        return {};
    }
    return ReidentifySubtrees(world, std::span(&root, 1),
                              depth == PrefabUnpackDepth::Completely ? Reidentify::UnpackCompletely : Reidentify::Unpack);
}

void GiveCopySceneIdentities(ECS::World& world, std::span<const ECS::EntityHandle> copyRoots)
{
    assert(ScriptingAbi::IsMainThreadOutsideJobFor(EngineCore::GetInstance()) &&
           "Scene::GiveCopySceneIdentities runs on the main thread only, outside any job");
    ReidentifySubtrees(world, copyRoots, Reidentify::Copy);
}

void RestorePrefabIdentities(ECS::World& world, std::span<const PrefabIdentityBefore> before)
{
    assert(ScriptingAbi::IsMainThreadOutsideJobFor(EngineCore::GetInstance()) &&
           "Scene::RestorePrefabIdentities runs on the main thread only, outside any job");
    for (const PrefabIdentityBefore& identity : before)
    {
        if (!world.IsValid(identity.Entity))
            continue;
        if (identity.Tag)
            world.AddComponentImmediate(identity.Entity, *identity.Tag);
        else
            world.RemoveComponentImmediate<Components::SceneEntityTag>(identity.Entity);
        if (identity.Marker)
            world.AddComponentImmediate(identity.Entity, *identity.Marker);
        else
            world.RemoveComponentImmediate<Components::PrefabInstance>(identity.Entity);
    }
}

CreatedPrefab CreatePrefab(ECS::World& world, ECS::EntityHandle root, const std::filesystem::path& prefabFile,
                           std::string& outError)
{
    EngineCore& engine = EngineCore::GetInstance();
    PrefabLibrary* library = engine.TryGetPrefabLibrary();
    if (!library)
    {
        outError = "Scene::CreatePrefab needs an initialized engine: call EngineCore::Initialize first.";
        return {};
    }
    assert(ScriptingAbi::IsMainThreadOutsideJobFor(engine) &&
           "Scene::CreatePrefab runs on the main thread only, outside any job");
    if (!world.IsValid(root))
    {
        outError = "Cannot create a prefab: the entity no longer exists. Select an entity in the scene.";
        return {};
    }
    const std::string rootName = DisplayNameOf(world, root);
    if (world.GetComponent<Components::PrefabInstance>(root))
    {
        outError = "\"" + rootName + "\" is already a prefab instance. Select an entity that is not an instance.";
        return {};
    }
    if (const ECS::EntityHandle instance = FindEnclosingInstance(world, root); instance.IsValid())
    {
        outError = "\"" + rootName + "\" is inside the prefab instance \"" + DisplayNameOf(world, instance) +
                   "\". Select an entity outside an instance.";
        return {};
    }
    if (world.GetComponent<Components::RuntimeOnlyEntity>(root))
    {
        outError = "\"" + rootName + "\" is runtime state that is never saved. Select an entity of the scene.";
        return {};
    }
    const std::filesystem::path path = prefabFile.lexically_normal();
    if (path.extension() != ".prefab")
    {
        outError = "Cannot create the prefab " + path.string() + ": the file name must end in .prefab.";
        return {};
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
    {
        outError = "A file already exists at " + path.string() + ". Choose another name, or remove the file first.";
        return {};
    }

    const std::vector<SubtreeEntity> subtree = CollectSubtree(world, root);
    std::vector<ECS::EntityHandle> handles;
    handles.reserve(subtree.size());
    for (const SubtreeEntity& entity : subtree)
        handles.push_back(entity.Handle);
    AssetManagerSceneResolver resolver(engine.GetAssetManager(), engine.GetResolvedAssetRoot(),
                                       AssetManagerSceneResolver::UnknownFile::Register);
    if (!WritePrefabFile(world, subtree, path, resolver, outError))
        return {};

    const GUID guid = resolver.GetOrCreateAssetGuid(path);
    if (guid.IsNull())
    {
        std::filesystem::remove(path, ec);
        outError = "Cannot create the prefab " + path.string() +
                   ": the file is outside the project's assets. Save the prefab in a folder of the project.";
        return {};
    }
    LoadOptions loadOptions{LoadMode::Additive};
    loadOptions.assetResolver = &resolver;
    std::string buildError;
    const PrefabTemplate* prefabTemplate = BuildTemplateFromFile(*library, path, guid, loadOptions, buildError);
    if (!prefabTemplate)
    {
        std::filesystem::remove(path, ec);
        outError = "Cannot create the prefab: the file written does not load. " + buildError;
        return {};
    }
    FlatSet<std::string_view> addresses;
    for (const std::string& address : prefabTemplate->Addresses)
        addresses.Insert(address);
    for (const SubtreeEntity& entity : subtree)
    {
        if (!addresses.Contains(entity.Address))
        {
            std::filesystem::remove(path, ec);
            outError = "Cannot create the prefab: \"" + DisplayNameOf(world, entity.Handle) +
                       "\" has no place in the file written. Report this with the scene.";
            return {};
        }
    }

    // The subtree becomes the instance in place: the marker on the root and every entity's scene
    // identity from its address, so a scene save writes the instance as a [prefab] section.
    const auto* rootTag = world.GetComponent<Components::SceneEntityTag>(root);
    std::string instanceId;
    if (rootTag && !rootTag->View().empty())
        instanceId = std::string(rootTag->View());
    else
    {
        std::unordered_set<std::string> used = CollectSceneIdentities(world);
        instanceId = AllocateUniqueSceneEntityId(SlugifyDisplayNameForSceneEntityId(rootName), used);
    }
    CreatedPrefab created{guid, {}};
    created.Members.reserve(subtree.size());
    for (const SubtreeEntity& entity : subtree)
    {
        created.Members.push_back(CaptureIdentity(world, entity.Handle));
        world.AddComponentImmediate(entity.Handle, MakeTag(SceneTagForPath(JoinPath(instanceId, entity.Address))));
    }
    Components::PrefabInstance marker{};
    marker.Source = guid;
    world.AddComponentImmediate(root, marker);
    library->Hold(world, *prefabTemplate);
    HoldReferencedPrefabs(*library, world, loadOptions, handles);
    return created;
}

} // namespace GameEngine::Scene
