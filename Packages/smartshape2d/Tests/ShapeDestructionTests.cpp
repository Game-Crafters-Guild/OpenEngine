#include "SmartShape2D/ShapeDestruction.h"
#include "SmartShape2D/ShapeExplosion.h"
#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include "SmartShape2D/ShapeCollision.h"
#include "SmartShape2D/ShapeGeometry.h"
#include "SmartShape2D/ShapeMesh.h"
#include "SmartShape2D/SmartShape.h"
#include "SmartShape2D/SmartShapeCut.h"
#include "Spline/SplineData.h"
#include "PhysicsECS/PhysicsWorldService.h"

#include <gtest/gtest.h>
#include <cmath>

namespace GameEngine::SmartShape2D
{
namespace
{
Mesh Rectangle()
{
    Mesh mesh;
    AppendShapeQuad(mesh, {-2, -2}, {2, -2}, {2, 2}, {-2, 2}, 0, 1, 0, false);
    return mesh;
}

float Area(const Mesh& mesh)
{
    float area = 0;
    for (size_t index = 0; index < mesh.Indices.size(); index += 3)
    {
        const auto& a = mesh.Vertices[mesh.Indices[index]];
        const auto& b = mesh.Vertices[mesh.Indices[index + 1]];
        const auto& c = mesh.Vertices[mesh.Indices[index + 2]];
        area += std::abs((b.Position[0] - a.Position[0]) * (c.Position[1] - a.Position[1]) -
                         (b.Position[1] - a.Position[1]) * (c.Position[0] - a.Position[0])) * 0.5f;
    }
    return area;
}
}

TEST(ShapeDestruction, InteriorExplosionRemovesAHoleAndPreservesTextureCoordinates)
{
    auto mesh = Rectangle();
    const Components::SmartShapeCut cut{};
    const auto cuts = BuildShapeCuts({&cut, 1});
    CutShapeMesh(mesh, cuts);
    EXPECT_NEAR(Area(mesh), 16.0f - 3.121445f, 0.001f);
    for (const auto& vertex : mesh.Vertices)
    {
        EXPECT_NEAR(vertex.TexCoords[0], (vertex.Position[0] + 2.0f) * 0.25f, 0.0001f);
        EXPECT_NEAR(vertex.TexCoords[1], (vertex.Position[1] + 2.0f) * 0.25f, 0.0001f);
    }
}

TEST(ShapeDestruction, RepeatedCutIsIdempotentAndCompleteCutEmptiesGeometry)
{
    auto mesh = Rectangle();
    Components::SmartShapeCut cut{};
    auto cuts = BuildShapeCuts({&cut, 1});
    CutShapeMesh(mesh, cuts);
    const float once = Area(mesh);
    CutShapeMesh(mesh, cuts);
    EXPECT_NEAR(Area(mesh), once, 0.001f);
    cut.AxisX = {10, 0};
    cut.AxisY = {0, 10};
    cuts = BuildShapeCuts({&cut, 1});
    CutShapeMesh(mesh, cuts);
    EXPECT_TRUE(mesh.Indices.empty());
}

TEST(ShapeDestruction, ReflectedEllipseHasPositiveWindingAndCorrectRemovedArea)
{
    auto mesh = Rectangle();
    const Components::SmartShapeCut cut{{}, {-1, 0}, {0, 0.5f}};
    const auto cuts = BuildShapeCuts({&cut, 1});
    CutShapeMesh(mesh, cuts);
    EXPECT_NEAR(Area(mesh), 16.0f - 3.121445f * 0.5f, 0.001f);
}

TEST(ShapeDestruction, CompleteExplosionRemovesCollisionEvenWhenVisualFillIsDisabled)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.Closed = true;
    for (const Mathematics::Vector3 point : {Mathematics::Vector3{-2,-2,0}, {2,-2,0}, {2,2,0}, {-2,2,0}})
        spline.AddPoint(point, 1.0f);
    Components::SmartShape settings;
    settings.FillEnabled = false;
    const auto geometry = BuildShape(spline, settings);
    const Components::SmartShapeCut cut{{}, {10,0}, {0,10}};
    const auto cuts = BuildShapeCuts({&cut, 1});
    EXPECT_FALSE(BuildCollision(geometry, true, settings).indices.empty());
    EXPECT_TRUE(BuildCollision(geometry, true, settings, cuts).indices.empty());
}
TEST(ShapeDestruction, ExplosionCollisionHasWallsOnlyAtThePerimeterAndHole)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.Closed = true;
    for (const Mathematics::Vector3 point : {Mathematics::Vector3{-2,-2,0}, {2,-2,0}, {2,2,0}, {-2,2,0}})
        spline.AddPoint(point, 1.0f);
    Components::SmartShape settings;
    const auto geometry = BuildShape(spline, settings);
    const Components::SmartShapeCut cut{};
    const auto collision = BuildCollision(geometry, true, settings, BuildShapeCuts({&cut, 1}));
    float wallArea = 0;
    for (size_t index = 0; index < collision.indices.size(); index += 3)
    {
        const auto a = collision.vertices[collision.indices[index]];
        const auto b = collision.vertices[collision.indices[index + 1]];
        const auto c = collision.vertices[collision.indices[index + 2]];
        if (a.z == b.z && b.z == c.z)
            continue;
        wallArea += Mathematics::Vector3::Cross(b - a, c - a).Length() * 0.5f;
    }
    EXPECT_NEAR(wallArea, 16.0f + 64.0f * std::sin(3.14159265f / 32.0f), 0.001f);
}

