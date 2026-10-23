#include "SmartShape2D/ShapeCollision.h"
#include "SmartShape2D/ShapeGeometry.h"
#include "SmartShape2D/SmartShape.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "PhysicsECS/Components/MeshColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/MeshColliderData.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Systems/PhysicsInitSystem.h"
#include "PhysicsECS/Systems/PhysicsWorldHooks.h"
#include "Spline/SplineData.h"

#include <gtest/gtest.h>

namespace GameEngine::SmartShape2D
{
namespace
{
Physics::MeshShapeDef BoxCollision(float width = 2.0f)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.Closed = true;
    spline.AddPoint({-width, -1, 0}, 1.0f);
    spline.AddPoint({width, -1, 0}, 1.0f);
    spline.AddPoint({width, 1, 0}, 1.0f);
    spline.AddPoint({-width, 1, 0}, 1.0f);
    return BuildCollision(BuildShape(spline, {}), true, Components::SmartShape{});
}

TEST(SmartShapeCollision, ClosedFillExtrudesWithoutInternalWalls)
{
    const auto mesh = BoxCollision();
    ASSERT_EQ(mesh.vertices.size(), 8u);
    EXPECT_EQ(mesh.indices.size(), 36u);
    for (size_t i = 0; i < 4; ++i)
    {
        EXPECT_FLOAT_EQ(mesh.vertices[i].z, -0.5f);
        EXPECT_FLOAT_EQ(mesh.vertices[i + 4].z, 0.5f);
    }
}

TEST(SmartShapeCollision, ReleasedHandleCannotAliasReplacementAndReadersKeepTheirData)
{
    const auto original = PhysicsECS::MeshColliderData::Register(BoxCollision());
    ASSERT_TRUE(original.IsValid());
    const auto read = PhysicsECS::MeshColliderData::Read(original);
    ASSERT_TRUE(read);
    PhysicsECS::MeshColliderData::Release(original);
    const auto replacement = PhysicsECS::MeshColliderData::Register(BoxCollision(3.0f));
    EXPECT_FALSE(PhysicsECS::MeshColliderData::Read(original));
    EXPECT_FLOAT_EQ(read->vertices.front().x, -2.0f);
    ASSERT_TRUE(PhysicsECS::MeshColliderData::Read(replacement));
    EXPECT_FLOAT_EQ(PhysicsECS::MeshColliderData::Read(replacement)->vertices.front().x, -3.0f);
    PhysicsECS::MeshColliderData::Release(replacement);
}

TEST(SmartShapeCollision, InvalidTopologyIsRejected)
{
    auto mesh = BoxCollision();
    mesh.indices[0] = 100000;
    EXPECT_FALSE(PhysicsECS::MeshColliderData::Register(std::move(mesh)).IsValid());
    EXPECT_FALSE(PhysicsECS::MeshColliderData::Register({}).IsValid());
}

class SmartShapePhysicsTest : public testing::Test
{
protected:
    void SetUp() override
    {
        Physics::PhysicsWorldSettings settings{};
        settings.numThreads = 1;
        settings.maxBodies = 128;
        settings.maxBodyPairs = 128;
        settings.maxContactConstraints = 128;
        PhysicsECS::PhysicsWorldService::Initialize(settings);
    }
    void TearDown() override { PhysicsECS::PhysicsWorldService::Shutdown(); }
};

TEST_F(SmartShapePhysicsTest, GeneratedColliderBuildsRebuildsAndReleasesThroughPhysicsECS)
{
    ECS::World world;
    PhysicsECS::RegisterPhysicsWorldHooks(world);
    const auto first = PhysicsECS::MeshColliderData::Register(BoxCollision());
    const auto second = PhysicsECS::MeshColliderData::Register(BoxCollision(3.0f));
    auto entity = world.Create();
    entity.Set(Components::Transform{});
    entity.Set(Components::WorldTransform{});
    Components::PhysicsCollider collider{};
    collider.layer = Physics::Layers::Static;
    entity.Set(collider);
    Components::PhysicsBody body{};
    body.motionType = Physics::MotionType::Static;
    entity.Set(body);
    Components::MeshColliderShape mesh{};
    mesh.DataIndex = first.Index();
    mesh.DataGeneration = first.Generation();
    entity.Set(mesh);
    world.ProcessCommands();
    PhysicsECS::PhysicsInitSystem initialize;
    initialize.Update(world, 0.0f);
    const auto* built = world.GetComponent<Components::PhysicsBody>(entity.GetHandle());
    ASSERT_TRUE(built);
    ASSERT_TRUE(built->initialized);
    const auto originalBody = built->body;
    EXPECT_TRUE(PhysicsECS::PhysicsWorldService::Get().IsBodyValid(originalBody));
    mesh.DataIndex = second.Index();
    mesh.DataGeneration = second.Generation();
    entity.Set(mesh);
    world.ProcessCommands();
    initialize.Update(world, 0.0f);
    built = world.GetComponent<Components::PhysicsBody>(entity.GetHandle());
    ASSERT_TRUE(built->initialized);
    const auto replacementBody = built->body;
    EXPECT_NE(originalBody, replacementBody);
    world.DestroyEntityImmediate(entity.GetHandle());
    EXPECT_FALSE(PhysicsECS::PhysicsWorldService::Get().IsBodyValid(replacementBody));
    PhysicsECS::MeshColliderData::Release(first);
    PhysicsECS::MeshColliderData::Release(second);
}
}
}
