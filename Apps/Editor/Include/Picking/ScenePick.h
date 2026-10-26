#pragma once

#include "Scene/SceneRaycast.h"

#include <vector>

namespace GameEngine::ECS { class World; }

namespace GameEngine::Editor::Picking
{

/// The Scene View's pick: the engine's scene raycast with the editor's two host inputs
/// (Scene/SceneRaycastHost.h). A mesh the Scene View eye hides is not hit, and the meshes of the
/// registered pick providers are tested beside the renderers. A caller that measures the ground
/// the same way in every process calls Scene::RaycastScene instead.
Scene::PickResult PickScene(const Mathematics::Ray3D& ray,
                            ECS::World& world,
                            const Scene::PickOptions& options = {});

/// Every hit along the ray, front to back, under the same two inputs.
void PickSceneAll(const Mathematics::Ray3D& ray,
                  ECS::World& world,
                  std::vector<Scene::PickHit>& outHits,
                  const Scene::PickOptions& options = {});

} // namespace GameEngine::Editor::Picking
