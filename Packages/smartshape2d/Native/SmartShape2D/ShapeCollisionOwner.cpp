#include "SmartShape2D/ShapeCollisionOwner.h"

#include "SmartShape2D/GeneratedShapeResources.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "PhysicsECS/Components/MeshColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/MeshColliderData.h"

namespace GameEngine::SmartShape2D
{
bool SetShapeCollision(ECS::World& world, ECS::EntityHandle owner, GenerationalVector::Handle data)
{
    if (!world.IsValid(owner) || !data.IsValid())
        return false;
    Components::GeneratedShapeResources resources{};
    if (const auto* existing = world.GetComponent<Components::GeneratedShapeResources>(owner))
        resources = *existing;
    const auto* mesh = world.GetComponent<Components::MeshColliderShape>(owner);
    if (mesh && (mesh->DataIndex != resources.CollisionData.Index() ||
                 mesh->DataGeneration != resources.CollisionData.Generation() || !resources.CollisionData.IsValid()))
        return false;
    const auto previous = resources.CollisionData;
    resources.Owner = owner;
    resources.CollisionData = data;
    if (!world.HasComponent<Components::PhysicsCollider>(owner))
    {
        Components::PhysicsCollider collider{};
        collider.layer = Physics::Layers::Static;
        world.AddComponentImmediate(owner, collider);
        resources.AddedPhysicsCollider = true;
    }
    if (!world.HasComponent<Components::PhysicsBody>(owner))
    {
        Components::PhysicsBody body{};
        body.motionType = Physics::MotionType::Static;
        world.AddComponentImmediate(owner, body);
        resources.AddedPhysicsBody = true;
    }
    Components::MeshColliderShape shape{};
    shape.DataIndex = data.Index();
    shape.DataGeneration = data.Generation();
    world.AddComponentImmediate(owner, shape);
    world.AddComponentImmediate(owner, resources);
    if (previous != data)
        PhysicsECS::MeshColliderData::Release(previous);
    return true;
}

void RemoveShapeCollision(ECS::World& world, ECS::EntityHandle owner)
{
    if (!world.IsValid(owner))
        return;
    const auto* stored = world.GetComponent<Components::GeneratedShapeResources>(owner);
    if (!stored || stored->Part != 0xFFFFFFFFu || stored->Owner != owner)
        return;
    const auto resources = *stored;
    const auto* mesh = world.GetComponent<Components::MeshColliderShape>(owner);
    const bool ownsMesh = mesh && mesh->DataIndex == resources.CollisionData.Index() &&
                          mesh->DataGeneration == resources.CollisionData.Generation();
    if (ownsMesh)
        world.RemoveComponentImmediate<Components::MeshColliderShape>(owner);
    // Another provider may have replaced the mesh. Keep its required physics components.
    if (ownsMesh || !mesh)
    {
        if (resources.AddedPhysicsBody)
            world.RemoveComponentImmediate<Components::PhysicsBody>(owner);
        if (resources.AddedPhysicsCollider)
            world.RemoveComponentImmediate<Components::PhysicsCollider>(owner);
    }
    world.RemoveComponentImmediate<Components::GeneratedShapeResources>(owner);
    PhysicsECS::MeshColliderData::Release(resources.CollisionData);
}
}
