#include <gtest/gtest.h>

#include "Editor/Entities/EditorECSHelpers.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Physics/PhysicsWorld.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Systems/PhysicsInitSystem.h"

using GameEngine::Components::PhysicsBody;
using GameEngine::Components::PhysicsCollider;
using GameEngine::Components::SphereColliderShape;
using GameEngine::Components::Transform;
using GameEngine::Components::WorldTransform;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;
using GameEngine::PhysicsECS::PhysicsWorldService;

namespace
{

class PhysicsBodyTeleportTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (PhysicsWorldService::IsInitialized())
            PhysicsWorldService::Shutdown();
        GameEngine::Physics::PhysicsWorldSettings settings{};
        settings.numThreads = 1;
        settings.maxBodies = 64;
        settings.maxBodyPairs = 64;
        settings.maxContactConstraints = 64;
        settings.tempAllocatorBytes = 4u * 1024u * 1024u;
        PhysicsWorldService::Initialize(settings);
    }

    void TearDown() override
    {
        if (PhysicsWorldService::IsInitialized())
            PhysicsWorldService::Shutdown();
    }

    // A dynamic sphere at `position`, with its body built.
    EntityHandle CreateBody(const GameEngine::Mathematics::WorldPosition& position)
    {
        const Transform transform =
            Transform::FromTRS(position, Quaternion(1.0f, 0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
        const EntityHandle e = m_World.Create().GetHandle();
        m_World.AddComponentImmediate(e, transform);
        WorldTransform wt{};
        wt.Position = transform.Position;
        wt.Basis = transform.Basis;
        m_World.AddComponentImmediate(e, wt);
        m_World.AddComponentImmediate(e, PhysicsCollider{});
        SphereColliderShape sphere{};
        sphere.radius = 0.5f;
        m_World.AddComponentImmediate(e, sphere);
        PhysicsBody body{};
        body.motionType = GameEngine::Physics::MotionType::Dynamic;
        m_World.AddComponentImmediate(e, body);
        m_Init.Update(m_World, 1.0f / 60.0f);
        return e;
    }

    World m_World;
    GameEngine::PhysicsECS::PhysicsInitSystem m_Init;
};

// A body 300 km out lands at the edited pose and stops.
TEST_F(PhysicsBodyTeleportTests, TeleportPutsAFarBodyAtTheEditedPoseAndStopsIt)
{
    const EntityHandle e = CreateBody(GameEngine::Mathematics::WorldPosition{300000.0, 5.0, 0.0});
    GameEngine::Physics::PhysicsWorld* physics = PhysicsWorldService::TryGet();
    const PhysicsBody* body = m_World.GetComponent<PhysicsBody>(e);
    ASSERT_TRUE(body && body->initialized && physics->IsBodyValid(body->body)) << "the fixture built no body";
    physics->SetLinearVelocity(body->body, GameEngine::Physics::Vector3(3.0f, 0.0f, 0.0f));

    ASSERT_TRUE(GameEngine::Editor::TeleportPhysicsBody(&m_World, e, Vector3(-511.75f, 7.5f, 100.125f),
                                                        Quaternion(1.0f, 0.0f, 0.0f, 0.0f)));

    const GameEngine::Physics::Transform pose = physics->GetBodyTransform(body->body);
    EXPECT_NEAR(pose.position.X, -511.75, 1.0e-4);
    EXPECT_NEAR(pose.position.Y, 7.5, 1.0e-4);
    EXPECT_NEAR(pose.position.Z, 100.125, 1.0e-4);
    EXPECT_EQ(physics->GetLinearVelocity(body->body).x, 0.0f);
}

} // namespace
