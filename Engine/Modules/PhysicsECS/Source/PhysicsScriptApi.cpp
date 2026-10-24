#include "PhysicsECS/PhysicsScriptApi.h"

#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/KinematicBody.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/PhysicsEntityEventCallbacks.h"
#include "PhysicsECS/PhysicsWorldService.h"

#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace GameEngine::PhysicsECS::Script
{
namespace
{

// The live body of the entity's PhysicsBody and the world it lives in, or null for either when there
// is none (no body, a body the physics systems have not created yet, or no world outside play).
struct BodyRef
{
    Physics::PhysicsWorld* World = nullptr;
    Physics::BodyHandle Body{};
};

BodyRef FindBody(const ECS::World& world, ECS::EntityHandle entity)
{
    Physics::PhysicsWorld* physics = PhysicsWorldService::TryGet();
    const Components::PhysicsBody* body = physics != nullptr ? world.GetComponent<Components::PhysicsBody>(entity) : nullptr;
    if (body == nullptr || !body->initialized || !physics->IsBodyValid(body->body))
        return {};
    return BodyRef{physics, body->body};
}

// True when the entity's body is kinematic, read from the KinematicBody tag, the same signal the physics step
// moves by. A kinematic body follows its Transform, so a script velocity on it is ignored. The first ignored
// call for an entity in a physics world logs a warning with the fix; later calls are silent.
bool IgnoresScriptVelocity(const ECS::World& world, ECS::EntityHandle entity)
{
    if (!world.HasComponent<Components::KinematicBody>(entity))
        return false;
    static std::mutex s_WarnedMutex;
    static std::unordered_set<std::uint32_t> s_Warned;
    static std::uint64_t s_WarnedGeneration = 0;
    std::lock_guard<std::mutex> lock(s_WarnedMutex);
    const std::uint64_t generation = PhysicsWorldService::GetWorldGeneration();
    if (generation != s_WarnedGeneration)
    {
        s_Warned.clear();
        s_WarnedGeneration = generation;
    }
    if (s_Warned.insert(entity.id).second)
        Logger::Log::Warning("[PhysicsECS] Entity {} has a kinematic body, which follows its Transform, so the "
                             "script velocity is ignored. Move the Transform instead.",
                             entity.id);
    return true;
}

// The query of a ray from origin along direction, or false for a zero direction or a maxDistance that is not
// above 0.
bool MakeRayQuery(Mathematics::Vector3 origin, Mathematics::Vector3 direction, float maxDistance, std::uint32_t layerMask,
                  bool ignoreTriggers, Physics::RayCastQuery& outQuery)
{
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (!(length > 0.0f) || !(maxDistance > 0.0f))
        return false;
    outQuery.ray.origin = origin;
    outQuery.ray.direction = direction / length;
    outQuery.maxDistance = maxDistance;
    outQuery.filter.layerMask = layerMask;
    outQuery.filter.ignoreSensors = ignoreTriggers;
    return true;
}

RayHit ToScript(const Physics::RayCastResult& result)
{
    RayHit hit;
    // PhysicsECS stores the body's entity id as its user data.
    hit.entity = ECS::EntityHandle(static_cast<std::uint32_t>(result.userData));
    hit.point = result.hitPoint;
    hit.normal = result.hitNormal;
    hit.distance = result.distance;
    return hit;
}

// The number of results a function that writes a span reports.
std::int32_t ResultCount(std::size_t count)
{
    return static_cast<std::int32_t>(std::min<std::size_t>(count, std::numeric_limits<std::int32_t>::max()));
}

// The lists the queries fill, kept per thread so a query allocates only when it finds more results than any
// query before it on that thread.
std::vector<Physics::RayCastResult>& RayCastResults()
{
    thread_local std::vector<Physics::RayCastResult> s_Results;
    return s_Results;
}

std::vector<Physics::OverlapResult>& OverlapResults()
{
    thread_local std::vector<Physics::OverlapResult> s_Results;
    return s_Results;
}

ContactEvent ToScript(const EntityContactEvent& e)
{
    ContactEvent result;
    result.self = e.self;
    result.other = e.other;
    result.point = e.point;
    result.normal = e.normal;
    return result;
}

TriggerEvent ToScript(const EntityTriggerEvent& e)
{
    TriggerEvent result;
    result.self = e.self;
    result.other = e.other;
    result.selfIsTrigger = e.selfIsTrigger;
    return result;
}

// The native subscription's callbacks for a script callback: each event converted to the script
// payload, and the callback's Ended when the subscription ends.
template <class Native, class Payload>
GameEngine::Function<void(const Native&)> Relay(ScriptCallback<Payload> callback)
{
    return [callback](const Native& e)
    {
        const Payload payload = ToScript(e);
        callback.Invoke(&payload, callback.User);
    };
}

template <class Payload>
PhysicsEntityEventCallbacks::EndedCallback EndedRelay(ScriptCallback<Payload> callback)
{
    if (callback.Ended == nullptr)
        return {};
    return [callback]
    { callback.Ended(callback.User); };
}

} // namespace

bool RayCast(Physics::PhysicsWorld& physics, Mathematics::Vector3 origin, Mathematics::Vector3 direction, RayHit& hit,
             float maxDistance, std::uint32_t layerMask, bool ignoreTriggers)
{
    Physics::RayCastQuery query;
    if (!MakeRayQuery(origin, direction, maxDistance, layerMask, ignoreTriggers, query))
        return false;
    Physics::RayCastResult result;
    if (!physics.RayCast(query, result))
        return false;
    hit = ToScript(result);
    return true;
}

std::int32_t RayCastAll(Physics::PhysicsWorld& physics, Mathematics::Vector3 origin, Mathematics::Vector3 direction,
                        std::span<RayHit> hits, float maxDistance, std::uint32_t layerMask, bool ignoreTriggers)
{
    Physics::RayCastQuery query;
    if (!MakeRayQuery(origin, direction, maxDistance, layerMask, ignoreTriggers, query))
        return 0;
    std::vector<Physics::RayCastResult>& results = RayCastResults();
    physics.RayCastAll(query, results, std::numeric_limits<std::uint32_t>::max());
    const std::size_t written = std::min(results.size(), hits.size());
    for (std::size_t i = 0; i < written; ++i)
        hits[i] = ToScript(results[i]);
    return ResultCount(results.size());
}

std::int32_t BoxOverlap(Physics::PhysicsWorld& physics, Mathematics::Vector3 center, Mathematics::Vector3 halfExtents,
                        Mathematics::Quaternion rotation, std::span<ECS::EntityHandle> bodies, std::uint32_t layerMask,
                        bool ignoreTriggers)
{
    Physics::BoxOverlapQuery query;
    query.center = center;
    query.halfExtents = halfExtents;
    query.rotation = rotation;
    query.filter.layerMask = layerMask;
    query.filter.ignoreSensors = ignoreTriggers;
    std::vector<Physics::OverlapResult>& results = OverlapResults();
    physics.BoxOverlap(query, results, std::numeric_limits<std::uint32_t>::max());
    const std::size_t written = std::min(results.size(), bodies.size());
    // PhysicsECS stores the body's entity id as its user data.
    for (std::size_t i = 0; i < written; ++i)
        bodies[i] = ECS::EntityHandle(static_cast<std::uint32_t>(results[i].userData));
    return ResultCount(results.size());
}

Mathematics::Vector3 GetLinearVelocity(ECS::World& world, ECS::EntityHandle entity)
{
    const BodyRef body = FindBody(world, entity);
    return body.World != nullptr ? body.World->GetLinearVelocity(body.Body) : Mathematics::Vector3(0.0f, 0.0f, 0.0f);
}

void SetLinearVelocity(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 velocity)
{
    if (const BodyRef body = FindBody(world, entity); body.World != nullptr && !IgnoresScriptVelocity(world, entity))
        body.World->SetLinearVelocity(body.Body, velocity);
}

Mathematics::Vector3 GetAngularVelocity(ECS::World& world, ECS::EntityHandle entity)
{
    const BodyRef body = FindBody(world, entity);
    return body.World != nullptr ? body.World->GetAngularVelocity(body.Body) : Mathematics::Vector3(0.0f, 0.0f, 0.0f);
}

void SetAngularVelocity(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 velocity)
{
    if (const BodyRef body = FindBody(world, entity); body.World != nullptr && !IgnoresScriptVelocity(world, entity))
        body.World->SetAngularVelocity(body.Body, velocity);
}

void AddImpulse(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 impulse)
{
    if (const BodyRef body = FindBody(world, entity); body.World != nullptr)
        body.World->AddImpulse(body.Body, impulse);
}

void AddImpulseAtPosition(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 impulse,
                          Mathematics::Vector3 position)
{
    if (const BodyRef body = FindBody(world, entity); body.World != nullptr)
        body.World->AddImpulseAtPosition(body.Body, impulse, position);
}

Mathematics::Vector3 GetDesiredVelocity(ECS::World& world, ECS::EntityHandle entity)
{
    const Components::CharacterController* controller = world.GetComponent<Components::CharacterController>(entity);
    if (controller == nullptr)
        return Mathematics::Vector3(0.0f, 0.0f, 0.0f);
    return Mathematics::Vector3(controller->desiredVelocityX, controller->desiredVelocityY, controller->desiredVelocityZ);
}

void SetDesiredVelocity(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 velocity)
{
    Components::CharacterController* controller = world.GetComponentForWrite<Components::CharacterController>(entity);
    if (controller == nullptr)
        return;
    controller->desiredVelocityX = velocity.x;
    controller->desiredVelocityY = velocity.y;
    controller->desiredVelocityZ = velocity.z;
}

bool IsGrounded(ECS::World& world, ECS::EntityHandle entity)
{
    const Components::CharacterController* controller = world.GetComponent<Components::CharacterController>(entity);
    return controller != nullptr && controller->grounded;
}

SubscriptionId SubscribeContactBegin(ECS::EntityHandle scope, ScriptCallback<ContactEvent> callback)
{
    return PhysicsEntityEventCallbacks::SubscribeContactBegin(scope, Relay<EntityContactEvent>(callback), EndedRelay(callback));
}

void UnsubscribeContactBegin(SubscriptionId subscription)
{
    PhysicsEntityEventCallbacks::Unsubscribe(subscription);
}

SubscriptionId SubscribeContactEnd(ECS::EntityHandle scope, ScriptCallback<ContactEvent> callback)
{
    return PhysicsEntityEventCallbacks::SubscribeContactEnd(scope, Relay<EntityContactEvent>(callback), EndedRelay(callback));
}

void UnsubscribeContactEnd(SubscriptionId subscription)
{
    PhysicsEntityEventCallbacks::Unsubscribe(subscription);
}

SubscriptionId SubscribeTriggerEnter(ECS::EntityHandle scope, ScriptCallback<TriggerEvent> callback)
{
    return PhysicsEntityEventCallbacks::SubscribeTriggerEnter(scope, Relay<EntityTriggerEvent>(callback), EndedRelay(callback));
}

void UnsubscribeTriggerEnter(SubscriptionId subscription)
{
    PhysicsEntityEventCallbacks::Unsubscribe(subscription);
}

SubscriptionId SubscribeTriggerExit(ECS::EntityHandle scope, ScriptCallback<TriggerEvent> callback)
{
    return PhysicsEntityEventCallbacks::SubscribeTriggerExit(scope, Relay<EntityTriggerEvent>(callback), EndedRelay(callback));
}

void UnsubscribeTriggerExit(SubscriptionId subscription)
{
    PhysicsEntityEventCallbacks::Unsubscribe(subscription);
}

} // namespace GameEngine::PhysicsECS::Script