TEST(ShapeDestruction, ExplosionRespectsOptInAndResetRemovesOnlyDamage)
{
    ECS::World world;
    const auto terrain = world.CreateEntity();
    Components::SmartShape shape;
    shape.Destructible = true;
    world.AddComponentImmediate(terrain, shape);
    world.AddComponentImmediate(terrain, Components::WorldTransform{});
    const auto decoration = world.CreateEntity();
    world.AddComponentImmediate(decoration, Components::SmartShape{});
    world.AddComponentImmediate(decoration, Components::WorldTransform{});
    EXPECT_EQ(ExplodeShapes(world, {1, 2, 0}, 0.5f), 1u);
    size_t count = 0;
    world.Query<ECS::Read<Components::SmartShapeCut>, ECS::Read<Components::Parent>>()
        .Each([&count, terrain](const Components::SmartShapeCut& cut, const Components::Parent& parent)
        {
            EXPECT_EQ(parent.Entity, terrain);
            EXPECT_FLOAT_EQ(cut.Center.x, 1);
            EXPECT_FLOAT_EQ(cut.Center.y, 2);
            EXPECT_FLOAT_EQ(cut.AxisX.x, 0.5f);
            ++count;
        });
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(ExplodeShapes(world, {}, -1), 0u);
    ResetShapeDestruction(world);
    EXPECT_EQ(world.Query<ECS::Read<Components::SmartShapeCut>>().Count(), 0u);
    EXPECT_TRUE(world.IsValid(terrain));
    EXPECT_TRUE(world.IsValid(decoration));
}

TEST(ShapeDestruction, ExplosionWakesSleepingBodiesInTheBlast)
{
    PhysicsECS::PhysicsWorldService::Initialize();
    auto& physics = PhysicsECS::PhysicsWorldService::Get();
    Physics::BoxShapeDef definition;
    definition.halfExtents = {0.25f, 0.25f, 0.25f};
    Physics::BodySettings settings;
    settings.shape = physics.CreateShape(definition);
    settings.motionType = Physics::MotionType::Dynamic;
    settings.layer = Physics::Layers::Dynamic;
    settings.startAwake = false;
    const auto body = physics.CreateBody(settings);
    ECS::World world;
    const auto terrain = world.CreateEntity();
    Components::SmartShape shape;
    shape.Destructible = true;
    world.AddComponentImmediate(terrain, shape);
    world.AddComponentImmediate(terrain, Components::WorldTransform{});
    constexpr float kStepSeconds = 1.0f / 60.0f;
    physics.Step(kStepSeconds);
    EXPECT_FLOAT_EQ(physics.GetBodyTransform(body).position.y, 0.0f);
    ExplodeShapes(world, {}, 1.0f);
    for (int step = 0; step < 30; ++step)
        physics.Step(kStepSeconds);
    EXPECT_LT(physics.GetBodyTransform(body).position.y, -0.5f);
    PhysicsECS::PhysicsWorldService::Shutdown();
}

}
