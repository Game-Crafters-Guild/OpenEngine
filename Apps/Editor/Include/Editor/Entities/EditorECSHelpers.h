#pragma once

#include "ECS/ECSTemplates.h"
#include "EditorChangeNotifications.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/WorldPosition.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/PhysicsWorldService.h"

namespace GameEngine::Editor
{

/// Write an updated component to the world and fire a commit notification.
/// Template parameter T is deduced from `updated` — callers never spell it out.
template<typename T>
void CommitComponentUpdate(ECS::World* world,
                                   ECS::EntityHandle entity,
                                   EditorChangeNotifications* notifications,
                                   const T& updated)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return;
    world->AddComponentImmediate(entity, updated);
    if (notifications)
        notifications->NotifyComponentCommit<T>(world, entity);
}

/// Same world write as CommitComponentUpdate, but notifies subscribers with
/// ChangeKind::Preview (live drag / in-progress edit) instead of Commit.
template<typename T>
void PreviewComponentUpdate(ECS::World* world,
                            ECS::EntityHandle entity,
                            EditorChangeNotifications* notifications,
                            const T& updated)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return;
    world->AddComponentImmediate(entity, updated);
    if (notifications)
        notifications->NotifyComponentChange<T>(world, entity, EditorChangeNotifications::ChangeKind::Preview);
}

/// Moves the live physics body of `entity` to an edited pose and stops it: the velocities become zero and the
/// previous pose is the new one, so the body does not blend from the old pose. Returns false when the entity has
/// no live body.
inline bool TeleportPhysicsBody(ECS::World* world, ECS::EntityHandle entity, const Mathematics::Vector3& position,
                                const Mathematics::Quaternion& rotation)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
    {
        return false;
    }

    Physics::PhysicsWorld* physics = PhysicsECS::PhysicsWorldService::TryGet();
    if (!physics)
    {
        return false;
    }

    auto* body = world->GetComponentForWrite<Components::PhysicsBody>(entity);
    if (!body || !body->initialized || !physics->IsBodyValid(body->body))
    {
        return false;
    }

    Physics::Transform pose{};
    pose.position = Mathematics::WorldPosition::FromVector3(position);
    const auto& glmRotation = rotation.GetGLM();
    pose.rotation = Physics::Quaternion(glmRotation.w, glmRotation.x, glmRotation.y, glmRotation.z);
    physics->SetBodyTransform(body->body, pose, Physics::ActivationMode::Activate);
    physics->SetLinearVelocity(body->body, Physics::Vector3(0.0f, 0.0f, 0.0f));
    physics->SetAngularVelocity(body->body, Physics::Vector3(0.0f, 0.0f, 0.0f));

    body->prevPhysicsTransform = pose;
    body->hasPrevPhysicsTransform = true;
    return true;
}

} // namespace GameEngine::Editor
