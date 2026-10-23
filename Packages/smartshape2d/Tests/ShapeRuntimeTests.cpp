#include "SmartShape2D/ShapeRuntime.h"
#include "Components/Hierarchy.h"
#include "Components/Spline/SplineComponent.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "PhysicsECS/Components/MeshColliderShape.h"
#include "PhysicsECS/MeshColliderData.h"
#include "SplineECS/SplineService.h"

#include <gtest/gtest.h>

namespace GameEngine::SmartShape2D
{
class SmartShapeRuntimeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        SplineECS::SplineService::Initialize();
        ShapeRuntime::Get().Initialize(nullptr);
    }
    void TearDown() override
    {
        ShapeRuntime::Get().Shutdown();
        SplineECS::SplineService::Shutdown();
    }
};

TEST_F(SmartShapeRuntimeTest, HeadlessCollisionStaysOnParentAcrossReloadDisableAndRemoval)
{
    ECS::World world;
    auto& service = SplineECS::SplineService::Get();
    const auto handle = service.CreateSpline(Spline::SplineType::Linear, true);
    auto* spline = service.GetSplineData(handle);
    spline->AddPoint({-1, -1, 0}, 1);
    spline->AddPoint({1, -1, 0}, 1);
    spline->AddPoint({1, 1, 0}, 1);
    spline->AddPoint({-1, 1, 0}, 1);
    const auto owner = world.CreateEntity();
    Components::SplineComponent reference{};
    reference.SplineDataIndex = handle.Index();
    reference.SplineDataGeneration = handle.Generation();
    world.AddComponentImmediate(owner, reference);
    world.AddComponentImmediate(owner, Components::SmartShape{});
    auto& runtime = ShapeRuntime::Get();
    runtime.Update(world);
    ASSERT_TRUE(world.HasComponent<Components::MeshColliderShape>(owner));
    const auto original = world.GetComponent<Components::GeneratedShapeResources>(owner)->CollisionData;
    EXPECT_TRUE(PhysicsECS::MeshColliderData::Read(original));
    runtime.Shutdown();
    runtime.Initialize(nullptr);
    runtime.Update(world);
    ASSERT_TRUE(world.HasComponent<Components::MeshColliderShape>(owner));
    EXPECT_FALSE(PhysicsECS::MeshColliderData::Read(original));
    uint32 children = 0;
    world.Query<ECS::Read<Components::Parent>>().Each([&children](const Components::Parent&) { ++children; });
    EXPECT_EQ(children, 0u);
    Components::SmartShape settings{};
    settings.CollisionEnabled = false;
    world.AddComponentImmediate(owner, settings);
    runtime.Update(world);
    EXPECT_TRUE(world.IsValid(owner));
    EXPECT_FALSE(world.HasComponent<Components::MeshColliderShape>(owner));
    settings.CollisionEnabled = true;
    world.AddComponentImmediate(owner, settings);
    runtime.Update(world);
    ASSERT_TRUE(world.HasComponent<Components::MeshColliderShape>(owner));
    world.RemoveComponentImmediate<Components::SmartShape>(owner);
    runtime.Update(world);
    EXPECT_TRUE(world.IsValid(owner));
    EXPECT_FALSE(world.HasComponent<Components::MeshColliderShape>(owner));
}
TEST_F(SmartShapeRuntimeTest, RestoredGeneratedChildrenAreAdoptedAndRetiredWithTheirOwner)
{
    ECS::World world;
    const auto owner = world.CreateEntity();
    const auto child = world.CreateEntity();
    Components::GeneratedShapeResources resources{};
    resources.Owner = owner;
    resources.Part = 0;
    world.AddComponentImmediate(child, resources);
    world.AddComponentImmediate(child, Components::Parent{owner});
    const auto snapshot = world.SerializeWorld();
    auto& runtime = ShapeRuntime::Get();
    // No SmartShape remains: normal retirement must remove its generated child.
    runtime.Update(world);
    EXPECT_FALSE(world.IsValid(child));
    world.DeserializeWorld(snapshot);
    ASSERT_TRUE(world.IsValid(child));
    runtime.Update(world);
    EXPECT_FALSE(world.IsValid(child));
    EXPECT_TRUE(world.IsValid(owner));
}

}
