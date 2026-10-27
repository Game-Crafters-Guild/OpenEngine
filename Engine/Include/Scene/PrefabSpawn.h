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

/// Spawns an instance of `prefab` into `world`: the root at `at`, relative to `parent` (the world when invalid).
/// The prefab must be resident: a loaded scene places it or names it in a PrefabRef field. Otherwise returns an
/// invalid handle and logs the fix once. Reads no file. Main thread only, outside a system or a job.
ECS::EntityHandle SpawnPrefab(ECS::World& world, const GUID& prefab, const Components::Transform& at,
                              ECS::EntityHandle parent);

/// Destroys `root` and every entity under it in the hierarchy through Components::DestroySubtree, queued
/// as World::DestroyEntity queues. It checks no instance marker: any live entity goes with its subtree.
/// Call it from the main thread, outside a job.
void DespawnPrefab(ECS::World& world, ECS::EntityHandle root);

} // namespace GameEngine::Scene
