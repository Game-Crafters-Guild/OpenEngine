#include "Scene/KeptComponentResolver.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "ECS/UnresolvedComponentStore.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Scene/AssetManagerSceneResolver.h"
#include "Scene/PrefabLibrary.h"
#include "Scene/PrefabPlacement.h"
#include "Scene/SceneEngineEmbedMaterializer.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneSchemaRegistry.h"

#include <span>
#include <string>
#include <vector>

namespace GameEngine::Scene
{
namespace
{
// Applies one kept component through its schema: defaults for a bare line, else each authored
// property in order. Leaves the error of the first property that does not apply in `error`.
bool ApplyKeptComponent(ECS::World& world, ECS::EntityHandle entity, const ISceneComponentSchema& schema,
                        const SceneReadContext& context, const ECS::PreservedComponent& kept, std::string& error)
{
    if (kept.Props.empty())
        return schema.AddDefault(world, entity, &error);
    for (const auto& [property, value] : kept.Props)
    {
        if (!schema.ApplyProperty(world, entity, context, property, value, &error))
            return false;
    }
    return true;
}

// Applies every kept component of `world` whose type is registered now. Adds each entity that got a
// component to `entities` once. Returns the number of components applied.
std::size_t ApplyRegisteredKeptComponents(ECS::World& world, std::vector<ECS::EntityHandle>& entities)
{
    // A world without a store has kept nothing; GetUnresolvedComponents would create one.
    const ECS::UnresolvedComponentStore* existing = world.TryGetUnresolvedComponents();
    if (!existing || existing->Empty())
        return 0;
    ECS::UnresolvedComponentStore& kept = world.GetUnresolvedComponents();

    SceneReadContext context{};
    context.TargetWorld = &world;
    std::size_t applied = 0;
    for (auto entityIt = kept.Map().begin(); entityIt != kept.Map().end();)
    {
        const ECS::EntityHandle entity = entityIt->first;
        if (!world.IsValid(entity))
        {
            entityIt = kept.Map().erase(entityIt);
            continue;
        }
        auto& components = entityIt->second;
        const std::size_t appliedBefore = applied;
        for (auto componentIt = components.begin(); componentIt != components.end();)
        {
            const ISceneComponentSchema* schema = SceneSchemaRegistry::Find(componentIt->Name);
            if (!schema)
            {
                ++componentIt;
                continue;
            }
            std::string error;
            if (!ApplyKeptComponent(world, entity, *schema, context, *componentIt, error))
            {
                Logger::Log::Warning("Scene: kept component '{}' did not apply now that its type is registered: {}",
                                     componentIt->Name, error);
                ++componentIt;
                continue;
            }
            Logger::Log::Info("Scene: applied kept component '{}' now that its type is registered", componentIt->Name);
            componentIt = components.erase(componentIt);
            ++applied;
        }
        if (applied > appliedBefore)
            entities.push_back(entity);
        if (components.empty())
            entityIt = kept.Map().erase(entityIt);
        else
            ++entityIt;
    }
    if (applied > 0)
        world.ProcessCommands();
    return applied;
}

// Holds in `library`, for `world`, the prefabs that the PrefabRef fields of `entities` name, through
// the pass that a scene load uses.
void HoldPrefabsNamedBy(PrefabLibrary& library, ECS::World& world, std::span<const ECS::EntityHandle> entities)
{
    EngineCore& engine = EngineCore::GetInstance();
    AssetManagerSceneResolver resolver(engine.GetAssetManager(), engine.GetResolvedAssetRoot(),
                                       AssetManagerSceneResolver::UnknownFile::AssignGuid);
    SceneEngineEmbedMaterializer materializer(engine.GetAssetManager());
    SceneLoadOptions options;
    options.Read.AssetResolver = &resolver;
    options.Read.AssetRootOverride = resolver.GetAssetRoot();
    options.Read.ProjectRoot = engine.GetWorkspaceRoot();
    options.EmbedMaterializer = &materializer;
    HoldReferencedPrefabs(library, world, options, entities);
}
} // namespace

std::size_t ResolveKeptComponents(ECS::World& world)
{
    std::vector<ECS::EntityHandle> entities;
    const std::size_t applied = ApplyRegisteredKeptComponents(world, entities);
    PrefabLibrary* library = EngineCore::GetInstance().TryGetPrefabLibrary();
    if (!library)
        return applied;
    if (!entities.empty())
        HoldPrefabsNamedBy(*library, world, entities);
    // A template built before the type registered keeps the component in the library's world.
    library->RebuildForRegisteredTypes();
    return applied;
}
} // namespace GameEngine::Scene
