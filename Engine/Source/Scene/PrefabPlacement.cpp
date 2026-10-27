#include "Scene/PrefabPlacement.h"

#include "Components/Hierarchy.h"
#include "Components/PrefabInstance.h"
#include "Components/SceneEntityTag.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Scene/PrefabLibrary.h"
#include "Scene/SceneAssetResolver.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Types/FlatSet.h"
#include "Types/PathUtils.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <optional>
#include <string_view>
#include <system_error>

namespace GameEngine::Scene
{

PrefabPlacement::PrefabPlacement(ECS::World& targetWorld, const LoadOptions& loadOptions,
                                 SceneLoadDegradation* loadDegradation)
    : world(targetWorld), options(loadOptions), degradation(loadDegradation)
{
}

PrefabPlacement::~PrefabPlacement() = default;

static ECS::EntityHandle FindMember(const PlacedPrefab& placed, std::string_view address)
{
    for (const auto& [memberAddress, h] : placed.members)
    {
        if (memberAddress == address)
            return h;
    }
    return {};
}

// Records the hashed identity `tag` of `path`. False, with both paths and the fix, when another path
// already has it.
static bool ClaimHashedTag(PrefabPlacement& placement, const std::string& tag, const std::string& path,
                           const std::filesystem::path& file, int line, const LoadFailure& failure)
{
    if (tag == path)
        return true;
    const std::string* claimed = placement.pathByHashedTag.Find(tag);
    if (!claimed)
    {
        placement.pathByHashedTag.InsertOrAssign(tag, path);
        return true;
    }
    if (*claimed == path)
        return true;
    return failure.Fail(file, line,
                        "Two entities get the scene identity \"" + tag + "\": \"" + *claimed + "\" and \"" + path +
                            "\". Shorten an id in one of the chains.");
}

static bool PlacePrefab(PrefabPlacement& placement, ParsedPrefab& prefab, const std::string& instancePath,
                        PlacedPrefab& out, const LoadFailure& failure);

// The prefab the [prefab] section `section` of `ownerFile` places, read and checked once per load.
// Null, with the failure recorded, when the file cannot be read, does not parse, or is not a prefab.
static ParsedPrefab* ReadPrefab(PrefabPlacement& placement, const SceneLoadContext& ownerCtx,
                                const std::filesystem::path& ownerFile, const SceneDoc& ownerDoc,
                                const SceneDoc::PrefabInstanceSection& section, const LoadFailure& failure)
{
    const SceneDoc::Resource& resource = ownerDoc.resources.at(section.sourceResourceId);
    const std::filesystem::path path = ResolveResourcePath(ownerCtx, ownerFile, resource).lexically_normal();
    const std::string key = path.generic_string();
    if (auto* found = placement.parsedByPath.Find(key))
        return found->get();

    std::string text;
    if (path.empty() || !ReadTextFile(path, text))
    {
        ReportUnreadableResource("Prefab", resource, path, placement.options.projectRoot, ownerFile,
                                 section.headerLine, failure.message, failure.file, failure.line);
        return nullptr;
    }
    auto parsed = std::make_unique<ParsedPrefab>();
    parsed->path = path;
    int errorLine = 0;
    std::string error;
    if (!ParseDocument(path, text, parsed->doc, &error, &errorLine) ||
        !ValidateDocument(parsed->doc, &error, &errorLine))
    {
        failure.Fail(path, errorLine, std::move(error));
        return nullptr;
    }
    if (!parsed->doc.isPrefab)
    {
        failure.Fail(ownerFile, section.headerLine,
                     "The prefab instance \"" + section.id + "\" on line " + std::to_string(section.headerLine) +
                         " uses resource \"" + section.sourceResourceId + "\", which is a scene, not a prefab. Point "
                         "the resource at a .prefab file, or place the scene with [subscene].");
        return nullptr;
    }
    parsed->ctx = MakeDocumentLoadContext(placement.world, parsed->path, parsed->doc, placement.options);
    return placement.parsedByPath.InsertOrAssign(key, std::move(parsed)).get();
}

// Places the prefab the section names at `instancePath` from its file's text and marks its root
// with the source.
static bool PlaceInstanceFromText(PrefabPlacement& placement, const SceneLoadContext& ownerCtx,
                                  const std::filesystem::path& ownerFile, const SceneDoc& ownerDoc,
                                  const SceneDoc::PrefabInstanceSection& section, const std::string& instancePath,
                                  PlacedPrefab& out, const LoadFailure& failure)
{
    ParsedPrefab* prefab = ReadPrefab(placement, ownerCtx, ownerFile, ownerDoc, section, failure);
    if (!prefab)
        return false;

    if (std::find(placement.chain.begin(), placement.chain.end(), prefab->path) != placement.chain.end())
    {
        std::string loop;
        for (const auto& file : placement.chain)
            loop += file.filename().string() + " places ";
        loop += prefab->path.filename().string();
        return failure.Fail(ownerFile, section.headerLine,
                            "Prefab cycle: " + loop + ". Remove the prefab instance on line " +
                                std::to_string(section.headerLine) + ", which closes the loop.");
    }

    if (placement.buildLog)
    {
        std::vector<std::filesystem::path>& files = placement.buildLog->files;
        if (std::find(files.begin(), files.end(), prefab->path) == files.end())
            files.push_back(prefab->path);
    }
    placement.chain.push_back(prefab->path);
    const bool placed = PlacePrefab(placement, *prefab, instancePath, out, failure);
    placement.chain.pop_back();
    if (!placed)
        return false;

    Components::PrefabInstance marker{};
    AssetReference source;
    if (TryResolveResourceIdToAssetReference(ownerCtx, section.sourceResourceId, source, nullptr))
        marker.Source = source.guid;
    placement.world.AddComponentImmediate(out.root, marker);
    return true;
}

static PrefabLibrary& LibraryOf(PrefabPlacement& placement)
{
    if (!placement.library)
    {
        placement.ownedLibrary = std::make_unique<PrefabLibrary>();
        placement.library = placement.ownedLibrary.get();
    }
    return *placement.library;
}

// Builds the template of the prefab at `path` (which `section` names) into the library: a new
// generation when the prefab is resident. `knownSource` is the prefab's asset GUID, or null. On
// failure: records the failure, leaves nothing of the build in the library world, returns null.
static const PrefabTemplate* BuildTemplate(PrefabPlacement& placement, const SceneLoadContext& ownerCtx,
                                           const std::filesystem::path& ownerFile, const SceneDoc& ownerDoc,
                                           const SceneDoc::PrefabInstanceSection& section,
                                           const std::filesystem::path& path, const GUID& knownSource,
                                           const LoadFailure& failure)
{
    PrefabLibrary& library = LibraryOf(placement);
    if (!placement.templateBuild)
    {
        placement.templateBuild =
            std::make_unique<PrefabPlacement>(library.GetWorld(), placement.options, nullptr);
        placement.templateBuild->writesSceneTags = false;
    }
    PrefabPlacement& build = *placement.templateBuild;
    SceneLoadDegradation rows;
    TemplateBuildLog log;
    build.degradation = &rows;
    build.buildLog = &log;
    PlacedPrefab placed;
    const bool built = PlaceInstanceFromText(build, ownerCtx, ownerFile, ownerDoc, section, std::string(), placed,
                                             failure);
    build.degradation = nullptr;
    build.buildLog = nullptr;

    ECS::World& libraryWorld = library.GetWorld();
    if (!built)
    {
        for (const ECS::EntityHandle h : log.entities)
        {
            if (libraryWorld.IsValid(h))
                libraryWorld.DestroyEntityImmediate(h);
        }
        return nullptr;
    }

    PrefabTemplate prefab;
    prefab.Path = path;
    if (const auto* marker = libraryWorld.GetComponent<Components::PrefabInstance>(placed.root))
    {
        prefab.Source = knownSource.IsNull() ? marker->Source : knownSource;
        Components::PrefabInstance known = *marker;
        known.Source = prefab.Source;
        libraryWorld.AddComponentImmediate(placed.root, known);
    }
    prefab.Entities.reserve(placed.members.size());
    prefab.Addresses.reserve(placed.members.size());
    prefab.Origins.reserve(placed.members.size());
    for (auto& [address, h] : placed.members)
    {
        const auto created = std::find(log.entities.begin(), log.entities.end(), h);
        assert(created != log.entities.end() && "Every template entity is created by its build");
        prefab.Addresses.push_back(std::move(address));
        prefab.Entities.push_back(h);
        prefab.Origins.push_back(log.origins[static_cast<std::size_t>(created - log.entities.begin())]);
    }
    for (SceneLoadSkip& skip : rows.skips)
    {
        const auto it = std::find(prefab.Entities.begin(), prefab.Entities.end(), skip.entityHandle);
        const std::int32_t index =
            it == prefab.Entities.end() ? -1 : static_cast<std::int32_t>(it - prefab.Entities.begin());
        prefab.Rows.push_back(PrefabTemplate::Row{std::move(skip), index});
    }
    prefab.Files = std::move(log.files);
    const auto* nameSchema = SceneSchemaRegistry::Find("Name");
    prefab.RootTakesFallbackName = nameSchema && !nameSchema->IsPresent(libraryWorld, placed.root);
    return &library.Insert(std::move(prefab));
}

// The template of the prefab `section` places: the resident one, or one built from the prefab's
// text the first time the library meets it.
static const PrefabTemplate* ResolveTemplate(PrefabPlacement& placement, const SceneLoadContext& ownerCtx,
                                             const std::filesystem::path& ownerFile, const SceneDoc& ownerDoc,
                                             const SceneDoc::PrefabInstanceSection& section,
                                             const LoadFailure& failure)
{
    const SceneDoc::Resource& resource = ownerDoc.resources.at(section.sourceResourceId);
    const std::filesystem::path path = ResolveResourcePath(ownerCtx, ownerFile, resource).lexically_normal();
    if (const PrefabTemplate* resident = LibraryOf(placement).FindByPath(path))
        return resident;
    return BuildTemplate(placement, ownerCtx, ownerFile, ownerDoc, section, path, GUID::Null(), failure);
}

std::string PathAsSpelledOnDisk(const std::filesystem::path& file, const std::filesystem::path& assetRoot)
{
    std::error_code error;
    const std::filesystem::path onDisk = std::filesystem::canonical(file, error);
    const std::filesystem::path shown = error ? file : onDisk;
    const std::filesystem::path root = std::filesystem::canonical(assetRoot, error);
    const std::filesystem::path relative = error ? std::filesystem::path{} : RelativePathUnderRoot(shown, root);
    return (relative.empty() ? shown : relative).generic_string();
}

const PrefabTemplate* BuildTemplateFromFile(PrefabLibrary& library, const std::filesystem::path& path,
                                            const GUID& source, const LoadOptions& options, std::string& error)
{
    SceneDoc ownerDoc;
    SceneDoc::Resource resource;
    resource.id = "prefab";
    resource.path = path.generic_string();
    ownerDoc.resources.emplace(resource.id, resource);
    SceneDoc::PrefabInstanceSection section;
    section.id = path.stem().string();
    section.sourceResourceId = resource.id;

    PrefabPlacement placement{library.GetWorld(), options, nullptr};
    placement.usesLibrary = true;
    placement.library = &library;
    const SceneLoadContext ownerCtx = MakeDocumentLoadContext(library.GetWorld(), path, ownerDoc, options);
    std::string message;
    std::filesystem::path file;
    int line = 0;
    const LoadFailure failure{&message, &file, &line};
    const PrefabTemplate* built = BuildTemplate(placement, ownerCtx, path, ownerDoc, section, path, source, failure);
    if (!built)
    {
        const std::filesystem::path assetRoot =
            options.assetRootOverride.empty() && options.assetResolver ? options.assetResolver->GetAssetRoot()
                                                                       : options.assetRootOverride;
        error = PathAsSpelledOnDisk(file.empty() ? path : file, assetRoot) + ":" + std::to_string(line) + ": " + message;
    }
    return built;
}

namespace
{
// A GUID that a PrefabRef field holds: the component type and the GUID's byte offset in it.
struct PrefabRefField
{
    ECS::ComponentTypeId TypeId = 0;
    std::uint32_t Offset = 0;
};
} // namespace

// Every PrefabRef GUID in the reflected fields of the component types that a scene saves.
static std::vector<PrefabRefField> CollectPrefabRefFields()
{
    std::vector<PrefabRefField> fields;
    for (const ECS::ComponentTypeId typeId : SceneSchemaRegistry::GetSavedComponentTypes())
    {
        for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(typeId))
        {
            if (field.Type != ECS::FieldTypeId::AssetGuid ||
                field.AssetCategory != static_cast<std::uint16_t>(AssetType::Prefab))
                continue;
            for (std::uint32_t offset = 0; offset + GUID::kSize <= field.Size; offset += GUID::kSize)
                fields.push_back({typeId, field.Offset + offset});
        }
    }
    return fields;
}

