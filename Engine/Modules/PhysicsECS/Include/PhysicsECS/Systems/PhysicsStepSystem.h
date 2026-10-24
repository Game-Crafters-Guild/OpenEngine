#pragma once

#include "ECS/ChangeFilter.h"
#include "ECS/Systems.h"
#include "Physics/PhysicsTypes.h"
#include "Types/Types.h"

#include <cstdint>
#include <vector>

namespace GameEngine::Physics
{
class PhysicsWorld;
}

namespace GameEngine::PhysicsECS
{
// Moves each kinematic body toward its entity's WorldTransform, then steps the physics world: in fixed steps
// when fixedTimeStep is above 0, else one step of the frame's deltaTime.
class PhysicsStepSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "PhysicsStepSystem"; }
    // Contact and trigger handlers run inside the step and call the engine's main-thread script API
    // (entity.Physics, an unsubscribe), so the step runs on the updating thread, outside any job,
    // whatever else its wave holds.
    bool RequiresExclusiveUpdate() const override { return true; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    // A kinematic body the pass gave a velocity, and the pose it moves to.
    struct MovingBody
    {
        Physics::BodyHandle body{};
        Physics::Transform target{};
        bool moved = false;
    };

    void MoveKinematicBodies(ECS::World& world, Physics::PhysicsWorld& physics, float32 duration);
    MovingBody* FindMovingBody(Physics::BodyHandle body);
    void StopArrivedBodies(Physics::PhysicsWorld& physics);

    // The bodies moved on the last frame that ran a step, sorted by handle. On the next such frame, a body
    // whose Transform did not move has arrived: it is placed on its exact pose, its velocity is set to zero
    // and it leaves the list. Handles belong to one physics world, so the list is cleared when the world
    // changes.
    std::vector<MovingBody> m_Moving;
    std::vector<MovingBody> m_NextMoving;
    ECS::ChangeGate m_TransformGate;
    uint64 m_WorldId = 0;
    std::uint64_t m_PhysicsWorldGeneration = 0;
};
} // namespace GameEngine::PhysicsECS
