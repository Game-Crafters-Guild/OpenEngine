#pragma once

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"

namespace GameEngine::Components
{
struct Transform;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Scene
{

/// Spawns an instance of the prefab asset `prefab` into `world`: a batch copy of its template in
/// the engine's prefab library, the entities in template order, the root at `at` under `parent`
/// (no parent when invalid) and marked with its source. `at` is relative to `parent`, or to the
/// world when `parent` is invalid. Returns the root.
///
/// The prefab must be resident: placed by a loaded scene, or named by a PrefabRef field of a
/// component in a loaded scene. Otherwise nothing is created, the handle returned is invalid and
/// the fix is logged once per prefab. A spawn never reads a file.
///
/// Call it from the main thread, outside a system or a job.
ECS::EntityHandle SpawnPrefab(ECS::World& world, const GUID& prefab, const Components::Transform& at,
                              ECS::EntityHandle parent);

/// Destroys `root` and every entity under it in the hierarchy through Components::DestroySubtree, queued
/// as World::DestroyEntity queues. It checks no instance marker: any live entity goes with its subtree.
/// Call it from the main thread, outside a job.
void DespawnPrefab(ECS::World& world, ECS::EntityHandle root);

} // namespace GameEngine::Scene