// Appends to `out` every non-null prefab GUID a PrefabRef field of `entities` in `world` holds.
static void CollectPrefabRefs(const ECS::World& world, std::span<const ECS::EntityHandle> entities,
                              std::span<const PrefabRefField> fields,
                              std::vector<GUID>& out)
{
    std::vector<std::uint8_t> bytes;
    for (const auto& [typeId, offset] : fields)
    {
        for (const ECS::EntityHandle entity : entities)
        {
            if (!world.HasComponent(entity, typeId) || !world.CaptureComponentBytes(entity, typeId, bytes) ||
                offset + GUID::kSize > bytes.size())
                continue;
            std::uint8_t raw[GUID::kSize];
            std::memcpy(raw, bytes.data() + offset, GUID::kSize);
            const GUID guid = GUID::FromBytes(raw);
            if (!guid.IsNull() && std::find(out.begin(), out.end(), guid) == out.end())
                out.push_back(guid);
        }
    }
}

std::unordered_set<std::string> CollectSceneIdentities(ECS::World& world)
{
    std::unordered_set<std::string> used;
    world.Query<ECS::Read<Components::SceneEntityTag>>().IncludeDisabled().Each(
        [&used](ECS::EntityHandle, const Components::SceneEntityTag& tag) { used.emplace(tag.View()); });
    return used;
}

