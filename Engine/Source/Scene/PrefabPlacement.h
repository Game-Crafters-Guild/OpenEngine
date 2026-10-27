#pragma once

// Places prefab instances into a world: each prefab resolved once into a template of the prefab
// library and each instance a batch clone of it, or, for the override writer's templates, each
// instance placed from its file's text; then the instance's override lines applied. Private to the
// scene IO sources.

#include "ECS/EntityRemap.h"
#include "Scene/PrefabLibrary.h"
#include "Scene/SceneDoc.h"
#include "Types/FlatMap.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine::Components
{
struct Transform;
}

namespace GameEngine::Scene
{

// What a template build records beyond the entities it places: every entity it created, in
// creation order, with the file (an index into `files`) and header line that author it, and every
// prefab file it read.
struct TemplateBuildLog
{
    std::vector<ECS::EntityHandle> entities;
    std::vector<PrefabTemplate::Origin> origins;
    std::vector<std::filesystem::path> files;
};

// A prefab file read once for one load or save, with the tables its lines resolve against.
struct ParsedPrefab
{
    std::filesystem::path path;
    SceneDoc doc;
    SceneLoadContext ctx;
};

// What one load (or one save's template placement) shares across the prefab instances it places.
struct PrefabPlacement
{
    ECS::World& world;
    const LoadOptions& options;
    SceneLoadDegradation* degradation = nullptr;

    // Each prefab file parsed once, by its absolute path. A scene places thousands of instances
    // of a few hundred prefabs.
    FlatMap<std::string, std::unique_ptr<ParsedPrefab>> parsedByPath;

    // The prefab files being placed, outermost first: a file already on the chain closes a cycle.
    std::vector<std::filesystem::path> chain;

    // Every hashed scene identity the load assigned, with its path; two paths with one identity
    // are refused. A path that fits the tag is its own identity and cannot collide.
    FlatMap<std::string, std::string> pathByHashedTag;

    // True: each instance is a clone of a template in `library` (one this placement creates and
    // owns when none is set before the first instance). False: each instance is placed from its
    // file's text, as a template build does.
    bool usesLibrary = false;
    PrefabLibrary* library = nullptr;
    std::unique_ptr<PrefabLibrary> ownedLibrary;

    // Builds templates into the library's world; created with the first template.
    std::unique_ptr<PrefabPlacement> templateBuild;

    // False while a template is built: the library holds no scene identities (SceneEntityTag).
    bool writesSceneTags = true;

    // Set while a template is built.
    TemplateBuildLog* buildLog = nullptr;

    // Reused by every clone.
    ECS::EntityRemap remap;

    PrefabPlacement(ECS::World& targetWorld, const LoadOptions& loadOptions, SceneLoadDegradation* loadDegradation);
    ~PrefabPlacement();
};

// The entities of one placed prefab instance with their addresses under its root, in template
// order. The root is at the empty address.
struct PlacedPrefab
{
    ECS::EntityHandle root{};
    std::vector<std::pair<std::string, ECS::EntityHandle>> members;
};

// Places the prefab `section` names at `instancePath`; the section's override lines are the caller's.
bool PlaceInstanceSection(PrefabPlacement& placement, const SceneLoadContext& ownerCtx,
                          const std::filesystem::path& ownerFile, const SceneDoc& ownerDoc,
                          const SceneDoc::PrefabInstanceSection& section, const std::string& instancePath,
                          PlacedPrefab& out, const LoadFailure& failure);

// `file` as it is spelled on disk, relative to `assetRoot` when the file is inside it. The registry
// gives paths in lower case; the file on disk has the name that the user wrote.
std::string PathAsSpelledOnDisk(const std::filesystem::path& file, const std::filesystem::path& assetRoot);

// Builds the template of the prefab file at `path` (absolute, lexically normal) into `library`, a
// new generation when it is resident; `source` is its asset GUID, or null. Null, with `error`
// naming the file, the line and the fix, when the prefab cannot be placed.
const PrefabTemplate* BuildTemplateFromFile(PrefabLibrary& library, const std::filesystem::path& path,
                                            const GUID& source, const LoadOptions& options, std::string& error);

// Holds in `library`, for `world`, every prefab a PrefabRef field of `entities` (in `world`) names,
// building the ones that are not resident through `options.assetResolver` (and, through their own
// PrefabRef fields, the prefabs they name). A prefab that cannot be read is logged and skipped.
void HoldReferencedPrefabs(PrefabLibrary& library, ECS::World& world, const LoadOptions& options,
                           std::span<const ECS::EntityHandle> entities);

// Clones `prefab` into `world` with its root at `at` under `parent`, marked with `source`; the
// clones are remap.GetTargets(), in template order. Null when the clone fails.
ECS::EntityHandle SpawnTemplate(ECS::World& world, const PrefabLibrary& library, const PrefabTemplate& prefab,
                                const GUID& source, const Components::Transform& at, ECS::EntityHandle parent,
                                ECS::EntityRemap& remap);

// The scene identities `world` uses, so a new instance gets an id no entity has.
std::unordered_set<std::string> CollectSceneIdentities(ECS::World& world);

// Places every [prefab] section of `doc` and applies its override lines.
bool PlaceSceneInstances(PrefabPlacement& placement, SceneLoadContext& sceneCtx,
                         const std::filesystem::path& sceneFilePath, const SceneDoc& doc, const std::string& idPrefix,
                         Instantiated& out, const LoadFailure& failure);

} // namespace GameEngine::Scene
