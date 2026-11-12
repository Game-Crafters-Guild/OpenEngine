#include "PhysicsECS/Systems/PhysicsStepSystem.h"

#include "PhysicsECS/PhysicsWorldService.h"

#include "PhysicsECS/Components/KinematicBody.h"
#include "PhysicsECS/Components/PhysicsBody.h"

#include "Components/Transform.h"
#include "ECS/Query.h"
#include "Physics/PhysicsWorld.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace GameEngine::PhysicsECS
{
namespace
{
bool BitEqual(const Physics::Transform& a, const Physics::Transform& b)
{
    return std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0 &&
           std::memcmp(&a.rotation.GetGLM(), &b.rotation.GetGLM(), sizeof(a.rotation.GetGLM())) == 0;
}

Physics::Transform PoseOf(const GameEngine::Components::WorldTransform& worldTransform)
{
    GameEngine::Components::Transform worldPose{};
    worldPose.Basis = worldTransform.Basis;
    const auto rotation = worldPose.GetRotation().GetGLM();
    Physics::Transform pose{};
    pose.position = worldTransform.Position;
    pose.rotation = Physics::Quaternion(rotation.w, rotation.x, rotation.y, rotation.z);
    return pose;
}

bool HandleLess(const Physics::BodyHandle& a, const Physics::BodyHandle& b)
{
    return a.Value() < b.Value();
}
} // namespace

PhysicsStepSystem::MovingBody* PhysicsStepSystem::FindMovingBody(Physics::BodyHandle body)
{
    const auto it = std::lower_bound(m_Moving.begin(), m_Moving.end(), body,
                                     [](const MovingBody& moving, const Physics::BodyHandle& handle)
                                     { return HandleLess(moving.body, handle); });
    return (it != m_Moving.end() && it->body.Value() == body.Value()) ? &*it : nullptr;
}

// A kinematic body follows its entity: the game, a script or the navigation movement system moves the
// Transform, and before the frame's steps the body gets the velocity that reaches that pose over `duration`,
// the time the steps cover. Moving by velocity, not by a teleport, lets the body push and carry the dynamic
// bodies it touches. The pass visits the KinematicBody entities in chunks whose WorldTransform changed since
// the last frame that ran a step. A body moved on that frame that is not moved again is stopped, so a resting
// body costs nothing. A frame that runs no step leaves every body alone. The body takes the pose the Transform
// holds when the step starts; a system that moves the Transform after physics, such as the navigation movement
// system, reaches the body on the next frame.
void PhysicsStepSystem::MoveKinematicBodies(ECS::World& world, Physics::PhysicsWorld& physics, float32 duration)
{
    if (duration <= 0.0f)
        return;

    const std::uint64_t generation = PhysicsWorldService::GetWorldGeneration();
    if (world.GetWorldId() != m_WorldId || generation != m_PhysicsWorldGeneration)
    {
        m_Moving.clear();
        m_TransformGate = {};
        m_WorldId = world.GetWorldId();
        m_PhysicsWorldGeneration = generation;
    }

    const uint64_t entryVersion = world.GetGlobalSystemVersion();
    m_NextMoving.clear();
    auto query = world.Query<ECS::Read<GameEngine::Components::WorldTransform>,
                             ECS::Read<GameEngine::Components::PhysicsBody>>();
    query.With<GameEngine::Components::KinematicBody>();
    if (ECS::ChangeFilter::Enabled())
        query.Changed<GameEngine::Components::WorldTransform>(m_TransformGate);
    query.Each(
        [&](ECS::EntityHandle /*e*/,
            const GameEngine::Components::WorldTransform& worldTransform,
            const GameEngine::Components::PhysicsBody& body)
        {
            if (!body.initialized || !physics.IsBodyValid(body.body))
                return;
            const Physics::Transform pose = PoseOf(worldTransform);
            // A body on the list whose Transform did not move has arrived, and StopArrivedBodies places it. A
            // body off the list rests on its exact pose unless its Transform moved.
            MovingBody* moving = FindMovingBody(body.body);
            if (moving ? BitEqual(moving->target, pose) : BitEqual(physics.GetBodyTransform(body.body), pose))
                return;
            if (moving)
                moving->moved = true;
            physics.MoveKinematic(body.body, pose, duration);
            m_NextMoving.push_back({body.body, pose, false});
        });
    m_TransformGate.LastRunVersion = entryVersion;

    StopArrivedBodies(physics);
    std::sort(m_NextMoving.begin(), m_NextMoving.end(),
              [](const MovingBody& a, const MovingBody& b) { return HandleLess(a.body, b.body); });
    m_Moving.swap(m_NextMoving);
    PhysicsWorldService::SetLastKinematicBodiesMoving(static_cast<uint32>(m_Moving.size()));
}

// A body that reached its pose keeps the velocity of its last move, and its integrated pose is off the
// target by rounding. It is placed on the exact pose with no velocity, so it stops, stays put and can sleep.
void PhysicsStepSystem::StopArrivedBodies(Physics::PhysicsWorld& physics)
{
    const Physics::Vector3 zero(0.0f, 0.0f, 0.0f);
    for (const MovingBody& arrived : m_Moving)
    {
        if (arrived.moved || !physics.IsBodyValid(arrived.body))
            continue;
        physics.SetBodyTransform(arrived.body, arrived.target, Physics::ActivationMode::DontActivate);
        physics.SetLinearVelocity(arrived.body, zero);
        physics.SetAngularVelocity(arrived.body, zero);
    }
}

void PhysicsStepSystem::Update(ECS::World& world, float32 deltaTime)
{
    auto* w = PhysicsWorldService::TryGet();
    if (!w)
        return;

    // Time the step so the editor Monitors panel can chart the physics cost of a frame.
    const auto stepStart = std::chrono::steady_clock::now();

    const auto& s = PhysicsWorldService::GetSettings();
    const int32 collisionSteps = (s.collisionSteps > 0) ? s.collisionSteps : 1;

    // A call outside SystemManager::Update sees step 0 and counts as a frame of one step.
    const ECS::SimulationClock& clock = world.GetSimulationClock();
    const uint32 stepInFrame = clock.GetStepInFrame();
    const bool firstStepOfFrame = stepInFrame <= 1;
    if (firstStepOfFrame)
    {
        const uint32 stepsThisFrame = (stepInFrame == 0) ? 1u : clock.GetStepsThisFrame();
        MoveKinematicBodies(world, *w, static_cast<float32>(stepsThisFrame) * deltaTime);
    }

    // Snapshot previous transforms for interpolation (prev <- current).
    world.Query<ECS::Write<GameEngine::Components::PhysicsBody>>()
        .Each(
        [&](ECS::EntityHandle /*e*/, GameEngine::Components::PhysicsBody& body)
        {
            if (!body.initialized || !w->IsBodyValid(body.body))
                return;
            body.prevPhysicsTransform = w->GetBodyTransform(body.body);
            body.hasPrevPhysicsTransform = true;
        });

    w->Step(deltaTime, collisionSteps);

    const auto stepEnd = std::chrono::steady_clock::now();
    const float32 stepMs = static_cast<float32>(std::chrono::duration<double, std::milli>(stepEnd - stepStart).count());
    PhysicsWorldService::AddStepMs(stepMs);
}
} // namespace GameEngine::PhysicsECS