void HoldReferencedPrefabs(PrefabLibrary& library, ECS::World& world, const LoadOptions& options,
                           std::span<const ECS::EntityHandle> entities)
{
    const auto fields = CollectPrefabRefFields();
    if (fields.empty())
        return;
    // Each GUID enters `pending` once, so a prefab that names itself or a cycle of prefabs ends.
    std::vector<GUID> pending;
    CollectPrefabRefs(world, entities, fields, pending);
    for (std::size_t next = 0; next < pending.size(); ++next)
    {
        const GUID source = pending[next];
        const PrefabTemplate* prefab = library.FindByGuid(source);
        if (!prefab)
        {
            std::filesystem::path path;
            AssetType type = AssetType::Unknown;
            ISceneAssetResolver* resolver = options.assetResolver;
            if (!resolver || !resolver->TryGetPathAndType(resolver->ResolveGuid(source), path, type))
            {
                Logger::Log::Error("Scene: a PrefabRef names the asset {}, which the project does not have. Assign "
                                   "an existing prefab to the field.",
                                   source.ToString());
                continue;
            }
            std::string error;
            prefab = BuildTemplateFromFile(library, path.lexically_normal(), source, options, error);
            if (!prefab)
            {
                Logger::Log::Error("Scene: a PrefabRef names a prefab that could not be loaded: {}", error);
                continue;
            }
            CollectPrefabRefs(library.GetWorld(), prefab->Entities, fields, pending);
        }
        library.Hold(world, *prefab);
    }
}

