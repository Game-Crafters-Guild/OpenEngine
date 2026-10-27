#pragma once

// The scene file's document model and the steps of the scene loader and saver (SceneIO.cpp) that the
// prefab placement (PrefabPlacement.cpp) and the prefab override writer (PrefabOverrideWriter.cpp)
// share. Private to the scene IO sources.

#include "Components/SceneEntityTag.h"
#include "ECS/Entity.h"
#include "Mathematics/Vector3.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneIOContext.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Scene
{

class ISceneComponentSchema;

// The characters an entity id never holds: '.' joins ids in a scene identity and in an address,
// '/' separates an address from its component line, and '~' separates the hash from the local id
// in a long scene identity.
inline constexpr std::string_view kIdSeparators = "./~";

struct SceneDoc
{
    bool isPrefab = false;
    std::string name;
    int version = 0;

    struct PropValue
    {
        std::string value;
        int line = 0;
        // Authored spelling of the property key (keys are lowercased for schema
        // matching); empty means "same as the lowered key". The load report and
        // the unknown-component preservation path echo this back.
        std::string originalKey;
    };

    // Insertion-ordered flat storage of a component's properties. Replaces a nested
    // unordered_map; the parse path only ever appends and the load/save/validate paths
    // only ever range-iterate, so a vector keeps file order deterministic and avoids the
    // per-entity, per-component map allocations.
    // components: [ { "transform", [ {"position", {"(1, 2, 3)", line}}, ... ] }, ... ]
    struct ComponentProps
    {
        std::string name;
        // Authored spelling of the component name (see PropValue::originalKey).
        std::string originalName;
        std::vector<std::pair<std::string, PropValue>> props;
    };
    using ComponentProperties = std::vector<ComponentProps>;

    struct Include
    {
        std::filesystem::path path;
        int line = 0;
    };
    std::vector<Include> includes;

    struct Embed
    {
        std::string id;
        std::string type;
        std::string guid; // optional; absent in files authored before embed identity was persisted
        std::unordered_map<std::string, PropValue> properties;
        int headerLine = 0;
    };
    std::unordered_map<std::string, Embed> embeds;

    struct Resource
    {
        std::string id;
        std::string path;
        std::string guid; // optional
        int headerLine = 0;
    };
    std::unordered_map<std::string, Resource> resources;

    // A body line that is not "Component.field = value" ({line, text}): no '=', no dot, or an empty
    // component or field name. The load reports each one.
    using UnreadLines = std::vector<std::pair<int, std::string>>;

    struct Entity
    {
        std::string id;
        std::optional<std::string> parent;
        ComponentProperties components;
        UnreadLines unreadLines;
        int headerLine = 0;
    };
    std::vector<Entity> entities; // in file order

    // One [prefab] section: an instance of the prefab its source names, with its override lines. An
    // override line names an entity of the instance by its address, the local ids from the
    // instance's root down to it joined by '.'; the empty address is the root.
    struct PrefabInstanceSection
    {
        std::string id;
        std::optional<std::string> parent;
        std::string sourceResourceId;

        // "address/Component.field = value": the changed fields of one entity, in file order.
        struct AddressedFields
        {
            std::string address;
            ComponentProperties components;
        };
        std::vector<AddressedFields> changedFields;

        // "+address/Component" and "-address/Component".
        struct AddressedComponent
        {
            std::string address;
            std::string name; // lowercased, for schema lookup
            std::string authoredName;
            int line = 0;
        };
        std::vector<AddressedComponent> addedComponents;
        std::vector<AddressedComponent> removedComponents;

        // "-entity address": the entity at the address and everything under it.
        struct RemovedEntity
        {
            std::string address;
            int line = 0;
        };
        std::vector<RemovedEntity> removedEntities;

        UnreadLines unreadLines;
        int headerLine = 0;
    };
    std::vector<PrefabInstanceSection> prefabInstances;

    struct Subscene
    {
        std::string id;
        std::string sourceResourceId;
        Mathematics::Vector3 offset;
        bool hasOffset = false;
        bool enabled = true;
        int headerLine = 0;
    };
    std::vector<Subscene> subscenes;

    bool hasHierarchyUi = false;
    SceneHierarchyUi hierarchyUi;

    bool hasEditorCamera = false;
    SceneEditorCamera editorCamera;

};

struct Instantiated
{
    std::vector<ECS::EntityHandle> created;
    std::unordered_map<std::string, ECS::EntityHandle> idToEntity;

    // An entity whose parent is inside a prefab instance: linked once the instance is placed.
    struct PendingParent
    {
        ECS::EntityHandle entity{};
        std::string parentId;
        std::filesystem::path file;
        int line = 0;
    };
    std::vector<PendingParent> pendingParents;
};

// Where a load or a template placement that stops records why: the message, the file and the line.
struct LoadFailure
{
    std::string* message = nullptr;
    std::filesystem::path* file = nullptr;
    int* line = nullptr;

    bool Fail(const std::filesystem::path& inFile, int atLine, std::string text) const
    {
        if (message)
            *message = std::move(text);
        if (file)
            *file = inFile;
        if (line)
            *line = atLine;
        return false;
    }
};

// The document: parsing and checking a scene or prefab file's text.
bool ParseDocument(const std::filesystem::path& filePath, const std::string& text, SceneDoc& out,
                   std::string* outError, int* outErrorLine = nullptr);
bool ValidateDocument(const SceneDoc& doc, std::string* outError, int* outErrorLine = nullptr);
bool ReadTextFile(const std::filesystem::path& p, std::string& out);

// The last scene IO error, which GetLastSceneIOError returns.
void SetError(const std::filesystem::path& file, int line, const std::string& msg);

// Scene identities and addresses.
// The id of a prefab file's root, the entity an instance's root is.
inline constexpr std::string_view kPrefabRootId = "root";

// The longest scene identity a SceneEntityTag holds.
inline constexpr std::size_t kMaxSceneTagLength = sizeof(Components::SceneEntityTag::value) - 1;

// A long scene identity is 16 hex digits of its path's hash and '~' before the local id.
inline constexpr std::size_t kHashedSceneTagHeadLength = 17;

// The longest id in a prefab file: a hashed scene identity keeps it whole.
inline constexpr std::size_t kMaxPrefabIdLength = kMaxSceneTagLength - kHashedSceneTagHeadLength;

Components::SceneEntityTag MakeTag(const std::string& s);
std::string SlugifyDisplayNameForSceneEntityId(std::string_view raw);
std::string AllocateUniqueSceneEntityId(const std::string& preferred, std::unordered_set<std::string>& used);
std::string JoinPath(std::string_view head, std::string_view id);
std::string_view LastIdOf(std::string_view pathOrTag);
std::string SceneTagForPath(std::string_view path);
bool IsMemberTag(std::string_view tag);
std::string Addressed(std::string_view address, std::string_view text);

// Loading a document's entities.
SceneLoadContext MakeDocumentLoadContext(ECS::World& world, const std::filesystem::path& file, const SceneDoc& doc,
                                         const LoadOptions& options);
std::filesystem::path ResolveResourcePath(const SceneLoadContext& ctx, const std::filesystem::path& ownerFile,
                                          const SceneDoc::Resource& r);
void ReportUnreadableResource(std::string_view kind, const SceneDoc::Resource& r,
                              const std::filesystem::path& resolvedPath, const std::filesystem::path& projectRoot,
                              const std::filesystem::path& ownerFile, int instanceLine, std::string* outError,
                              std::filesystem::path* outErrorFile, int* outErrorLine);
void RecordUnreadLines(const SceneDoc::UnreadLines& unreadLines, const std::string& entityId,
                       const std::filesystem::path& ownerFile, ECS::EntityHandle h, std::size_t firstRow,
                       SceneLoadDegradation* degradation);
void PreserveOrDropUnhandledComponent(ECS::World& world, ECS::EntityHandle h, const SceneDoc::ComponentProps& comp,
                                      const std::string& entityId, const std::filesystem::path& ownerFile);
void ApplyComponentBlock(ECS::World& world, const SceneLoadContext& ctx, const ISceneComponentSchema& schema,
                         ECS::EntityHandle h, const SceneDoc::ComponentProps& authored, const std::string& entityId,
                         const std::filesystem::path& ownerFile, int headerLine, SceneLoadDegradation* degradation);
void ApplyFallbackName(ECS::World& world, const SceneLoadContext& ctx, ECS::EntityHandle h,
                       std::string_view fallbackName, const std::string& entityId);
void ApplyEntityComponents(ECS::World& world, const SceneLoadContext& ctx, const std::filesystem::path& ownerFile,
                           ECS::EntityHandle h, const SceneDoc::Entity& e, const std::string& entityId,
                           std::string_view fallbackName, SceneLoadDegradation* degradation);

// Saving: the authored text of fields the load could not read, and a prefab instance's source path.
void ApplyPreservedFieldOverrides(ECS::World& world, ECS::EntityHandle entity, std::vector<std::string>& lines);
bool ResolvePrefabSourceForSave(const SceneSaveContext& ctx, const GUID& source, std::string& outPath);

} // namespace GameEngine::Scene
