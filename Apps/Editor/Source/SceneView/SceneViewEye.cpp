#include "SceneView/SceneViewEye.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/SceneViewHidden.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Entities/EntityDisplayName.h"

#include <string>
#include <vector>

namespace GameEngine::Editor
{
void SetHiddenInSceneView(ECS::World& world, std::span<const ECS::EntityHandle> entities, bool hidden)
{
    for (ECS::EntityHandle entity : entities)
    {
        if (!entity.IsValid() || !world.IsValid(entity))
            continue;
        const bool isHidden = world.HasComponent<Components::SceneViewHidden>(entity);
        if (hidden && !isHidden)
            world.AddComponentImmediate<Components::SceneViewHidden>(entity, Components::SceneViewHidden{});
        else if (!hidden && isHidden)
            world.RemoveComponentImmediate<Components::SceneViewHidden>(entity);
    }
}

bool IsHiddenInSceneView(ECS::World& world, ECS::EntityHandle entity)
{
    return world.IsValid(entity) && (world.HasComponent<Components::SceneViewHidden>(entity) ||
                                     world.HasComponent<Components::SceneViewHiddenInHierarchy>(entity));
}

SceneViewEyeState DescribeSceneViewEye(ECS::World& world, ECS::EntityHandle entity)
{
    SceneViewEyeState state;
    if (!world.IsValid(entity))
        return state;
    state.HasMesh = world.HasComponent<Components::MeshRenderer>(entity);
    state.HiddenSelf = world.HasComponent<Components::SceneViewHidden>(entity);
    if (state.HiddenSelf || !world.HasComponent<Components::SceneViewHiddenInHierarchy>(entity))
        return state;
    for (const auto* link = world.GetComponent<Components::Parent>(entity); link && world.IsValid(link->parent);
         link = world.GetComponent<Components::Parent>(link->parent))
    {
        if (!world.HasComponent<Components::SceneViewHidden>(link->parent))
            continue;
        state.HiddenByParent = true;
        state.HidingAncestor = link->parent;
        break;
    }
    return state;
}

std::string SceneViewEyeTooltip(ECS::World& world, const SceneViewEyeState& state)
{
    if (state.HiddenSelf)
        return state.HasMesh ? "Hidden in the Scene View. Click to show"
                             : "The meshes under it are hidden in the Scene View. Click to show";
    if (!state.HiddenByParent)
        return state.HasMesh ? "Hide this mesh and the meshes under it in the Scene View"
                             : "Hide the meshes under it in the Scene View";
    const std::string ancestor =
        world.IsValid(state.HidingAncestor) ? EntityDisplayName(world, state.HidingAncestor) : "its parent";
    return "Hidden with " + ancestor + ". Click to keep it hidden when " + ancestor + " is shown";
}

void RegisterSceneViewEyeTraits()
{
    EditorComponentTraits traits;
    traits.HideInInspector = true;
    EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::SceneViewHidden>(), traits);
    EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::SceneViewHiddenInHierarchy>(),
                                                  std::move(traits));
}

} // namespace GameEngine::Editor
