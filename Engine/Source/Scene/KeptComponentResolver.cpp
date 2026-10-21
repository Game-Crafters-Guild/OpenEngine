#include "Scene/KeptComponentResolver.h"

#include "ECS/UnresolvedComponentStore.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneSchemaRegistry.h"

#include <string>

namespace GameEngine::Scene
{
namespace
{
// Applies one kept component through its schema: defaults for a bare line, else each authored
// property in order. Leaves the error of the first property that does not apply in `error`.
bool ApplyKeptComponent(ECS::World& world, ECS::EntityHandle entity, const ISceneComponentSchema& schema,
                        const SceneLoadContext& context, const ECS::PreservedComponent& kept, std::string& error)
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
} // namespace

std::size_t ResolveKeptComponents(ECS::World& world)
{
    // A world without a store has kept nothing; GetUnresolvedComponents would create one.
    const ECS::UnresolvedComponentStore* existing = world.TryGetUnresolvedComponents();
    if (!existing || existing->Empty())
        return 0;
    ECS::UnresolvedComponentStore& kept = world.GetUnresolvedComponents();

    SceneLoadContext context{};
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
        if (components.empty())
            entityIt = kept.Map().erase(entityIt);
        else
            ++entityIt;
    }
    if (applied > 0)
        world.ProcessCommands();
    return applied;
}
} // namespace GameEngine::Scene
