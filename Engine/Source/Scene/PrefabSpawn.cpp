#include "Scene/PrefabSpawn.h"

#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/PrefabInstance.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/EntityRemap.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Scene/PrefabLibrary.h"
#include "Scene/PrefabPlacement.h"
#include "Scripting/AbiMainThread.h"

#include <cassert>

namespace GameEngine::Scene
{

ECS::EntityHandle SpawnTemplate(ECS::World& world, const PrefabLibrary& library, const PrefabTemplate& prefab,
                                const GUID& source, const Components::Transform& at, ECS::EntityHandle parent,
                                ECS::EntityRemap& remap)
{
    if (!world.CreateBatchFromWorld(library.GetWorld(), prefab.Plan, remap))
        return {};
    const ECS::EntityHandle root = remap.GetTargets().front();
    world.AddComponentImmediate(root, at);
    if (parent.IsValid())
        world.AddComponentImmediate(root, Components::Parent{parent});
    Components::PrefabInstance marker{};
    marker.Source = source;
    world.AddComponentImmediate(root, marker);
    return root;
}

ECS::EntityHandle SpawnPrefab(ECS::World& world, const GUID& prefab, const Components::Transform& at,
                              ECS::EntityHandle parent)
{
    PrefabLibrary* library = EngineCore::GetInstance().TryGetPrefabLibrary();
    if (!library)
    {
        Logger::Log::Error("Scene::SpawnPrefab needs an initialized engine: call EngineCore::Initialize first.");
        return {};
    }
    assert(ScriptingAbi::IsMainThreadOutsideJobFor(EngineCore::GetInstance()) &&
           "Scene::SpawnPrefab runs on the main thread only, outside any job");
    const PrefabTemplate* prefabTemplate = library->FindByGuid(prefab);
    if (!prefabTemplate)
    {
        library->ReportNotResident(prefab);
        return {};
    }
    thread_local ECS::EntityRemap remap;
    return SpawnTemplate(world, *library, *prefabTemplate, prefab, at, parent, remap);
}

void DespawnPrefab(ECS::World& world, ECS::EntityHandle root)
{
    assert(ScriptingAbi::IsMainThreadOutsideJobFor(EngineCore::GetInstance()) &&
           "Scene::DespawnPrefab runs on the main thread only, outside any job");
    Components::DestroySubtree(world, root);
}

} // namespace GameEngine::Scene
