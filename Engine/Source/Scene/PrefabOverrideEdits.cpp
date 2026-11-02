// Apply and revert of one prefab instance override, and the propagation of a prefab file change to
// the instances of an open world. Both compare the lines the scene saver writes, as
// DescribePrefabInstance does, so an apply or a revert removes exactly the override it names.

#include "Scene/PrefabAuthoring.h"

#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/PrefabInstance.h"
#include "Components/SceneEntityTag.h"
#include "Core/Engine.h"
#include "ECS/EntityRemap.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Scene/AssetManagerSceneResolver.h"
#include "Scene/PrefabLibrary.h"
#include "Scene/PrefabOverrideWriter.h"
#include "Scene/SceneDoc.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scripting/AbiMainThread.h"
#include "Types/FlatMap.h"
#include "Types/FlatSet.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine::Scene
{

namespace
{

// One instance entity with the prefab entity it derives from, and the identities both sides write
// their entity fields with.
struct PrefabMember
{
    ECS::EntityHandle Root;
    ECS::EntityHandle Entity;
    std::string Address;
    std::size_t TemplateIndex = 0;
    std::unordered_map<ECS::EntityId, std::string> InstanceTags;
    std::unordered_map<ECS::EntityId, std::string> TemplateTags;
    std::unordered_map<std::string, ECS::EntityHandle> EntityByTag;
};

// An entity cloned into an instance for a prefab entity the instance did not have.
struct GainedMember
{
    ECS::EntityHandle Clone;
    std::size_t TemplateIndex = 0; // the index of its entity in the new generation
};

// The outermost prefab instance root at or above `entity`, or the invalid handle.
ECS::EntityHandle FindOutermostRoot(const ECS::World& world, ECS::EntityHandle entity)
{
    ECS::EntityHandle root;
    for (ECS::EntityHandle at = entity; at.IsValid() && world.IsValid(at);)
    {
        if (world.GetComponent<Components::PrefabInstance>(at))
            root = at;
        const auto* parent = world.GetComponent<Components::Parent>(at);
        at = parent ? parent->Entity : ECS::EntityHandle{};
    }
    return root;
}

// The index of `address` in the template's addresses, or the address count when it has none.
std::size_t FindAddress(const PrefabTemplate& prefab, std::string_view address)
{
    const auto it = std::find(prefab.Addresses.begin(), prefab.Addresses.end(), address);
    return static_cast<std::size_t>(it - prefab.Addresses.begin());
}

// The identities of the described instance entities both ways, and of the template entities as the
// instance at `instanceId` names them.
void CollectTags(const ECS::World& world, const std::vector<PrefabInstanceEntity>& described,
                 const PrefabTemplate& prefab, std::string_view instanceId,
                 std::unordered_map<ECS::EntityId, std::string>& instanceTags,
                 std::unordered_map<std::string, ECS::EntityHandle>& entityByTag,
                 std::unordered_map<ECS::EntityId, std::string>& templateTags)
{
    for (const PrefabInstanceEntity& entry : described)
    {
        if (const auto* tag = world.GetComponent<Components::SceneEntityTag>(entry.Entity))
        {
            instanceTags[entry.Entity.id] = std::string(tag->View());
            entityByTag[std::string(tag->View())] = entry.Entity;
        }
    }
    for (std::size_t index = 0; index < prefab.Addresses.size(); ++index)
        templateTags[prefab.Entities[index].id] = SceneTagForPath(JoinPath(instanceId, prefab.Addresses[index]));
}

// Finds the prefab entity `entity` derives from in the newest generation of its prefab. False, with
// the cause, when `entity` is no prefab entity.
bool FindMember(ECS::World& world, ECS::EntityHandle entity, PrefabLibrary& library, const PrefabTemplate*& outPrefab,
                PrefabMember& out, std::string& outError)
{
    out.Root = FindOutermostRoot(world, entity);
    const auto* marker = out.Root.IsValid() ? world.GetComponent<Components::PrefabInstance>(out.Root) : nullptr;
    const auto* rootTag = marker ? world.GetComponent<Components::SceneEntityTag>(out.Root) : nullptr;
    outPrefab = rootTag ? library.FindByGuid(marker->Source) : nullptr;
    if (!outPrefab)
    {
        outError = "The entity is not part of a prefab instance whose prefab is loaded.";
        return false;
    }
    std::vector<PrefabInstanceEntity> described;
    std::vector<std::string> addresses;
    DescribePrefabInstanceAgainst(world, out.Root, library, *outPrefab, described, addresses);
    const auto it = std::find_if(described.begin(), described.end(),
                                 [entity](const PrefabInstanceEntity& entry) { return entry.Entity == entity; });
    if (it == described.end() || it->AddedOnInstance)
    {
        outError = "The entity was added on this instance. The prefab does not have it.";
        return false;
    }
    out.Entity = entity;
    out.Address = addresses[static_cast<std::size_t>(it - described.begin())];
    out.TemplateIndex = FindAddress(*outPrefab, out.Address);
    CollectTags(world, described, *outPrefab, rootTag->View(), out.InstanceTags, out.EntityByTag, out.TemplateTags);
    return true;
}

const ComponentText* FindText(const std::vector<ComponentText>& texts, std::string_view key)
{
    const auto it = std::find_if(texts.begin(), texts.end(), [key](const ComponentText& text) { return text.key == key; });
    return it == texts.end() ? nullptr : &*it;
}

// The key of a "Component.field = value" line: the text before the first '=', without the spaces
// around it. A key has no '=', so a value with one keeps its key.
std::string_view LineKey(std::string_view line)
{
    const std::vector<std::string_view> fields = Split(line, '=');
    return fields.empty() ? std::string_view() : TrimWhitespaceView(fields.front());
}

// The line of `text` whose key is `key`, case-insensitively; null when it has none.
const std::string* FindLine(const std::vector<std::string>& lines, std::string_view key)
{
    const auto it = std::find_if(lines.begin(), lines.end(),
                                 [key](const std::string& line) { return EqualsIgnoreCase(LineKey(line), key); });
    return it == lines.end() ? nullptr : &*it;
}

std::string NotInPrefab(const PrefabOverride& prefabOverride)
{
    const std::string target = prefabOverride.Kind == PrefabOverrideKind::Field
                                   ? IdentifierToWords(prefabOverride.Component) + " " + IdentifierToWords(prefabOverride.Field)
                                   : IdentifierToWords(prefabOverride.Component);
    return std::format("{} is not in the prefab.", target);
}

// `file` as the disk spells it. The library keeps a prefab's path as its registry key, which is in
// lower case, and a write to that path gives the file the lower-case name. `file` when it does not exist.
std::filesystem::path SpelledOnDisk(const std::filesystem::path& file)
{
    std::error_code error;
    const std::filesystem::path onDisk = std::filesystem::canonical(file, error);
    return error ? file : onDisk;
}

// The scene contexts an edit writes and reads lines with.
struct LineContexts
{
    explicit LineContexts(ECS::World& world, const std::filesystem::path& file)
        : Engine(EngineCore::GetInstance())
        , Resolver(Engine.GetAssetManager(), Engine.GetResolvedAssetRoot(),
                   AssetManagerSceneResolver::UnknownFile::AssignGuid)
        , File(file)
    {
        Save.SceneFile = &File;
        Save.AssetRoot = Resolver.GetAssetRoot();
        Save.Resolver = &Resolver;
        Save.Unresolved = world.TryGetUnresolvedComponents();
        Save.WriteEveryField = true;
        Options.clearWorldFirst = false;
        Options.assetResolver = &Resolver;
        Options.assetRootOverride = Resolver.GetAssetRoot();
        Load = MakeDocumentLoadContext(world, File, SceneDoc{}, Options);
    }

    EngineCore& Engine;
    AssetManagerSceneResolver Resolver;
    std::filesystem::path File;
    SceneWriteContext Save;
    SceneReadOptions Options;
    SceneReadContext Load;
};

// Sets the fields `lines` name on `entity` through the component's schema, as a load reads them; adds
// the component when the entity does not have it.
void ApplyLines(ECS::World& world, const SceneReadContext& context, ECS::EntityHandle entity, std::string_view token,
                std::span<const std::string> lines)
{
    std::string text = "[prefab version=1]\n[entity id=\"edit\"]\n";
    for (const std::string& line : lines)
        text.append(line).append(1, '\n');
    SceneDoc doc;
    std::string error;
    if (!ParseDocument(*context.SceneFile, text, doc, &error) || doc.entities.empty())
    {
        Logger::Log::Error("Prefab edit: the lines of {} do not parse: {}", token, error);
        return;
    }
    for (const SceneDoc::ComponentProps& component : doc.entities.front().components)
    {
        if (const ISceneComponentSchema* schema = SceneSchemaRegistry::Find(component.name))
            ApplyComponentBlock(world, context, *schema, entity, component, {}, *context.SceneFile, 0, nullptr);
    }
}

// Removes the component `token` names from `entity`.
void RemoveComponent(ECS::World& world, ECS::EntityHandle entity, std::string_view token)
{
    const ISceneComponentSchema* schema = SceneSchemaRegistry::Find(ToLowerAscii(token));
    if (schema && schema->IsPresent(world, entity))
        schema->Remove(world, entity);
}

// Gives the instance entity the new prefab values it does not override: a field changed in the
// prefab where the instance keeps the old value, a component the prefab added, and a component the
// prefab removed. A component the instance removed stays removed. Returns true when it changed the entity.
bool PropagateToMember(ECS::World& world, const SceneReadContext& context, ECS::EntityHandle entity, bool isRoot,
                       const std::vector<ComponentText>& instance, const std::vector<ComponentText>& before,
                       const std::vector<ComponentText>& after)
{
    bool changedEntity = false;
    for (const ComponentText& next : after)
    {
        if (next.key == "hierarchyorder" || (isRoot && IsPlacementComponent(next.key)))
            continue;
        const ComponentText* previous = FindText(before, next.key);
        const ComponentText* current = FindText(instance, next.key);
        if (!previous)
        {
            if (!current)
            {
                ApplyLines(world, context, entity, next.token, next.lines);
                changedEntity = true;
            }
            continue;
        }
        if (!current)
            continue;
        std::vector<std::string> changed;
        for (const std::string& line : next.lines)
        {
            if (std::find(previous->lines.begin(), previous->lines.end(), line) != previous->lines.end())
                continue;
            const std::string_view key = LineKey(line);
            const std::string* previousLine = FindLine(previous->lines, key);
            const std::string* currentLine = FindLine(current->lines, key);
            if (!currentLine || (previousLine && *currentLine == *previousLine))
                changed.push_back(line);
        }
        if (!changed.empty())
        {
            ApplyLines(world, context, entity, next.token, changed);
            changedEntity = true;
        }
    }
    for (const ComponentText& previous : before)
    {
        if (previous.key != "hierarchyorder" && !FindText(after, previous.key) && FindText(instance, previous.key))
        {
            RemoveComponent(world, entity, previous.token);
            changedEntity = true;
        }
    }
    return changedEntity;
}

// Applies to `clone` each line of `after` that the text of the clone does not have. A clone holds the
// values of its prefab entity, so only its entity fields differ: they name the copy it came from.
void ApplyClonedEntityFields(ECS::World& world, const SceneReadContext& context, ECS::EntityHandle clone,
                             const std::vector<ComponentText>& current, const std::vector<ComponentText>& after)
{
    for (const ComponentText& next : after)
    {
        if (next.key == "hierarchyorder")
            continue;
        const ComponentText* held = FindText(current, next.key);
        std::vector<std::string> differing;
        for (const std::string& line : next.lines)
        {
            if (!held || std::find(held->lines.begin(), held->lines.end(), line) == held->lines.end())
                differing.push_back(line);
        }
        if (!differing.empty())
            ApplyLines(world, context, clone, next.token, differing);
    }
}

// Clones into the instance at `instanceId` every entity `after` has at an address `before` does not
// have, under the instance entity at its parent's address and with the identity of its address. A clone
// whose parent the instance removed is not kept. Records the identity of each clone in `entityByTag`.
// Returns each kept clone.
std::vector<GainedMember> AddGainedMembers(ECS::World& world, PrefabLibrary& library, std::string_view instanceId,
                                           const PrefabTemplate& before, const PrefabTemplate& after,
                                           const std::vector<PrefabInstanceEntity>& described,
                                           const std::vector<std::string>& addresses,
                                           std::unordered_map<std::string, ECS::EntityHandle>& entityByTag)
{
    std::vector<bool> gained(after.Addresses.size(), false);
    bool anyGained = false;
    for (std::size_t index = 0; index < after.Addresses.size(); ++index)
    {
        gained[index] = FindAddress(before, after.Addresses[index]) == before.Addresses.size() &&
                        !entityByTag.contains(SceneTagForPath(JoinPath(instanceId, after.Addresses[index])));
        anyGained = anyGained || gained[index];
    }
    if (!anyGained)
        return {};
    FlatMap<std::string, ECS::EntityHandle> memberByAddress;
    for (std::size_t index = 0; index < described.size(); ++index)
    {
        if (!described[index].AddedOnInstance)
            memberByAddress.InsertOrAssign(addresses[index], described[index].Entity);
    }
    ECS::EntityRemap remap;
    if (!world.CreateBatchFromWorld(library.GetWorld(), after.Plan, remap))
    {
        Logger::Log::Error("Prefab {}: the entities it gained could not be added to the instance \"{}\". Reload "
                           "the scene.",
                           after.Path.filename().string(), instanceId);
        return {};
    }
    const std::vector<ECS::EntityHandle> copy(remap.GetTargets().begin(), remap.GetTargets().end());
    for (std::size_t index = 0; index < copy.size(); ++index)
    {
        if (!gained[index])
            continue;
        const auto* parent = world.GetComponent<Components::Parent>(copy[index]);
        const auto parentAt = parent ? std::find(copy.begin(), copy.end(), parent->Entity) : copy.end();
        if (parentAt == copy.end() || gained[static_cast<std::size_t>(parentAt - copy.begin())])
            continue;
        const ECS::EntityHandle* member =
            memberByAddress.Find(after.Addresses[static_cast<std::size_t>(parentAt - copy.begin())]);
        if (member)
            world.GetComponentForWrite<Components::Parent>(copy[index])->Entity = *member;
    }
    Components::DestroySubtreeImmediate(world, copy.front());
    std::vector<GainedMember> kept;
    for (std::size_t index = 0; index < copy.size(); ++index)
    {
        if (!gained[index] || !world.IsValid(copy[index]))
            continue;
        const std::string tag = SceneTagForPath(JoinPath(instanceId, after.Addresses[index]));
        world.AddComponentImmediate(copy[index], MakeTag(tag));
        entityByTag[tag] = copy[index];
        kept.push_back({copy[index], index});
    }
    return kept;
}

// Destroys every member of the instance whose address `after` does not have, with its subtree. An entity
// the instance added under a destroyed member first moves to the nearest member that stays and keeps its
// world pose. Returns true when it destroyed a member.
bool RemoveLostMembers(ECS::World& world, const PrefabTemplate& after,
                       const std::vector<PrefabInstanceEntity>& described, const std::vector<std::string>& addresses)
{
    FlatSet<ECS::EntityHandle> lost;
    for (std::size_t index = 0; index < described.size(); ++index)
    {
        if (!described[index].AddedOnInstance && FindAddress(after, addresses[index]) == after.Addresses.size())
            lost.Insert(described[index].Entity);
    }
    if (lost.Empty())
        return false;
    for (const PrefabInstanceEntity& entry : described)
    {
        const auto* parent = entry.AddedOnInstance ? world.GetComponent<Components::Parent>(entry.Entity) : nullptr;
        if (!parent || !lost.Contains(parent->Entity))
            continue;
        ECS::EntityHandle keeper = parent->Entity;
        while (lost.Contains(keeper))
            keeper = world.GetComponent<Components::Parent>(keeper)->Entity;
        Components::ReparentKeepingWorldPose(world, entry.Entity, keeper);
    }
    for (const ECS::EntityHandle member : lost)
    {
        const auto* parent = world.GetComponent<Components::Parent>(member);
        if (!parent || !lost.Contains(parent->Entity))
            Components::DestroySubtreeImmediate(world, member);
    }
    return true;
}

// Gives the instance at `root` the change from the generation `before` of its prefab to `after`. Returns
// true when the instance changed.
bool PropagateToInstance(ECS::World& world, PrefabLibrary& library, ECS::EntityHandle root, std::string_view instanceId,
                         const PrefabTemplate& before, const PrefabTemplate& after)
{
    std::vector<PrefabInstanceEntity> described;
    std::vector<std::string> addresses;
    DescribePrefabInstanceAgainst(world, root, library, before, described, addresses);
    std::unordered_map<ECS::EntityId, std::string> instanceTags;
    std::unordered_map<ECS::EntityId, std::string> beforeTags;
    std::unordered_map<ECS::EntityId, std::string> afterTags;
    std::unordered_map<std::string, ECS::EntityHandle> entityByTag;
    CollectTags(world, described, before, instanceId, instanceTags, entityByTag, beforeTags);
    CollectTags(world, described, after, instanceId, instanceTags, entityByTag, afterTags);
    const std::vector<GainedMember> gained =
        AddGainedMembers(world, library, instanceId, before, after, described, addresses, entityByTag);

    LineContexts contexts(world, before.Path);
    contexts.Load.EntityIdMap = &entityByTag;
    SceneWriteContext instanceCtx = contexts.Save;
    instanceCtx.EntityTagById = &instanceTags;
    SceneWriteContext beforeCtx = contexts.Save;
    beforeCtx.EntityTagById = &beforeTags;
    beforeCtx.Unresolved = library.GetWorld().TryGetUnresolvedComponents();
    SceneWriteContext afterCtx = beforeCtx;
    afterCtx.EntityTagById = &afterTags;

    const auto schemas = SceneSchemaRegistry::GetAllSorted();
    std::vector<ComponentText> instanceText;
    std::vector<ComponentText> beforeText;
    std::vector<ComponentText> afterText;
    // A clone's entity fields name the copy it came from: read them again by identity, so that they name
    // the members of this instance.
    for (const GainedMember& member : gained)
    {
        CollectComponentText(world, member.Clone, instanceCtx, schemas, instanceText);
        CollectComponentText(library.GetWorld(), after.Entities[member.TemplateIndex], afterCtx, schemas, afterText);
        ApplyClonedEntityFields(world, contexts.Load, member.Clone, instanceText, afterText);
    }
    bool changed = !gained.empty();
    for (std::size_t index = 0; index < described.size(); ++index)
    {
        const PrefabInstanceEntity& entry = described[index];
        const std::string& address = addresses[index];
        if (entry.AddedOnInstance)
            continue;
        const std::size_t beforeIndex = FindAddress(before, address);
        const std::size_t afterIndex = FindAddress(after, address);
        if (beforeIndex == before.Addresses.size() || afterIndex == after.Addresses.size())
            continue;
        CollectComponentText(world, entry.Entity, instanceCtx, schemas, instanceText);
        CollectComponentText(library.GetWorld(), before.Entities[beforeIndex], beforeCtx, schemas, beforeText);
        CollectComponentText(library.GetWorld(), after.Entities[afterIndex], afterCtx, schemas, afterText);
        changed = PropagateToMember(world, contexts.Load, entry.Entity, address.empty(), instanceText, beforeText,
                                    afterText) ||
                  changed;
    }
    return RemoveLostMembers(world, after, described, addresses) || changed;
}

// The id on the section header line `header` ("[entity id=\"lid\" ...]"), or empty.
std::string_view HeaderId(std::string_view header)
{
    constexpr std::string_view kId = "id=\"";
    const std::size_t begin = header.find(kId);
    if (begin == std::string_view::npos)
        return {};
    const std::size_t end = header.find('"', begin + kId.size());
    return end == std::string_view::npos ? std::string_view() : header.substr(begin + kId.size(), end - begin - kId.size());
}

bool IsSectionHeader(std::string_view line)
{
    return TrimWhitespaceView(line).starts_with('[');
}

// The lines of one section of a scene file: the header at Header, the body up to End.
struct SectionLines
{
    std::vector<std::string> Lines;
    std::size_t Header = 0;
    std::size_t End = 0;

    // False when `headerLine` (1-based) is no section header of `text`.
    bool Read(const std::string& text, int headerLine)
    {
        for (std::string_view line : Split(text, '\n'))
            Lines.emplace_back(line);
        if (!Lines.empty() && Lines.back().empty())
            Lines.pop_back();
        if (headerLine < 1 || static_cast<std::size_t>(headerLine) > Lines.size() ||
            !IsSectionHeader(Lines[headerLine - 1]))
            return false;
        Header = static_cast<std::size_t>(headerLine - 1);
        End = Header + 1;
        while (End < Lines.size() && !IsSectionHeader(Lines[End]))
            ++End;
        return true;
    }

    // Puts `line` in place of the body line with its key, or after the last body line.
    void Set(const std::string& line)
    {
        const std::string_view key = LineKey(line);
        for (std::size_t index = Header + 1; index < End; ++index)
        {
            if (EqualsIgnoreCase(LineKey(Lines[index]), key))
            {
                Lines[index] = line;
                return;
            }
        }
        std::size_t at = End;
        while (at > Header + 1 && TrimWhitespaceView(Lines[at - 1]).empty())
            --at;
        Lines.insert(Lines.begin() + static_cast<std::ptrdiff_t>(at), line);
        ++End;
    }

    // Removes every body line of the component `token`.
    void RemoveComponent(std::string_view token)
    {
        const std::string prefix = std::string(token) + ".";
        for (std::size_t index = End; index-- > Header + 1;)
        {
            if (StartsWithIgnoreCase(Lines[index], prefix))
            {
                Lines.erase(Lines.begin() + static_cast<std::ptrdiff_t>(index));
                --End;
            }
        }
    }

    std::string Join() const
    {
        std::string text;
        for (const std::string& line : Lines)
            text.append(line).append(1, '\n');
        return text;
    }
};

// The identities the prefab file `fileIndex` of `prefab` gives its entities, by instance entity: the
// id on the header that authors each member in that file.
std::unordered_map<ECS::EntityId, std::string> CollectFileIds(ECS::World& world, ECS::EntityHandle root,
                                                              PrefabLibrary& library, const PrefabTemplate& prefab,
                                                              std::uint32_t fileIndex,
                                                              const std::vector<std::string>& fileLines)
{
    std::vector<PrefabInstanceEntity> described;
    std::vector<std::string> addresses;
    DescribePrefabInstanceAgainst(world, root, library, prefab, described, addresses);
    std::unordered_map<ECS::EntityId, std::string> ids;
    for (std::size_t entryIndex = 0; entryIndex < described.size(); ++entryIndex)
    {
        const PrefabInstanceEntity& entry = described[entryIndex];
        const std::size_t index =
            entry.AddedOnInstance ? prefab.Addresses.size() : FindAddress(prefab, addresses[entryIndex]);
        if (index == prefab.Addresses.size())
            continue;
        const PrefabTemplate::Origin& origin = prefab.Origins[index];
        if (origin.File == fileIndex && origin.Line >= 1 && static_cast<std::size_t>(origin.Line) <= fileLines.size())
            ids[entry.Entity.id] = std::string(HeaderId(fileLines[origin.Line - 1]));
    }
    return ids;
}

} // namespace

bool PropagatePrefabGeneration(ECS::World& world, PrefabLibrary& library, const PrefabTemplate& before,
                               const PrefabTemplate& after)
{
    if (after.Source.IsNull())
        return false;
    std::vector<std::pair<ECS::EntityHandle, std::string>> roots;
    world.Query<ECS::Read<Components::PrefabInstance>, ECS::Read<Components::SceneEntityTag>>()
        .IncludeDisabled()
        .Each(
            [&roots, &after](ECS::EntityHandle entity, const Components::PrefabInstance& marker,
                             const Components::SceneEntityTag& tag)
            {
                if (marker.Source == after.Source && !IsMemberTag(tag.View()))
                    roots.emplace_back(entity, std::string(tag.View()));
            });
    bool changed = false;
    for (const auto& [root, instanceId] : roots)
        changed = PropagateToInstance(world, library, root, instanceId, before, after) || changed;
    return changed;
}

bool RevertPrefabOverride(ECS::World& world, ECS::EntityHandle entity, const PrefabOverride& prefabOverride,
                          std::string& outError)
{
    EngineCore& engine = EngineCore::GetInstance();
    PrefabLibrary* library = engine.TryGetPrefabLibrary();
    assert(ScriptingAbi::IsMainThreadOutsideJobFor(engine) && "Scene::RevertPrefabOverride runs on the main thread only");
    const PrefabTemplate* prefab = nullptr;
    PrefabMember member;
    if (!library || !FindMember(world, entity, *library, prefab, member, outError))
        return false;

    if (prefabOverride.Kind == PrefabOverrideKind::AddedComponent)
    {
        RemoveComponent(world, entity, prefabOverride.Component);
        return true;
    }
    if (prefabOverride.Kind != PrefabOverrideKind::Field && prefabOverride.Kind != PrefabOverrideKind::RemovedComponent)
    {
        outError = "Only a field, an added component and a removed component revert from the Inspector.";
        return false;
    }

    LineContexts contexts(world, prefab->Path);
    contexts.Load.EntityIdMap = &member.EntityByTag;
    SceneWriteContext templateCtx = contexts.Save;
    templateCtx.EntityTagById = &member.TemplateTags;
    templateCtx.Unresolved = library->GetWorld().TryGetUnresolvedComponents();
    std::vector<ComponentText> templateText;
    CollectComponentText(library->GetWorld(), prefab->Entities[member.TemplateIndex], templateCtx,
                         SceneSchemaRegistry::GetAllSorted(), templateText);

    const ComponentText* component = FindText(templateText, ToLowerAscii(prefabOverride.Component));
    if (!component)
    {
        outError = NotInPrefab(prefabOverride);
        return false;
    }
    if (prefabOverride.Kind == PrefabOverrideKind::RemovedComponent)
    {
        ApplyLines(world, contexts.Load, entity, component->token, component->lines);
        return true;
    }
    const std::string* line = FindLine(component->lines, component->token + "." + prefabOverride.Field);
    if (!line)
    {
        outError = NotInPrefab(prefabOverride);
        return false;
    }
    ApplyLines(world, contexts.Load, entity, component->token, std::span(line, 1));
    return true;
}

PrefabTemplateEntity FindPrefabTemplateEntity(ECS::World& world, ECS::EntityHandle entity)
{
    EngineCore& engine = EngineCore::GetInstance();
    PrefabLibrary* library = engine.TryGetPrefabLibrary();
    assert(ScriptingAbi::IsMainThreadOutsideJobFor(engine) &&
           "Scene::FindPrefabTemplateEntity runs on the main thread only");
    const PrefabTemplate* prefab = nullptr;
    PrefabMember member;
    std::string error;
    if (!library || !FindMember(world, entity, *library, prefab, member, error))
        return {};
    return {&library->GetWorld(), prefab->Entities[member.TemplateIndex]};
}

PrefabTemplateEntity FindPrefabTemplateEntityAt(ECS::World& world, ECS::EntityHandle entity, std::string_view address)
{
    EngineCore& engine = EngineCore::GetInstance();
    PrefabLibrary* library = engine.TryGetPrefabLibrary();
    assert(ScriptingAbi::IsMainThreadOutsideJobFor(engine) &&
           "Scene::FindPrefabTemplateEntityAt runs on the main thread only");
    const ECS::EntityHandle root = FindOutermostRoot(world, entity);
    const auto* marker = root.IsValid() ? world.GetComponent<Components::PrefabInstance>(root) : nullptr;
    const PrefabTemplate* prefab = library && marker ? library->FindByGuid(marker->Source) : nullptr;
    if (!prefab)
        return {};
    const std::size_t index = FindAddress(*prefab, address);
    if (index >= prefab->Entities.size())
        return {};
    return {&library->GetWorld(), prefab->Entities[index]};
}

namespace
{

// One target of an apply, with the prefab entity it names.
struct PlannedApply
{
    const PrefabTemplate* Prefab = nullptr;
    PrefabMember Member;
    const PrefabOverride* Override = nullptr;
    std::filesystem::path File;
    int Line = 0;
};

// A prefab file that an apply writes: its text on disk, and its text with the overrides put in so far.
struct PendingPrefabFile
{
    std::filesystem::path File;
    std::string Before;
    std::string Text;
};

// Puts the override of `planned` into `text`, the text of the file that authors its entity.
bool PutOverrideInText(ECS::World& world, PrefabLibrary& library, const PlannedApply& planned, std::string& text,
                       std::string& outError)
{
    const PrefabOverride& prefabOverride = *planned.Override;
    const PrefabMember& member = planned.Member;
    const PrefabTemplate& prefab = *planned.Prefab;
    SectionLines section;
    if (!section.Read(text, planned.Line))
    {
        outError = std::format("Cannot read the prefab {}. Check that the file exists and is not locked.",
                               SpelledOnDisk(planned.File).filename().string());
        return false;
    }

    // The entity fields write as the identities the prefab file gives its entities.
    const std::unordered_map<ECS::EntityId, std::string> fileIds = CollectFileIds(
        world, member.Root, library, prefab, prefab.Origins[member.TemplateIndex].File, section.Lines);
    LineContexts contexts(world, planned.File);
    SceneWriteContext instanceCtx = contexts.Save;
    instanceCtx.EntityTagById = &fileIds;
    SceneWriteContext templateCtx = contexts.Save;
    templateCtx.EntityTagById = &member.TemplateTags;
    templateCtx.Unresolved = library.GetWorld().TryGetUnresolvedComponents();
    const auto schemas = SceneSchemaRegistry::GetAllSorted();
    std::vector<ComponentText> instanceText;
    std::vector<ComponentText> templateText;
    CollectComponentText(world, member.Entity, instanceCtx, schemas, instanceText);
    CollectComponentText(library.GetWorld(), prefab.Entities[member.TemplateIndex], templateCtx, schemas,
                         templateText);

    const std::string key = ToLowerAscii(prefabOverride.Component);
    const ComponentText* instanceComponent = FindText(instanceText, key);
    const ComponentText* templateComponent = FindText(templateText, key);
    switch (prefabOverride.Kind)
    {
        case PrefabOverrideKind::Field:
        {
            const std::string* line =
                instanceComponent ? FindLine(instanceComponent->lines, instanceComponent->token + "." + prefabOverride.Field)
                                  : nullptr;
            if (!line || !templateComponent)
            {
                outError = NotInPrefab(prefabOverride);
                return false;
            }
            section.Set(*line);
            break;
        }
        case PrefabOverrideKind::AddedComponent:
            if (!instanceComponent)
            {
                outError = std::format("The entity does not have {}.", IdentifierToWords(prefabOverride.Component));
                return false;
            }
            for (const std::string& line : instanceComponent->lines)
                section.Set(line);
            break;
        case PrefabOverrideKind::RemovedComponent:
            if (!templateComponent)
            {
                outError = NotInPrefab(prefabOverride);
                return false;
            }
            // A prefab entity without a Name line takes its id as its name.
            if (key == "name")
            {
                outError = "Every prefab entity has a name, so the prefab cannot remove Name. Use Revert to restore the name.";
                return false;
            }
            section.RemoveComponent(templateComponent->token);
            break;
        case PrefabOverrideKind::RemovedChild:
            break;
    }
    text = section.Join();
    return true;
}

// Edits a file's sections bottom first: an edit changes the line count of its own section only, so each header
// stays at the line the template read it at.
bool IsEditedBefore(const PlannedApply& a, const PlannedApply& b)
{
    return a.File != b.File ? a.File < b.File : a.Line > b.Line;
}

} // namespace

bool ApplyPrefabOverrides(ECS::World& world, std::span<const PrefabOverrideTarget> targets,
                          std::vector<PrefabFileEdit>& outEdits, std::string& outError)
{
    EngineCore& engine = EngineCore::GetInstance();
    PrefabLibrary* library = engine.TryGetPrefabLibrary();
    assert(ScriptingAbi::IsMainThreadOutsideJobFor(engine) && "Scene::ApplyPrefabOverrides runs on the main thread only");
    if (!library)
    {
        outError = "Scene::ApplyPrefabOverrides needs an initialized engine: call EngineCore::Initialize first.";
        return false;
    }
    std::vector<PlannedApply> plan;
    plan.reserve(targets.size());
    for (const PrefabOverrideTarget& target : targets)
    {
        PlannedApply& planned = plan.emplace_back();
        if (!FindMember(world, target.Entity, *library, planned.Prefab, planned.Member, outError))
            return false;
        if (target.Override.Kind == PrefabOverrideKind::RemovedChild)
        {
            outError = "Only a field, an added component and a removed component apply from the Inspector.";
            return false;
        }
        const PrefabTemplate::Origin& origin = planned.Prefab->Origins[planned.Member.TemplateIndex];
        planned.Override = &target.Override;
        planned.File = planned.Prefab->Files[origin.File];
        planned.Line = origin.Line;
    }
    std::stable_sort(plan.begin(), plan.end(), IsEditedBefore);

    std::vector<PendingPrefabFile> files;
    for (const PlannedApply& planned : plan)
    {
        if (files.empty() || files.back().File != planned.File)
        {
            PendingPrefabFile& file = files.emplace_back();
            file.File = planned.File;
            if (!ReadTextFile(file.File, file.Before))
            {
                outError = std::format("Cannot read the prefab {}. Check that the file exists and is not locked.",
                                       SpelledOnDisk(file.File).filename().string());
                return false;
            }
            file.Text = file.Before;
        }
        if (!PutOverrideInText(world, *library, planned, files.back().Text, outError))
            return false;
    }

    std::vector<std::filesystem::path> written;
    bool wroteAll = true;
    for (PendingPrefabFile& file : files)
    {
        const std::filesystem::path onDisk = SpelledOnDisk(file.File);
        if (!WriteTextFileAtomic(onDisk, file.Text))
        {
            outError = std::format("Cannot write the prefab {}. Check that the file is not read-only or locked.",
                                   onDisk.filename().string());
            wroteAll = false;
            break;
        }
        written.push_back(file.File);
        outEdits.push_back({std::move(file.File), std::move(file.Before), std::move(file.Text)});
    }
    // One rebuild for all written files, so each instance takes the new prefab values once.
    library->RebuildFiles(written);
    return wroteAll;
}

bool WritePrefabFileText(const std::filesystem::path& file, const std::string& text, std::string& outError)
{
    EngineCore& engine = EngineCore::GetInstance();
    PrefabLibrary* library = engine.TryGetPrefabLibrary();
    assert(ScriptingAbi::IsMainThreadOutsideJobFor(engine) && "Scene::WritePrefabFileText runs on the main thread only");
    if (!library)
    {
        outError = "Scene::WritePrefabFileText needs an initialized engine: call EngineCore::Initialize first.";
        return false;
    }
    const std::filesystem::path onDisk = SpelledOnDisk(file);
    if (!WriteTextFileAtomic(onDisk, text))
    {
        outError = std::format("Cannot write the prefab {}. Check that the file is not read-only or locked.",
                               onDisk.filename().string());
        return false;
    }
    library->RebuildFiles(std::span(&file, 1));
    return true;
}

} // namespace GameEngine::Scene