// Places the instance `section` at `instancePath` as a batch clone of its prefab's template, with
// scene identities from the addresses and the root marked with its source (and named after the
// instance when the prefab gives no name). Records the template's load-report rows again.
static bool PlaceInstanceFromLibrary(PrefabPlacement& placement, const SceneLoadContext& ownerCtx,
                                     const std::filesystem::path& ownerFile, const SceneDoc& ownerDoc,
                                     const SceneDoc::PrefabInstanceSection& section, const std::string& instancePath,
                                     PlacedPrefab& out, const LoadFailure& failure)
{
    const PrefabTemplate* prefab = ResolveTemplate(placement, ownerCtx, ownerFile, ownerDoc, section, failure);
    if (!prefab)
        return false;

    ECS::World& world = placement.world;
    placement.library->Hold(world, *prefab);
    if (!world.CreateBatchFromWorld(placement.library->GetWorld(), prefab->Plan, placement.remap))
        return failure.Fail(ownerFile, section.headerLine,
                            "The prefab instance \"" + section.id + "\" could not be copied from its prefab. Reload "
                            "the scene; if it persists, re-save the prefab.");
    const std::span<const ECS::EntityHandle> targets = placement.remap.GetTargets();
    out.root = targets.front();
    out.members.clear();
    out.members.reserve(targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        const std::string& address = prefab->Addresses[i];
        const std::string path = JoinPath(instancePath, address);
        const std::string tag = SceneTagForPath(path);
        const PrefabTemplate::Origin& origin = prefab->Origins[i];
        if (!ClaimHashedTag(placement, tag, path, prefab->Files[origin.File], origin.Line, failure))
            return false;
        world.AddComponentImmediate(targets[i], MakeTag(tag));
        out.members.emplace_back(address, targets[i]);
    }
    if (prefab->RootTakesFallbackName)
        ApplyFallbackName(world, ownerCtx, out.root, LastIdOf(instancePath), instancePath);

    Components::PrefabInstance marker{};
    AssetReference source;
    if (TryResolveResourceIdToAssetReference(ownerCtx, section.sourceResourceId, source, nullptr))
        marker.Source = source.guid;
    world.AddComponentImmediate(out.root, marker);

    if (placement.degradation)
    {
        for (const PrefabTemplate::Row& row : prefab->Rows)
        {
            SceneLoadSkip skip = row.Skip;
            skip.entityId = JoinPath(instancePath, skip.entityId);
            skip.entityHandle = row.EntityIndex >= 0 ? targets[static_cast<std::size_t>(row.EntityIndex)]
                                                     : ECS::EntityHandle{};
            placement.degradation->skips.push_back(std::move(skip));
        }
    }
    return true;
}

