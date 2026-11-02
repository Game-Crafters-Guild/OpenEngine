#pragma once

// Writes a prefab instance as the scene saver saves it: its [prefab] section and the lines that
// differ from its prefab, found against the prefab placed once per save. Private to the scene IO
// sources.

#include "Scene/PrefabAuthoring.h"
#include "Scene/PrefabPlacement.h"
#include "Types/FlatMap.h"
#include "Types/FlatSet.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine::Scene
{

class ISceneComponentSchema;

// The prefabs a save compares its instances with, each placed once into a world of its own the save
// owns, with its entities by address.
struct PrefabTemplates
{
    struct Template
    {
        PlacedPrefab placed;
        FlatMap<std::string, ECS::EntityHandle> byAddress;
    };

    explicit PrefabTemplates(const SceneWriteOptions& saveOptions)
    {
        loadOptions.clearWorldFirst = false;
        loadOptions.assetResolver = saveOptions.assetResolver;
        loadOptions.assetRootOverride = saveOptions.assetRootOverride;
    }

    SceneReadOptions loadOptions;
    std::unique_ptr<ECS::World> world;
    std::unique_ptr<PrefabPlacement> placement;
    FlatMap<std::string, std::unique_ptr<Template>> byPath;
};

// A prefab instance the scene places, as the saver writes it: its root, its members by address in
// save order, and the template they are compared with.
struct SavedPrefabInstance
{
    ECS::EntityHandle h{};
    std::string id;         // the root's scene identity: the instance id
    std::string sourcePath; // the prefab's path, from the asset database
    std::string sourceGuid;
    std::string parentId;   // the parent's scene identity, if any
    std::optional<std::size_t> parentInstance; // the saved instance the parent belongs to, if any
    const PrefabTemplates::Template* templ = nullptr;
    std::vector<std::pair<std::string, ECS::EntityHandle>> members; // the root first, at ""
    FlatSet<std::string> addresses;
};

// Gives `h` a scene-level id from its name when its tag cannot be one.
void AssignSceneLevelIdIfNeeded(ECS::World& world, ECS::EntityHandle h,
                                std::unordered_map<ECS::EntityId, std::string>& tagByEntity,
                                std::unordered_set<std::string>& usedTags);

// Sorts the scene's entities into prefab instances and the rest.
bool CollectPrefabInstances(ECS::World& world, const std::vector<ECS::EntityHandle>& entities,
                            const SceneWriteOptions& options, const SceneWriteContext& saveCtx,
                            const std::filesystem::path& sceneFilePath,
                            std::unordered_map<ECS::EntityId, std::string>& tagByEntity,
                            std::unordered_set<ECS::EntityId>& excluded, PrefabTemplates& templates,
                            std::vector<SavedPrefabInstance>& instances);

// One component of an entity as the scene saver writes it: its file token and its lines.
struct ComponentText
{
    std::string token;
    std::string key; // the token lowercased
    std::vector<std::string> lines;
};

// The lines the scene saver writes for `entity`, grouped by component.
void CollectComponentText(ECS::World& world, ECS::EntityHandle entity, const SceneWriteContext& context,
                          const std::vector<const ISceneComponentSchema*>& schemas, std::vector<ComponentText>& out);

// True for a component of an instance's root that places it in the scene: never an override.
bool IsPlacementComponent(std::string_view key);

// DescribePrefabInstance against the generation `prefab` of the instance's prefab. `outAddresses[i]` is the
// address of `out[i]` under the root: empty for the root and for an entity that the instance added.
void DescribePrefabInstanceAgainst(ECS::World& world, ECS::EntityHandle root, PrefabLibrary& library,
                                   const PrefabTemplate& prefab, std::vector<PrefabInstanceEntity>& out,
                                   std::vector<std::string>& outAddresses);

// Writes the body of one instance's [prefab] section.
void WritePrefabInstanceBody(ECS::World& world, const SavedPrefabInstance& instance,
                             const PrefabTemplates& templates, const SceneWriteContext& saveCtx,
                             const std::vector<const ISceneComponentSchema*>& schemas, std::string& out);

} // namespace GameEngine::Scene
