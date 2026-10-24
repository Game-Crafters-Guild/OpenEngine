#pragma once

// The Physics script API: ray casts and box overlaps against the physics world, the velocity and the impulses of an
// entity's PhysicsBody, the input of its CharacterController, and its contact and trigger events.
// Every marked declaration is generated into the C# binding (Physics, and entity.Physics); a native
// script calls the same functions. Distances are in meters, velocities in meters per second, impulses
// in newton seconds; +Y is up.

#include "ECS/ECS.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Types/ScriptCallback.h"

#include <cstdint>
#include <span>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Physics
{
class PhysicsWorld;
}

namespace GameEngine::PhysicsECS::Script
{

/// The layer mask that sees every collision layer.
inline constexpr std::uint32_t kAllLayers = 0xFFFFFFFFu;

/// Where a ray hit a body: the body's entity, the hit point, the surface normal there, and the
/// distance from the ray's origin.
// @ge-script
struct RayHit
{
    ECS::EntityHandle entity{};
    Mathematics::Vector3 point{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 normal{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
};

/// Two solid bodies started or stopped touching: the entity you subscribed to (self), the entity it
/// touches (other), and the contact point and normal in world space. The normal points from other into
/// self: a body resting on the ground gets +Y.
// @ge-script
struct ContactEvent
{
    ECS::EntityHandle self{};
    ECS::EntityHandle other{};
    Mathematics::Vector3 point{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 normal{0.0f, 1.0f, 0.0f};
};

/// A body entered or left a trigger: the entity you subscribed to (self), the other entity of the pair,
/// and whether self is the trigger.
// @ge-script
struct TriggerEvent
{
    ECS::EntityHandle self{};
    ECS::EntityHandle other{};
    bool selfIsTrigger = false;
};

/// Casts a ray and finds the nearest body it hits within maxDistance. False when it hits nothing, and
/// outside play mode. The direction need not be of length 1. Only bodies on a layer of layerMask are
/// hit; triggers are hit unless ignoreTriggers is true.
// @ge-script @ge-script-try @ge-context
bool RayCast(Physics::PhysicsWorld& physics, Mathematics::Vector3 origin, Mathematics::Vector3 direction, RayHit& hit,
             float maxDistance = 1000.0f, std::uint32_t layerMask = kAllLayers, bool ignoreTriggers = false);

/// Casts a ray and writes every body it hits within maxDistance into hits, nearest first, each body once at
/// its nearest hit. Returns the number of bodies the ray hits. When that is more than hits holds, the nearest
/// ones that fit are written: call again with a larger buffer to get all of them. A buffer of length 0 asks
/// the number only. 0 outside play mode. The direction need not be of length 1. Only bodies on a layer of
/// layerMask are hit; triggers are hit unless ignoreTriggers is true.
// @ge-script @ge-context
std::int32_t RayCastAll(Physics::PhysicsWorld& physics, Mathematics::Vector3 origin, Mathematics::Vector3 direction,
                        std::span<RayHit> hits, float maxDistance = 1000.0f, std::uint32_t layerMask = kAllLayers,
                        bool ignoreTriggers = false);

/// Writes the entity of every body that overlaps a box into bodies, in no fixed order. The box is centered
/// at center, extends halfExtents from it along each of its axes (in meters, each above 0), and is turned by
/// rotation. Returns the number of bodies in the box. When that is more than bodies holds, the ones that fit
/// are written: call again with a larger buffer to get all of them. A buffer of length 0 asks the number
/// only. 0 outside play mode. Only bodies on a layer of layerMask are found; triggers are found unless
/// ignoreTriggers is true.
// @ge-script @ge-context
std::int32_t BoxOverlap(Physics::PhysicsWorld& physics, Mathematics::Vector3 center, Mathematics::Vector3 halfExtents,
                        Mathematics::Quaternion rotation, std::span<ECS::EntityHandle> bodies,
                        std::uint32_t layerMask = kAllLayers, bool ignoreTriggers = false);

/// The velocity of the entity's body. Zero for an entity with no PhysicsBody, and outside play mode.
// @ge-script @ge-context
Mathematics::Vector3 GetLinearVelocity(ECS::World& world, ECS::EntityHandle entity);

/// Sets the velocity of the entity's body. Does nothing for an entity with no PhysicsBody, for a static
/// body, and outside play mode. A kinematic body follows its Transform: the call is ignored and logs a
/// warning once per entity. Move its Transform instead.
// @ge-script @ge-context
void SetLinearVelocity(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 velocity);

/// The angular velocity of the entity's body, in radians per second about each axis.
// @ge-script @ge-context
Mathematics::Vector3 GetAngularVelocity(ECS::World& world, ECS::EntityHandle entity);

/// Sets the angular velocity of the entity's body, in radians per second about each axis. A kinematic body
/// follows its Transform: the call is ignored and logs a warning once per entity. Rotate its Transform
/// instead.
// @ge-script @ge-context
void SetAngularVelocity(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 velocity);

/// Applies an impulse at the body's center of mass: its velocity changes at once by impulse / mass.
// @ge-script @ge-context
void AddImpulse(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 impulse);

/// Applies an impulse at a point in world space, which also spins the body.
// @ge-script @ge-context
void AddImpulseAtPosition(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 impulse,
                          Mathematics::Vector3 position);

/// The velocity the entity's CharacterController moves at: X and Z replace the motion every physics
/// step, and a Y above 0 is a jump at that speed. Zero for an entity with no CharacterController.
// @ge-script @ge-context
Mathematics::Vector3 GetDesiredVelocity(ECS::World& world, ECS::EntityHandle entity);

/// Sets the velocity the entity's CharacterController moves at.
// @ge-script @ge-context
void SetDesiredVelocity(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 velocity);

/// True while the entity's CharacterController stands on ground.
// @ge-script @ge-context
bool IsGrounded(ECS::World& world, ECS::EntityHandle entity);

/// Calls the handler when a solid body of the entity starts to touch another.
// @ge-script-event PhysicsStep
SubscriptionId SubscribeContactBegin(ECS::EntityHandle scope, ScriptCallback<ContactEvent> callback);

/// Stops the calls of SubscribeContactBegin.
// @ge-script-event
void UnsubscribeContactBegin(SubscriptionId subscription);

/// Calls the handler when a solid body of the entity stops touching another.
// @ge-script-event PhysicsStep
SubscriptionId SubscribeContactEnd(ECS::EntityHandle scope, ScriptCallback<ContactEvent> callback);

/// Stops the calls of SubscribeContactEnd.
// @ge-script-event
void UnsubscribeContactEnd(SubscriptionId subscription);

/// Calls the handler when a body enters a trigger, for the trigger's entity and for the body's.
// @ge-script-event PhysicsStep
SubscriptionId SubscribeTriggerEnter(ECS::EntityHandle scope, ScriptCallback<TriggerEvent> callback);

/// Stops the calls of SubscribeTriggerEnter.
// @ge-script-event
void UnsubscribeTriggerEnter(SubscriptionId subscription);

/// Calls the handler when a body leaves a trigger, for the trigger's entity and for the body's.
// @ge-script-event PhysicsStep
SubscriptionId SubscribeTriggerExit(ECS::EntityHandle scope, ScriptCallback<TriggerEvent> callback);

/// Stops the calls of SubscribeTriggerExit.
// @ge-script-event
void UnsubscribeTriggerExit(SubscriptionId subscription);

} // namespace GameEngine::PhysicsECS::Script