// Places the prefab the section names at `instancePath` and marks its root with the source. The
// section's own override lines are the caller's to apply, against the caller's entity ids.
bool PlaceInstanceSection(PrefabPlacement& placement, const SceneLoadContext& ownerCtx,
                          const std::filesystem::path& ownerFile, const SceneDoc& ownerDoc,
                          const SceneDoc::PrefabInstanceSection& section, const std::string& instancePath,
                          PlacedPrefab& out, const LoadFailure& failure)
{
    if (placement.usesLibrary)
        return PlaceInstanceFromLibrary(placement, ownerCtx, ownerFile, ownerDoc, section, instancePath, out, failure);
    return PlaceInstanceFromText(placement, ownerCtx, ownerFile, ownerDoc, section, instancePath, out, failure);
}

// The load-report row for an override line whose address names no entity of the prefab: an
// orphaned override. The load goes on; the line is not kept by a save.
static void RecordOrphanedOverride(SceneLoadDegradation* degradation, const std::string& instancePath,
                                   std::string_view address, std::string component, std::string_view line,
                                   const std::filesystem::path& ownerFile, int lineNo, ECS::EntityHandle root)
{
    if (!degradation)
        return;
    degradation->skips.push_back(SceneLoadSkip{
        JoinPath(instancePath, address), std::move(component), {},
        "'" + std::string(line) + "' names \"" + std::string(address) +
            "\", which the prefab does not have. Delete the line, or put the entity back in the prefab.",
        ownerFile, lineNo, /*preserved=*/false, root});
}

// True when the section changes a field of `component` at its address.
static bool HasFieldLinesFor(const SceneDoc::PrefabInstanceSection& section,
                             const SceneDoc::PrefabInstanceSection::AddressedComponent& component)
{
    for (const auto& fields : section.changedFields)
    {
        if (fields.address != component.address)
            continue;
        for (const auto& changed : fields.components)
        {
            if (changed.name == component.name)
                return true;
        }
    }
    return false;
}

// True when `memberAddress` is `address` or the address of an entity under it.
static bool IsAtOrUnder(std::string_view memberAddress, std::string_view address)
{
    return memberAddress.starts_with(address) &&
           (memberAddress.size() == address.size() || memberAddress[address.size()] == '.');
}

// Destroys the member of `placed` at `address` and every member under it, and moves them from
// `placed` to `removed`.
static void RemoveMemberAndItsChildren(ECS::World& world, PlacedPrefab& placed, std::string_view address,
                                       std::vector<std::pair<std::string, ECS::EntityHandle>>& removed)
{
    const auto first =
        std::stable_partition(placed.members.begin(), placed.members.end(),
                              [address](const auto& member) { return !IsAtOrUnder(member.first, address); });
    for (auto it = first; it != placed.members.end(); ++it)
    {
        world.DestroyEntityImmediate(it->second);
        removed.push_back(*it);
    }
    placed.members.erase(first, placed.members.end());
}

