#include "Picking/ScenePick.h"

#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "Picking/EditorPickProviders.h"
#include "Scene/SceneRaycastHost.h"
#include "SceneView/SceneViewEye.h"

namespace GameEngine::Editor::Picking
{
namespace
{

// The pick providers' meshes of the current pick. Picks run one at a time on the main thread, so
// one vector serves every pick and allocates only when the providers offer more meshes than any
// pick before.
std::vector<Scene::RaycastMesh>& ProviderMeshScratch()
{
    static std::vector<Scene::RaycastMesh> s_Meshes;
    return s_Meshes;
}

Scene::RaycastMesh ToRaycastMesh(ECS::EntityHandle entity, const SyntheticMeshPick& pick)
{
    return {entity, Rendering::MeshGPUHandle(pick.MeshGpuHandleId), pick.ModelAssetGuid, pick.RenderLayers};
}

// Fills `meshes` with the pick providers' meshes the ray tests: the restricted entity's own, or
// every pickable one the providers enumerate when the pick includes meshes.
void CollectProviderMeshes(ECS::World& world, const Scene::PickOptions& options,
                           std::vector<Scene::RaycastMesh>& meshes)
{
    meshes.clear();
    const bool restricted = world.IsValid(options.RestrictToEntity);
    if (!restricted && !options.IncludeMeshes)
        return;
    for (const EditorPickProvider& provider : EditorPickProviderRegistry::Get().Snapshot())
    {
        if (restricted)
        {
            SyntheticMeshPick pick;
            if (provider.Resolve && provider.Resolve(world, options.RestrictToEntity, pick))
                meshes.push_back(ToRaycastMesh(options.RestrictToEntity, pick));
            continue;
        }
        if (!provider.Enumerate)
            continue;
        provider.Enumerate(world, [&meshes](ECS::EntityHandle entity, const SyntheticMeshPick& pick)
        {
            meshes.push_back(ToRaycastMesh(entity, pick));
        });
    }
}

Scene::HostRaycastInputs CollectHostInputs(ECS::World& world, const Scene::PickOptions& options)
{
    std::vector<Scene::RaycastMesh>& meshes = ProviderMeshScratch();
    CollectProviderMeshes(world, options, meshes);
    Scene::HostRaycastInputs host;
    host.ExtraMeshes = meshes;
    host.SkipEntity = &IsHiddenInSceneView;
    return host;
}

} // namespace

Scene::PickResult PickScene(const Mathematics::Ray3D& ray,
                            ECS::World& world,
                            const Scene::PickOptions& options)
{
    return Scene::RaycastSceneForHost(ray, world, options, CollectHostInputs(world, options));
}

void PickSceneAll(const Mathematics::Ray3D& ray,
                  ECS::World& world,
                  std::vector<Scene::PickHit>& outHits,
                  const Scene::PickOptions& options)
{
    Scene::RaycastSceneAllForHost(ray, world, outHits, options, CollectHostInputs(world, options));
}

} // namespace GameEngine::Editor::Picking
