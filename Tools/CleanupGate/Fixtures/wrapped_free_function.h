#pragma once

namespace GameEngine::Scene
{

/// One free function whose signature wraps over three lines. The last line alone also matches a function head.
ECS::EntityHandle SpawnPrefab(ECS::World& world, const GUID& prefab,
                              const Components::Transform& at,
                              const float32 (&bounds)[6]);

} // namespace GameEngine::Scene