// Applies one [prefab] section's override lines to the instance `placed`, whose root is at
// `instancePath`: removed entities, removed components, added components, then changed fields. A
// line whose address names no entity, a removal of a component no build registers and a removal of a
// component the entity does not have are load-report rows, never a failure. The entities a "-entity"
// line removes go to `removed`.
static bool ApplyInstanceOverrides(ECS::World& world, const SceneLoadContext& ctx,
                                   const std::filesystem::path& ownerFile,
                                   const SceneDoc::PrefabInstanceSection& section, const std::string& instancePath,
                                   PlacedPrefab& placed,
                                   std::vector<std::pair<std::string, ECS::EntityHandle>>& removed,
                                   SceneLoadDegradation* degradation, const LoadFailure& failure)
{
    const std::size_t firstRow = degradation ? degradation->skips.size() : 0;

    for (const auto& removedEntity : section.removedEntities)
    {
        if (!FindMember(placed, removedEntity.address).IsValid())
        {
            RecordOrphanedOverride(degradation, instancePath, removedEntity.address, {},
                                   "-entity " + removedEntity.address, ownerFile, removedEntity.line, placed.root);
            continue;
        }
        RemoveMemberAndItsChildren(world, placed, removedEntity.address, removed);
    }

    for (const auto& component : section.removedComponents)
    {
        const ECS::EntityHandle h = FindMember(placed, component.address);
        const std::string text = "-" + Addressed(component.address, component.authoredName);
        if (!h.IsValid())
        {
            RecordOrphanedOverride(degradation, instancePath, component.address, component.authoredName, text,
                                   ownerFile, component.line, placed.root);
            continue;
        }
        const auto* schema = SceneSchemaRegistry::Find(component.name);
        if (!schema)
        {
            if (degradation)
                degradation->skips.push_back(SceneLoadSkip{
                    JoinPath(instancePath, component.address), component.authoredName, {},
                    "'" + text + "' removes a component no loaded module registers. Delete the line, or load the "
                                 "module that registers " + component.authoredName + ".",
                    ownerFile, component.line, /*preserved=*/false, h});
            continue;
        }
        if (!schema->IsPresent(world, h))
        {
            if (degradation)
                degradation->skips.push_back(SceneLoadSkip{
                    JoinPath(instancePath, component.address), component.authoredName, {},
                    "'" + text + "' removes a component the entity does not have. Delete the line.", ownerFile,
                    component.line, /*preserved=*/false, h});
            continue;
        }
        if (!schema->Remove(world, h))
            return failure.Fail(ownerFile, component.line,
                                "Component '" + component.authoredName + "' does not support removal");
    }

    for (const auto& component : section.addedComponents)
    {
        const ECS::EntityHandle h = FindMember(placed, component.address);
        if (!h.IsValid())
        {
            RecordOrphanedOverride(degradation, instancePath, component.address, component.authoredName,
                                   "+" + Addressed(component.address, component.authoredName),
                                   ownerFile, component.line, placed.root);
            continue;
        }
        // The component's field lines add it with their values below.
        if (HasFieldLinesFor(section, component))
            continue;
        const SceneDoc::ComponentProps block{component.name, component.authoredName, {}};
        const std::string rowId = JoinPath(instancePath, component.address);
        if (const auto* schema = SceneSchemaRegistry::Find(component.name))
            ApplyComponentBlock(world, ctx, *schema, h, block, rowId, ownerFile, component.line, degradation);
        else
            PreserveOrDropUnhandledComponent(world, h, block, rowId, ownerFile);
    }

    for (const auto& fields : section.changedFields)
    {
        const ECS::EntityHandle h = FindMember(placed, fields.address);
        const std::string rowId = JoinPath(instancePath, fields.address);
        for (const auto& comp : fields.components)
        {
            const int line = comp.props.empty() ? section.headerLine : comp.props.front().second.line;
            if (!h.IsValid())
            {
                RecordOrphanedOverride(degradation, instancePath, fields.address, comp.originalName,
                                       Addressed(fields.address, comp.originalName + "." +
                                                                     (comp.props.empty() ? std::string()
                                                                                         : comp.props.front().first)),
                                       ownerFile, line, placed.root);
                continue;
            }
            if (const auto* schema = SceneSchemaRegistry::Find(comp.name))
                ApplyComponentBlock(world, ctx, *schema, h, comp, rowId, ownerFile, line, degradation);
            else
                PreserveOrDropUnhandledComponent(world, h, comp, rowId, ownerFile);
        }
    }

    RecordUnreadLines(section.unreadLines, instancePath, ownerFile, placed.root, firstRow, degradation);
    return true;
}

// The address under a prefab's root of the entity `identity` names in the prefab's file: an entity
// id, an instance section's id, or an instance id, '.', and an address inside that instance. False
// when the name resolves to nothing in the file, or its parent chain loops.
static bool AddressOfIdentity(const SceneDoc& doc, std::string_view identity,
                              FlatMap<std::string, std::string>& addressByIdentity, FlatSet<std::string>& resolving,
                              std::string& outAddress)
{
    const std::string key(identity);
    if (const std::string* known = addressByIdentity.Find(key))
    {
        outAddress = *known;
        return true;
    }
    if (!resolving.Insert(key))
        return false;

    const std::optional<std::string>* parent = nullptr;
    std::string_view ownId;
    std::string_view rest;
    if (auto e = std::find_if(doc.entities.begin(), doc.entities.end(),
                              [identity](const auto& entity) { return entity.id == identity; });
        e != doc.entities.end())
    {
        parent = &e->parent;
        ownId = e->id;
    }
    else
    {
        const std::size_t dot = identity.find('.');
        const std::string_view sectionId = identity.substr(0, dot);
        auto b = std::find_if(doc.prefabInstances.begin(), doc.prefabInstances.end(),
                              [sectionId](const auto& instance) { return instance.id == sectionId; });
        if (b == doc.prefabInstances.end())
            return false;
        parent = &b->parent;
        ownId = b->id;
        if (dot != std::string_view::npos)
            rest = identity.substr(dot + 1);
    }

    std::string address;
    if (parent->has_value())
    {
        std::string parentAddress;
        if (!AddressOfIdentity(doc, **parent, addressByIdentity, resolving, parentAddress))
            return false;
        address = JoinPath(parentAddress, ownId);
    }
    outAddress = JoinPath(address, rest);
    addressByIdentity.InsertOrAssign(key, outAddress);
    resolving.Erase(key);
    return true;
}

