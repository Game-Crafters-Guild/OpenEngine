// Scene View pick-target resolution: maps a raycast hit to the entity a
// click should select, honoring model-instance boundaries (the engine's
// analog of Unity's "outermost prefab root" pick).
#pragma once

#include "ECS/Entity.h"

#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Scene
{
struct PickHit;
}

namespace GameEngine::Editor::Picking
{

// The entity a root-mode pick selects: the outermost climb root (IsPickClimbRoot) at or above
// `entity`; else the topmost ancestor of the same model instance, never across a node that renders
// another asset; else `entity` itself.
ECS::EntityHandle ResolvePickRoot(ECS::World& world, ECS::EntityHandle entity);

// Ordered click-through candidates from a distance-sorted, pickability-filtered hit list.
// Root mode: the nearest hit's root, then each hit; a runtime-only hit becomes its outermost pick climb root if it has one.
// Exact mode: the hits nearest to farthest. Deduplicated, order preserved.
void BuildPickCandidates(ECS::World& world,
                         const std::vector<Scene::PickHit>& sortedHits,
                         bool exactPick,
                         std::vector<ECS::EntityHandle>& outCandidates);
} // namespace GameEngine::Editor::Picking
