#include "Scene/PrefabOverrideWriter.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/PrefabInstance.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/UnresolvedComponentStore.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Types/StringUtils.h"

#include <algorithm>

namespace GameEngine::Scene
{

// One component of an entity as the scene saver writes it: its file token and its lines.
struct ComponentText
{
    std::string token;
    std::string key; // the token lowercased
    std::vector<std::string> lines;
};

// The lines the scene saver writes for `entity`, grouped by component: the hand-written schemas',
// the reflected components', the switch lines of components switched off, the components no build
// registers, and the authored text of fields the load could not read.
static void CollectComponentText(ECS::World& world, ECS::EntityHandle entity, const SceneSaveContext& ctx,
                                 const std::vector<const ISceneComponentSchema*>& schemas,
                                 std::vector<ComponentText>& out)
{
    out.clear();
    const auto add = [&out](std::string_view token) -> ComponentText& {
        ComponentText& text = out.emplace_back();
        text.token = std::string(token);
        text.key = ToLowerAscii(text.token);
        return text;
    };
    for (const auto* schema : schemas)
    {
        if (schema && schema->IsPresent(world, entity))
            schema->Serialize(world, entity, ctx, add(schema->GetComponentName()).lines);
    }
    auto* arch = world.GetEntityArchetype(entity);
    if (arch)
    {
        const auto& signature = arch->GetSignature();
        for (ECS::ComponentTypeId typeId : signature.GetComponents())
        {
            if (const ISceneComponentSchema* s = SceneSchemaRegistry::ReflectionSchemaForUnhandledType(typeId))
                s->Serialize(world, entity, ctx, add(s->GetComponentName()).lines);
        }
        for (ECS::ComponentTypeId typeId : signature.GetComponents())
        {
            const ECS::ComponentRegistry::ComponentInfo* info = ECS::ComponentRegistry::GetComponentInfo(typeId);
            if (!info || !signature.Contains(ECS::ComponentDisabledTypeId(info->Name)) ||
                !ECS::ComponentRegistry::SwitchesThroughDisabledTag(typeId))
                continue;
            const std::string_view token = SceneSchemaRegistry::SchemaTokenForComponentType(typeId);
            if (token.empty())
                continue;
            const std::string key = ToLowerAscii(std::string(token));
            auto it = std::find_if(out.begin(), out.end(), [&key](const ComponentText& t) { return t.key == key; });
            ComponentText& text = it != out.end() ? *it : add(token);
            text.lines.insert(text.lines.begin(), std::string(token) + ".enabled = false");
        }
    }
    if (const ECS::UnresolvedComponentStore* store = world.TryGetUnresolvedComponents())
    {
        if (auto it = store->Map().find(entity); it != store->Map().end())
        {
            for (const ECS::PreservedComponent& pc : it->second)
            {
                ComponentText& text = add(pc.Name);
                for (const auto& [prop, value] : pc.Props)
                    text.lines.push_back(pc.Name + "." + prop + " = " + value);
            }
        }
    }
    for (ComponentText& text : out)
        ApplyPreservedFieldOverrides(world, entity, text.lines);
}

// The components of an instance's root that place it in the scene rather than override its prefab:
// written on every instance, whatever the prefab holds.
static bool IsPlacementComponent(std::string_view key)
{
    return key == "transform" || key == "name" || key == "hierarchyorder";
}

static std::vector<ComponentText>::const_iterator FindComponentText(const std::vector<ComponentText>& texts,
                                                                     const std::string& key)
{
    return std::find_if(texts.begin(), texts.end(), [&key](const ComponentText& t) { return t.key == key; });
}

// True when the instance entity has a component its template entity does not.
static bool AddsAComponent(const std::vector<ComponentText>& instance, const std::vector<ComponentText>& templateText)
{
    return std::any_of(instance.begin(), instance.end(), [&templateText](const ComponentText& text) {
        return FindComponentText(templateText, text.key) == templateText.end();
    });
}

// The override lines of the instance entity at `address` ("" for the root): every line that differs
// from the template entity's, "+address/Component" and its lines for a component the template does
// not have, "-address/Component" for one the instance does not have. `instance` and `templateText`
// hold every field (SceneSaveContext::WriteEveryField), so a field the instance clears differs from
// the template's line; `added` is the instance's plain text, which an added component writes, and is
// empty when the instance adds none. Sibling order is the prefab's, so a member's HierarchyOrder is
// never written.
static void WriteEntityOverrides(std::string_view address, const std::vector<ComponentText>& instance,
                                 const std::vector<ComponentText>& added,
                                 const std::vector<ComponentText>& templateText, std::string& out)
{
    const bool isRoot = address.empty();

    for (const ComponentText& text : instance)
    {
        if (!isRoot && text.key == "hierarchyorder")
            continue;
        const auto templateIt = FindComponentText(templateText, text.key);
        if (isRoot && IsPlacementComponent(text.key))
        {
            for (const auto& line : text.lines)
                out.append(line).append(1, '\n');
            continue;
        }
        if (templateIt == templateText.end())
        {
            out.append("+").append(Addressed(address, text.token)).append(1, '\n');
            for (const auto& line : FindComponentText(added, text.key)->lines)
                out.append(Addressed(address, line)).append(1, '\n');
            continue;
        }
        for (const auto& line : text.lines)
        {
            if (std::find(templateIt->lines.begin(), templateIt->lines.end(), line) == templateIt->lines.end())
                out.append(Addressed(address, line)).append(1, '\n');
        }
        // A component the prefab switches off and the instance switches on writes its switch.
        const std::string switchedOff = templateIt->token + ".enabled = false";
        if (std::find(templateIt->lines.begin(), templateIt->lines.end(), switchedOff) != templateIt->lines.end() &&
            std::find(text.lines.begin(), text.lines.end(), text.token + ".enabled = false") == text.lines.end())
            out.append(Addressed(address, text.token + ".enabled = true")).append(1, '\n');
    }
    for (const ComponentText& text : templateText)
    {
        if (text.key == "hierarchyorder")
            continue;
        if (FindComponentText(instance, text.key) == instance.end())
            out.append("-").append(Addressed(address, text.token)).append(1, '\n');
    }
}

// The template of the prefab at `prefabPath`, placed on first use. Null, with the message, when the
// prefab cannot be placed.
static const PrefabTemplates::Template* GetPrefabTemplate(PrefabTemplates& templates,
                                                          const std::filesystem::path& sceneFilePath,
                                                          const std::filesystem::path& prefabPath,
                                                          std::string& outError, std::filesystem::path& outFile,
                                                          int& outLine)
{
    const std::string key = prefabPath.lexically_normal().generic_string();
    if (auto* found = templates.byPath.Find(key))
        return found->get();
    if (!templates.world)
    {
        templates.world = std::make_unique<ECS::World>();
        templates.placement = std::make_unique<PrefabPlacement>(*templates.world, templates.loadOptions, nullptr);
    }

    // The template is placed as a scene that holds one instance of the prefab would place it.
    SceneDoc owner{};
    owner.resources["prefab"] = SceneDoc::Resource{"prefab", prefabPath.generic_string(), {}, 0};
    SceneDoc::PrefabInstanceSection section{};
    section.id = "template_" + std::to_string(templates.byPath.Size());
    section.sourceResourceId = "prefab";
    const SceneLoadContext ownerCtx =
        MakeDocumentLoadContext(*templates.world, sceneFilePath, owner, templates.loadOptions);

    auto made = std::make_unique<PrefabTemplates::Template>();
    if (!PlaceInstanceSection(*templates.placement, ownerCtx, sceneFilePath, owner, section, section.id, made->placed,
                              LoadFailure{&outError, &outFile, &outLine}))
        return nullptr;
    for (const auto& [address, h] : made->placed.members)
        made->byAddress.InsertOrAssign(address, h);
    return templates.byPath.InsertOrAssign(key, std::move(made)).get();
}

// Gives `h` a scene-level id from its name when its tag cannot be one (it holds '.', '/' or '~': an
// entity that left its prefab instance, or a member written as an entity), unique in `usedTags`.
void AssignSceneLevelIdIfNeeded(ECS::World& world, ECS::EntityHandle h,
                                std::unordered_map<ECS::EntityId, std::string>& tagByEntity,
                                std::unordered_set<std::string>& usedTags)
{
    auto it = tagByEntity.find(h.id);
    if (it == tagByEntity.end() || it->second.find_first_of(kIdSeparators) == std::string::npos)
        return;
    std::string preferred;
    if (const auto* name = world.GetComponent<Components::Name>(h))
        preferred = SlugifyDisplayNameForSceneEntityId(name->View());
    if (preferred.empty())
        preferred = SlugifyDisplayNameForSceneEntityId(LastIdOf(it->second));
    it->second = AllocateUniqueSceneEntityId(preferred, usedTags);
    world.AddComponentImmediate(h, MakeTag(it->second));
}

// Sorts the scene's entities into prefab instances and the rest. An entity is a member of the
// outermost instance above it when its parent is that instance's root or a member, its tag is a
// member tag, and the address its parent chain gives it is one the prefab has and no other member
// took; its tag is then written again from its path. Any other entity is the scene's own (one added
// on an instance among them) and keeps a scene-level id. Members and roots go to `excluded`.
bool CollectPrefabInstances(ECS::World& world, const std::vector<ECS::EntityHandle>& entities,
                            const SaveOptions& options, const SceneSaveContext& saveCtx,
                            const std::filesystem::path& sceneFilePath,
                            std::unordered_map<ECS::EntityId, std::string>& tagByEntity,
                            std::unordered_set<ECS::EntityId>& excluded, PrefabTemplates& templates,
                            std::vector<SavedPrefabInstance>& instances)
{
    std::unordered_set<std::string> usedTags;
    usedTags.reserve(tagByEntity.size());
    for (const auto& [id, tag] : tagByEntity)
        usedTags.insert(tag);
    std::unordered_map<ECS::EntityId, std::pair<std::size_t, std::string>> placeById; // instance, address
    FlatMap<std::string, std::string> pathByHashedTag;

    for (const ECS::EntityHandle h : entities)
    {
        if (excluded.count(h.id) > 0)
            continue;
        const auto* parent = world.GetComponent<Components::Parent>(h);
        const auto parentPlace = parent && parent->Entity.IsValid() ? placeById.find(parent->Entity.id) : placeById.end();
        const auto tagIt = tagByEntity.find(h.id);
        if (options.preservePrefabInstances && parentPlace != placeById.end() && tagIt != tagByEntity.end() &&
            IsMemberTag(tagIt->second))
        {
            SavedPrefabInstance& instance = instances[parentPlace->second.first];
            std::string address = JoinPath(parentPlace->second.second, LastIdOf(tagIt->second));
            const std::string path = JoinPath(instance.id, address);
            std::string tag = SceneTagForPath(path);
            // The entity that carries the identity the path gives keeps the address; another one
            // there (dragged in from an instance of the same prefab) is an added entity.
            const bool ownsAddress = tagIt->second == tag || usedTags.count(tag) == 0;
            if (ownsAddress && instance.templ->byAddress.Contains(address) && instance.addresses.Insert(address))
            {
                if (tag != path)
                {
                    if (const std::string* claimed = pathByHashedTag.Find(tag); claimed && *claimed != path)
                    {
                        SetError(sceneFilePath, 0,
                                 "Cannot save: two entities get the scene identity \"" + tag + "\": \"" + *claimed +
                                     "\" and \"" + path + "\". Shorten an id in one of the chains.");
                        return false;
                    }
                    pathByHashedTag.InsertOrAssign(tag, path);
                }
                if (tagIt->second != tag)
                {
                    tagIt->second = tag;
                    world.AddComponentImmediate(h, MakeTag(tag));
                }
                instance.members.emplace_back(address, h);
                placeById[h.id] = {parentPlace->second.first, std::move(address)};
                excluded.insert(h.id);
                continue;
            }
        }

        AssignSceneLevelIdIfNeeded(world, h, tagByEntity, usedTags);
        const auto* marker = world.GetComponent<Components::PrefabInstance>(h);
        if (!options.preservePrefabInstances || !marker)
            continue;

        SavedPrefabInstance instance{};
        instance.h = h;
        instance.id = tagByEntity.count(h.id) ? tagByEntity[h.id] : ("e_" + std::to_string(h.id));
        if (!ResolvePrefabSourceForSave(saveCtx, marker->Source, instance.sourcePath))
        {
            SetError(sceneFilePath, 0,
                     "Cannot save prefab instance '" + instance.id +
                         "': the asset database does not know its source prefab. Save the scene "
                         "from the project that holds the prefab, reimport the prefab, or delete the instance.");
            return false;
        }
        instance.sourceGuid = marker->Source.ToString();
        std::filesystem::path prefabPath;
        AssetType type = AssetType::Unknown;
        saveCtx.Resolver->TryGetPathAndType(saveCtx.Resolver->ResolveGuid(marker->Source), prefabPath, type);
        std::string error;
        std::filesystem::path errorFile;
        int errorLine = 0;
        instance.templ = GetPrefabTemplate(templates, sceneFilePath, prefabPath, error, errorFile, errorLine);
        if (!instance.templ)
        {
            SetError(errorFile.empty() ? sceneFilePath : errorFile, errorLine,
                     "Cannot save prefab instance '" + instance.id + "': " + error);
            return false;
        }
        if (parent && parent->Entity.IsValid())
        {
            if (auto it = tagByEntity.find(parent->Entity.id); it != tagByEntity.end())
                instance.parentId = it->second;
            if (parentPlace != placeById.end())
                instance.parentInstance = parentPlace->second.first;
        }
        instance.members.emplace_back(std::string(), h);
        instance.addresses.Insert(std::string());
        placeById[h.id] = {instances.size(), std::string()};
        excluded.insert(h.id);
        instances.push_back(std::move(instance));
    }
    return true;
}

// Writes the body of one instance's [prefab] section: the root's placement and its overrides, each
// member's overrides at its address, then "-entity address" for each part of the prefab the
// instance no longer has.
void WritePrefabInstanceBody(ECS::World& world, const SavedPrefabInstance& instance,
                             const PrefabTemplates& templates, const SceneSaveContext& saveCtx,
                             const std::vector<const ISceneComponentSchema*>& schemas, std::string& out)
{
    // A template's entity references write as the identities the instance's entities have.
    std::unordered_map<ECS::EntityId, std::string> templateTags;
    templateTags.reserve(instance.templ->placed.members.size());
    for (const auto& [address, h] : instance.templ->placed.members)
        templateTags[h.id] = SceneTagForPath(JoinPath(instance.id, address));
    SceneSaveContext templateCtx = saveCtx;
    templateCtx.EntityTagById = &templateTags;
    templateCtx.Unresolved = templates.world->TryGetUnresolvedComponents();
    templateCtx.WriteEveryField = true;
    SceneSaveContext instanceCtx = saveCtx;
    instanceCtx.WriteEveryField = true;

    std::vector<ComponentText> instanceText;
    std::vector<ComponentText> addedText;
    std::vector<ComponentText> templateText;
    for (const auto& [address, h] : instance.members)
    {
        CollectComponentText(world, h, instanceCtx, schemas, instanceText);
        CollectComponentText(*templates.world, *instance.templ->byAddress.Find(address), templateCtx, schemas,
                             templateText);
        addedText.clear();
        if (AddsAComponent(instanceText, templateText))
            CollectComponentText(world, h, saveCtx, schemas, addedText);
        WriteEntityOverrides(address, instanceText, addedText, templateText, out);
    }

    for (const auto& [address, h] : instance.templ->placed.members)
    {
        if (address.empty() || instance.addresses.Contains(address))
            continue;
        const std::size_t dot = address.rfind('.');
        const std::string parentAddress = dot == std::string::npos ? std::string() : address.substr(0, dot);
        if (instance.addresses.Contains(parentAddress))
            out.append("-entity ").append(address).append(1, '\n');
    }
}

} // namespace GameEngine::Scene