// Places `prefab` at `instancePath` into the placement's world, in layers, a later one winning per
// field: a variant's base; the prefab's own entities; its nested instances with their override
// lines; the variant's override lines. Every entity gets its scene identity from its path.
static bool PlacePrefab(PrefabPlacement& placement, ParsedPrefab& prefab, const std::string& instancePath,
                        PlacedPrefab& out, const LoadFailure& failure)
{
    ECS::World& world = placement.world;
    const SceneDoc& doc = prefab.doc;

    // The file's own names for its entities: entity ids, instance ids, and "instance.address".
    std::unordered_map<std::string, ECS::EntityHandle> entityByIdentity;
    FlatMap<std::string, std::string> addressByIdentity;
    FlatSet<std::string> resolving;

    struct NestedInstance
    {
        const SceneDoc::PrefabInstanceSection* section = nullptr;
        std::string path;
        PlacedPrefab placed;
    };
    std::vector<NestedInstance> nested;
    nested.reserve(doc.prefabInstances.size());
    const SceneDoc::PrefabInstanceSection* variantRoot = nullptr;

    for (const auto& section : doc.prefabInstances)
    {
        std::string address;
        if (!AddressOfIdentity(doc, section.id, addressByIdentity, resolving, address))
            return failure.Fail(prefab.path, section.headerLine,
                                "Missing parent '" + section.parent.value_or("") + "' for prefab instance '" +
                                    section.id + "'");
        NestedInstance instance{&section, JoinPath(instancePath, address), {}};
        if (!PlaceInstanceSection(placement, prefab.ctx, prefab.path, doc, section, instance.path, instance.placed,
                                  failure))
            return false;
        if (!section.parent)
        {
            variantRoot = &section;
            out.root = instance.placed.root;
        }
        for (const auto& [memberAddress, h] : instance.placed.members)
        {
            out.members.emplace_back(JoinPath(address, memberAddress), h);
            entityByIdentity[JoinPath(section.id, memberAddress)] = h;
        }
        nested.push_back(std::move(instance));
    }

    std::vector<ECS::EntityHandle> created;
    created.reserve(doc.entities.size());
    std::vector<std::string> paths;
    paths.reserve(doc.entities.size());
    for (const auto& e : doc.entities)
    {
        std::string address;
        if (!AddressOfIdentity(doc, e.id, addressByIdentity, resolving, address))
            return failure.Fail(prefab.path, e.headerLine,
                                "Missing parent '" + e.parent.value_or("") + "' for entity '" + e.id + "'");
        const ECS::EntityHandle h = world.CreateEntity();
        if (!h.IsValid())
            return failure.Fail(prefab.path, e.headerLine, "Failed to create entity");
        if (TemplateBuildLog* log = placement.buildLog)
        {
            const auto file = std::find(log->files.begin(), log->files.end(), prefab.path);
            log->entities.push_back(h);
            log->origins.push_back({static_cast<std::uint32_t>(file - log->files.begin()), e.headerLine});
        }
        std::string path = JoinPath(instancePath, address);
        const std::string tag = SceneTagForPath(path);
        if (placement.writesSceneTags)
        {
            if (!ClaimHashedTag(placement, tag, path, prefab.path, e.headerLine, failure))
                return false;
            world.AddComponentImmediate(h, MakeTag(tag));
        }
        if (address.empty())
            out.root = h;
        out.members.emplace_back(std::move(address), h);
        entityByIdentity[e.id] = h;
        created.push_back(h);
        paths.push_back(std::move(path));
    }
    std::stable_partition(out.members.begin(), out.members.end(),
                          [](const auto& member) { return member.first.empty(); });

    // Parent links, now that every entity of the file and of its nested instances exists.
    for (std::size_t entityIndex = 0; entityIndex < doc.entities.size(); ++entityIndex)
    {
        const auto& parent = doc.entities[entityIndex].parent;
        if (!parent)
            continue;
        auto it = entityByIdentity.find(*parent);
        if (it == entityByIdentity.end())
            return failure.Fail(prefab.path, doc.entities[entityIndex].headerLine,
                                "Missing parent '" + *parent + "' for entity '" + doc.entities[entityIndex].id + "'");
        world.AddComponentImmediate(created[entityIndex], Components::Parent{it->second});
    }
    for (const auto& instance : nested)
    {
        if (!instance.section->parent)
            continue;
        auto it = entityByIdentity.find(*instance.section->parent);
        if (it == entityByIdentity.end())
            return failure.Fail(prefab.path, instance.section->headerLine,
                                "Missing parent '" + *instance.section->parent + "' for prefab instance '" +
                                    instance.section->id + "'");
        world.AddComponentImmediate(instance.placed.root, Components::Parent{it->second});
    }

    // The file's lines resolve entity references against the file's own names.
    prefab.ctx.EntityIdMap = &entityByIdentity;
    prefab.ctx.EntityIdPrefix.clear();
    for (std::size_t entityIndex = 0; entityIndex < doc.entities.size(); ++entityIndex)
    {
        const SceneDoc::Entity& e = doc.entities[entityIndex];
        // A member's fallback name is its id; the root's is the instance's own id.
        const std::string_view fallbackName = e.parent ? std::string_view(e.id) : LastIdOf(instancePath);
        ApplyEntityComponents(world, prefab.ctx, prefab.path, created[entityIndex], e, paths[entityIndex], fallbackName,
                              placement.degradation);
    }

    std::vector<std::pair<std::string, ECS::EntityHandle>> removed;
    bool applied = true;
    for (auto& instance : nested)
    {
        if (instance.section == variantRoot)
            continue;
        applied = ApplyInstanceOverrides(world, prefab.ctx, prefab.path, *instance.section, instance.path,
                                         instance.placed, removed, placement.degradation, failure);
        if (!applied)
            break;
    }
    if (applied && variantRoot)
        applied = ApplyInstanceOverrides(world, prefab.ctx, prefab.path, *variantRoot, instancePath, out, removed,
                                         placement.degradation, failure);
    prefab.ctx.EntityIdMap = nullptr;
    if (!applied)
        return false;

    std::erase_if(out.members, [&world](const auto& member) { return !world.IsValid(member.second); });
    return true;
}

