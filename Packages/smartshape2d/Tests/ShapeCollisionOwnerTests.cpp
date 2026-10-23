#include "SmartShape2D/ShapeCollisionOwner.h"
#include "SmartShape2D/GeneratedShapeResources.h"
#include "SmartShape2D/ShapeCollision.h"
#include "SmartShape2D/ShapeGeometry.h"
#include "SmartShape2D/SmartShape.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "PhysicsECS/Components/MeshColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/MeshColliderData.h"
#include "Spline/SplineData.h"

#include <gtest/gtest.h>

namespace GameEngine::SmartShape2D
{
namespace
{
GenerationalVector::Handle MakeCollision()
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.Closed = true;
    spline.AddPoint({-1, -1, 0}, 1);
    spline.AddPoint({1, -1, 0}, 1);
    spline.AddPoint({1, 1, 0}, 1);
    spline.AddPoint({-1, 1, 0}, 1);
    return PhysicsECS::MeshColliderData::Register(BuildCollision(BuildShape(spline, {}), true, Components::SmartShape{}));
}

TEST(SmartShapeCollisionOwner, RebuildAndDisableKeepParentAlive)
{
    ECS::World world;
    const auto owner = world.CreateEntity();
    const auto first = MakeCollision();
    ASSERT_TRUE(SetShapeCollision(world, owner, first));
    ASSERT_TRUE(world.HasComponent<Components::MeshColliderShape>(owner));
    EXPECT_EQ(world.GetComponent<Components::PhysicsBody>(owner)->motionType, Physics::MotionType::Static);
    auto collider = *world.GetComponent<Components::PhysicsCollider>(owner);
    collider.isTrigger = true;
    world.AddComponentImmediate(owner, collider);
    const auto replacement = MakeCollision();
    ASSERT_TRUE(SetShapeCollision(world, owner, replacement));
    EXPECT_TRUE(world.GetComponent<Components::PhysicsCollider>(owner)->isTrigger);
    EXPECT_FALSE(PhysicsECS::MeshColliderData::Read(first));
    EXPECT_EQ(world.GetComponent<Components::MeshColliderShape>(owner)->DataGeneration, replacement.Generation());
    RemoveShapeCollision(world, owner);
    EXPECT_TRUE(world.IsValid(owner));
    EXPECT_FALSE(world.HasComponent<Components::MeshColliderShape>(owner));
    EXPECT_FALSE(world.HasComponent<Components::PhysicsBody>(owner));
    EXPECT_FALSE(world.HasComponent<Components::PhysicsCollider>(owner));
    EXPECT_FALSE(PhysicsECS::MeshColliderData::Read(replacement));
}

TEST(SmartShapeCollisionOwner, ExistingPhysicsSettingsSurviveRebuildAndRemoval)
{
    ECS::World world;
    const auto owner = world.CreateEntity();
    Components::PhysicsBody body{};
    body.motionType = Physics::MotionType::Static;
    body.linearDamping = 0.37f;
    world.AddComponentImmediate(owner, body);
    Components::PhysicsCollider collider{};
    collider.isTrigger = true;
    collider.belongsToMask = 0x15;
    world.AddComponentImmediate(owner, collider);
    ASSERT_TRUE(SetShapeCollision(world, owner, MakeCollision()));
    ASSERT_TRUE(SetShapeCollision(world, owner, MakeCollision()));
    RemoveShapeCollision(world, owner);
    ASSERT_TRUE(world.HasComponent<Components::PhysicsBody>(owner));
    EXPECT_FLOAT_EQ(world.GetComponent<Components::PhysicsBody>(owner)->linearDamping, 0.37f);
    ASSERT_TRUE(world.HasComponent<Components::PhysicsCollider>(owner));
    EXPECT_TRUE(world.GetComponent<Components::PhysicsCollider>(owner)->isTrigger);
    EXPECT_EQ(world.GetComponent<Components::PhysicsCollider>(owner)->belongsToMask, 0x15u);
}

TEST(SmartShapeCollisionOwner, DoesNotReplaceAnotherProvidersMesh)
{
    ECS::World world;
    const auto owner = world.CreateEntity();
    const auto authored = MakeCollision();
    Components::MeshColliderShape mesh{};
    mesh.DataIndex = authored.Index();
    mesh.DataGeneration = authored.Generation();
    world.AddComponentImmediate(owner, mesh);
    const auto generated = MakeCollision();
    EXPECT_FALSE(SetShapeCollision(world, owner, generated));
    RemoveShapeCollision(world, owner);
    EXPECT_EQ(world.GetComponent<Components::MeshColliderShape>(owner)->DataIndex, authored.Index());
    EXPECT_TRUE(PhysicsECS::MeshColliderData::Read(authored));
    PhysicsECS::MeshColliderData::Release(authored);
    PhysicsECS::MeshColliderData::Release(generated);
}
}
}
