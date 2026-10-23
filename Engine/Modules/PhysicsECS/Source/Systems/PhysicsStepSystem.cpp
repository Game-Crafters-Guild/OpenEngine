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
    std::memcpy(worldPose.matrix, worldTransform.matrix, sizeof(worldPose.matrix));
    const auto position = worldPose.GetPosition();
    const auto rotation = worldPose.GetRotation().GetGLM();
    Physics::Transform pose{};
    pose.position = Physics::Vector3(position.x, position.y, position.z);
    pose.rotation = Physics::Quaternion(rotation.w, rotation.x, rotation.y, rotation.z);
    return pose;
}

bool HandleLess(const Physics::BodyHandle& a, const Physics::BodyHandle& b)
{
    return a.Value() < b.Value();
}

// The number of fixed steps the accumulator holds, at most maxSubSteps. The step loop subtracts the same
// fixed step the same number of times.
int32 CountFixedSteps(float32 accumulator, float32 fixedTimeStep, int32 maxSubSteps)
{
    int32 steps = 0;
    while (accumulator >= fixedTimeStep && steps < maxSubSteps)
    {
        accumulator -= fixedTimeStep;
        ++steps;
    }
    return steps;
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

    // Time the whole step (all fixed sub-steps + interpolation snapshot) so the
    // editor Monitors panel can chart per-frame physics cost. This is the clean
    // simulation boundary; measuring here keeps the timing decoupled from the
    // scheduler and from Core diagnostics.
    const auto stepStart = std::chrono::steady_clock::now();

    const auto& s = PhysicsWorldService::GetSettings();
    const int32 collisionSteps = (s.collisionSteps > 0) ? s.collisionSteps : 1;

    // Fixed-timestep stepping (recommended for stability and determinism).
    // Note: PhysicsWorld is currently a global singleton, so this accumulator is effectively global too.
    static float32 accumulator = 0.0f;
    static std::uint64_t lastGen = 0;
    const std::uint64_t gen = PhysicsWorldService::GetWorldGeneration();
    if (gen != lastGen)
    {
        accumulator = 0.0f;
        lastGen = gen;
    }

    const float32 fixed = s.fixedTimeStep;
    const int32 maxSub = (s.maxSubSteps > 0) ? s.maxSubSteps : 1;

    if (fixed > 0.0f)
    {
        // Clamp to avoid "spiral of death" on long frames (e.g., debugger break).
        const float32 clampedDt = (deltaTime > 0.25f) ? 0.25f : ((deltaTime < 0.0f) ? 0.0f : deltaTime);
        accumulator += clampedDt;

        const int32 steps = CountFixedSteps(accumulator, fixed, maxSub);
        MoveKinematicBodies(world, *w, static_cast<float32>(steps) * fixed);
        for (int32 step = 0; step < steps; ++step)
        {
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

            w->Step(fixed, collisionSteps);
            accumulator -= fixed;
        }

        const float32 alpha = (fixed > 0.0f) ? (accumulator / fixed) : 1.0f;
        PhysicsWorldService::SetRenderInterpolationAlpha(alpha);
    }
    else
    {
        // Variable timestep fallback.
        MoveKinematicBodies(world, *w, deltaTime);
        w->Step(deltaTime, collisionSteps);
        PhysicsWorldService::SetRenderInterpolationAlpha(1.0f);
    }

    const auto stepEnd = std::chrono::steady_clock::now();
    PhysicsWorldService::SetLastStepMs(
        static_cast<float32>(std::chrono::duration<double, std::milli>(stepEnd - stepStart).count()));
}
} // namespace GameEngine::PhysicsECS