// Places every [prefab] section of a scene (or of a prefab file loaded on its own) under the ids
// the scene's own lines use, applies each section's override lines, and links each instance root
// and each entity that waits on a member to its parent.
bool PlaceSceneInstances(PrefabPlacement& placement, SceneLoadContext& sceneCtx,
                         const std::filesystem::path& sceneFilePath, const SceneDoc& doc,
                         const std::string& idPrefix, Instantiated& out, const LoadFailure& failure)
{
    ECS::World& world = placement.world;
    sceneCtx.EntityIdMap = &out.idToEntity;
    sceneCtx.EntityIdPrefix = idPrefix;

    std::vector<std::pair<ECS::EntityHandle, const SceneDoc::PrefabInstanceSection*>> roots;
    roots.reserve(doc.prefabInstances.size());
    for (const auto& section : doc.prefabInstances)
    {
        const std::string instancePath = idPrefix + section.id;
        PlacedPrefab placed;
        if (!PlaceInstanceSection(placement, sceneCtx, sceneFilePath, doc, section, instancePath, placed, failure))
            return false;
        for (const auto& [address, h] : placed.members)
        {
            const std::string path = JoinPath(instancePath, address);
            const std::string tag = SceneTagForPath(path);
            if (tag != path)
                out.idToEntity[tag] = h;
            out.idToEntity[path] = h;
        }

        std::vector<std::pair<std::string, ECS::EntityHandle>> removed;
        if (!ApplyInstanceOverrides(world, sceneCtx, sceneFilePath, section, instancePath, placed, removed,
                                    placement.degradation, failure))
            return false;
        for (const auto& [address, h] : removed)
        {
            const std::string path = JoinPath(instancePath, address);
            out.idToEntity.erase(SceneTagForPath(path));
            out.idToEntity.erase(path);
        }
        for (const auto& member : placed.members)
            out.created.push_back(member.second);
        roots.emplace_back(placed.root, &section);
    }

    for (const auto& [root, section] : roots)
    {
        if (!section->parent)
            continue;
        out.pendingParents.push_back({root, idPrefix + *section->parent, sceneFilePath, section->headerLine});
    }
    for (const auto& pending : out.pendingParents)
    {
        auto it = out.idToEntity.find(pending.parentId);
        if (it == out.idToEntity.end())
            return failure.Fail(pending.file, pending.line, "Missing parent '" + pending.parentId + "'");
        world.AddComponentImmediate(pending.entity, Components::Parent{it->second});
    }
    out.pendingParents.clear();
    return true;
}

} // namespace GameEngine::Scene
