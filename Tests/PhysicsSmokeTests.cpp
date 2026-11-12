#include <gtest/gtest.h>

#include "EngineLogCapture.h"
#include "PagedTiledTerrain.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Physics/PhysicsWorld.h"
#include "PhysicsECS/HeightFieldDataProvider.h"
#include "PhysicsECS/ProceduralMeshColliders.h"
#include "ProceduralMeshCollidersInternal.h"
#include "PhysicsECS/PhysicsEntityEventCallbacks.h"
#include "PhysicsECS/PhysicsScriptApi.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CapsuleColliderShape.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/KinematicBody.h"
#include "PhysicsECS/Components/MeshColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PhysicsColliderOwner.h"
#include "PhysicsECS/Components/PhysicsWorldSettingsComponent.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "PhysicsECS/Systems/PhysicsInitSystem.h"
#include "PhysicsECS/Systems/PhysicsStepSystem.h"
#include "PhysicsECS/Systems/PhysicsWorldBootstrapSystem.h"
#include "PhysicsECS/Systems/PhysicsWorldHooks.h"
#include "PhysicsECS/Systems/PhysicsWritebackSystem.h"
#include "PhysicsECS/Systems/RegisterPhysicsSystems.h"

#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/Systems/RegisterTerrainSystems.h"
#include "TerrainECS/Systems/TerrainPhysicsSystem.h"
#include "TerrainECS/Systems/TerrainWorldHooks.h"
#include "TerrainECS/Components/TerrainPageCollider.h"
#include "TerrainECS/Components/TerrainPlanetPageCollider.h"
#include "TerrainECS/Planet/PlanetChart.h"
#include "TerrainECS/Planet/PlanetPageRequester.h"
#include "TerrainECS/TerrainHeightFunction/ITerrainHeightFunction.h"
#include "TerrainECS/TerrainHeightPageDriver.h"
#include "TerrainECS/TerrainPages.h"
#include "Rendering/Core/Device.h"
#include "Components/Terrain/Terrain.h"
#include "Components/RuntimeOnlyEntity.h"

#include "Components/Transform.h"
#include "Core/CpuProfiler.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Scene/SceneIO.h"
#include "Memory/AllocationCountScope.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
static GameEngine::Physics::PhysicsWorldSettings SmallDeterministicSettings()
{
    using namespace GameEngine::Physics;
    PhysicsWorldSettings settings{};
    settings.gravity = Vector3(0.0f, -9.81f, 0.0f);
    settings.numThreads = 1;
    settings.maxBodies = 1024;
    settings.maxBodyPairs = 1024;
    settings.maxContactConstraints = 1024;
    settings.tempAllocatorBytes = 16u * 1024u * 1024u;
    return settings;
}

struct ScopedPhysicsWorldService
{
    explicit ScopedPhysicsWorldService(const GameEngine::Physics::PhysicsWorldSettings& settings)
    {
        using namespace GameEngine::PhysicsECS;
        // Ensure a clean global state for this test.
        if (PhysicsWorldService::IsInitialized())
            PhysicsWorldService::Shutdown();
        PhysicsEntityEventCallbacks::ClearAll();

        PhysicsWorldService::Initialize(settings);
        PhysicsEntityEventCallbacks::NotifyWorldStateChanged();
    }

    ~ScopedPhysicsWorldService()
    {
        using namespace GameEngine::PhysicsECS;
        PhysicsEntityEventCallbacks::ClearAll();
        if (PhysicsWorldService::IsInitialized())
            PhysicsWorldService::Shutdown();
    }
};

// A fresh TerrainService (which registers the real heightfield provider) for one test.
struct ScopedTerrainService
{
    ScopedTerrainService()
    {
        using GameEngine::TerrainECS::TerrainService;
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
    }

    ~ScopedTerrainService()
    {
        using GameEngine::TerrainECS::TerrainService;
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }
};

TEST(Physics, FallingSphereHitsPlane)
{
    using namespace GameEngine::Physics;

    PhysicsWorldSettings settings = SmallDeterministicSettings();

    PhysicsWorld world(settings);

    // Verify contact callbacks fire (high-level event plumbing test).
    int contactBeginCount = 0;
    world.SetContactCallback([&](const ContactEvent& e)
                             {
                                 if (e.state == ContactState::Begin)
                                     ++contactBeginCount;
                             });

    // Static plane at y=0 with normal +Y.
    PlaneShapeDef plane{};
    plane.normal = Vector3(0.0f, 1.0f, 0.0f);
    plane.d = 0.0f;
    plane.halfExtent = 2000.0f;
    const ShapeHandle planeShape = world.CreateShape(plane);
    ASSERT_TRUE(planeShape.IsValid());

    BodySettings ground{};
    ground.shape = planeShape;
    ground.motionType = MotionType::Static;
    ground.layer = Layers::Static;
    ground.userData = 111;
    const BodyHandle groundBody = world.CreateBody(ground);
    ASSERT_TRUE(groundBody.IsValid());

    // Dynamic sphere above the plane.
    const float32 radius = 0.5f;
    SphereShapeDef sphere{};
    sphere.radius = radius;
    const ShapeHandle sphereShape = world.CreateShape(sphere);
    ASSERT_TRUE(sphereShape.IsValid());

    BodySettings ball{};
    ball.shape = sphereShape;
    ball.motionType = MotionType::Dynamic;
    ball.layer = Layers::Dynamic;
    ball.mass = 1.0f;
    ball.position = WorldPosition{0.0, 5.0, 0.0};
    ball.startAwake = true;
    ball.userData = 222;
    const BodyHandle ballBody = world.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    // Step for a few seconds.
    constexpr float32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i)
    {
        world.Step(dt, 1);
    }

    EXPECT_GT(contactBeginCount, 0);

    const Transform t = world.GetBodyTransform(ballBody);
    EXPECT_LT(t.position.Y, 5.0f);
    // Should not tunnel through the plane.
    EXPECT_GT(t.position.Y, radius * 0.5f);
}

// 300 km from the origin a float holds a position to 31 mm. An impulse 10 mm off the center
// of a body there spins it: the point is exact.
TEST(Physics, ImpulseAtAPointFarFromTheOriginSpinsTheBody)
{
    using namespace GameEngine::Physics;

    PhysicsWorld world(SmallDeterministicSettings());
    SphereShapeDef sphere{};
    sphere.radius = 0.5f;
    BodySettings ball{};
    ball.shape = world.CreateShape(sphere);
    ball.motionType = MotionType::Dynamic;
    ball.layer = Layers::Dynamic;
    ball.mass = 1.0f;
    ball.position = WorldPosition{300000.0, 0.0, 0.0};
    ball.startAwake = true;
    const BodyHandle body = world.CreateBody(ball);
    ASSERT_TRUE(body.IsValid());

    // A unit impulse along +Y at 10 mm along +X: the angular impulse is 0.01 about +Z, and a
    // solid sphere of 1 kg and radius 0.5 m has an inertia of 0.1.
    world.AddImpulseAtPosition(body, Vector3(0.0f, 1.0f, 0.0f), WorldPosition{300000.01, 0.0, 0.0});
    EXPECT_NEAR(world.GetAngularVelocity(body).z, 0.1f, 1e-3f);
    EXPECT_NEAR(world.GetLinearVelocity(body).y, 1.0f, 1e-4f);
}

// A ball that lands on a box 300 km from the origin reports the contact point under its center,
// to the millimeter: a float would put it on the 31 mm grid.
TEST(Physics, ContactPointFarFromTheOriginIsExact)
{
    using namespace GameEngine::Physics;

    PhysicsWorld world(SmallDeterministicSettings());
    WorldPosition contact{};
    bool touched = false;
    world.SetContactCallback([&](const ContactEvent& e)
                             {
                                 if (e.state != ContactState::Begin || touched)
                                     return;
                                 touched = true;
                                 contact = e.point;
                             });

    constexpr double kFar = 300000.0;
    BoxShapeDef box{};
    box.halfExtents = Vector3(5.0f, 0.5f, 5.0f);
    BodySettings ground{};
    ground.shape = world.CreateShape(box);
    ground.motionType = MotionType::Static;
    ground.layer = Layers::Static;
    ground.position = WorldPosition{kFar, 0.0, 0.0};
    ASSERT_TRUE(world.CreateBody(ground).IsValid());

    SphereShapeDef sphere{};
    sphere.radius = 0.5f;
    BodySettings ball{};
    ball.shape = world.CreateShape(sphere);
    ball.motionType = MotionType::Dynamic;
    ball.layer = Layers::Dynamic;
    ball.mass = 1.0f;
    ball.position = WorldPosition{kFar + 0.012, 1.1, 0.0};
    ball.startAwake = true;
    ASSERT_TRUE(world.CreateBody(ball).IsValid());

    for (int i = 0; i < 60 && !touched; ++i)
        world.Step(1.0f / 60.0f, 1);

    ASSERT_TRUE(touched);
    EXPECT_NEAR(contact.X, kFar + 0.012, 1e-3);
    EXPECT_NEAR(contact.Z, 0.0, 1e-3);
}

TEST(Physics, EntityCallbacks_ContactBeginEnd)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;

    PhysicsWorldSettings settings = SmallDeterministicSettings();
    ScopedPhysicsWorldService svc(settings);
    auto& world = PhysicsWorldService::Get();

    constexpr uint64 groundEid = 1001;
    constexpr uint64 ballEid = 1002;

    int beginCount = 0;
    int endCount = 0;
    bool sawOtherGroundOnBegin = false;

    const auto beginId = PhysicsEntityEventCallbacks::SubscribeContactBegin(
        ECS::EntityHandle(static_cast<uint32>(ballEid)),
        [&](const EntityContactEvent& e)
        {
            ++beginCount;
            if (e.other == ECS::EntityHandle(static_cast<uint32>(groundEid)))
                sawOtherGroundOnBegin = true;
        });

    const auto endId = PhysicsEntityEventCallbacks::SubscribeContactEnd(
        ECS::EntityHandle(static_cast<uint32>(ballEid)),
        [&](const EntityContactEvent& /*e*/)
        {
            ++endCount;
        });

    // Static plane at y=0 with normal +Y.
    PlaneShapeDef plane{};
    plane.normal = Vector3(0.0f, 1.0f, 0.0f);
    plane.d = 0.0f;
    plane.halfExtent = 2000.0f;
    const ShapeHandle planeShape = world.CreateShape(plane);
    ASSERT_TRUE(planeShape.IsValid());

    BodySettings ground{};
    ground.shape = planeShape;
    ground.motionType = MotionType::Static;
    ground.layer = Layers::Static;
    ground.restitution = 0.9f;
    ground.userData = groundEid; // PhysicsECS convention: ECS entity id
    const BodyHandle groundBody = world.CreateBody(ground);
    ASSERT_TRUE(groundBody.IsValid());

    // Dynamic sphere above the plane with high restitution so it bounces (generates End events).
    SphereShapeDef sphere{};
    sphere.radius = 0.5f;
    const ShapeHandle sphereShape = world.CreateShape(sphere);
    ASSERT_TRUE(sphereShape.IsValid());

    BodySettings ball{};
    ball.shape = sphereShape;
    ball.motionType = MotionType::Dynamic;
    ball.layer = Layers::Dynamic;
    ball.mass = 1.0f;
    ball.position = WorldPosition{0.0, 5.0, 0.0};
    ball.startAwake = true;
    ball.restitution = 0.9f;
    ball.userData = ballEid; // PhysicsECS convention: ECS entity id
    const BodyHandle ballBody = world.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    constexpr float32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 600; ++i)
        world.Step(dt, 1);

    EXPECT_GT(beginCount, 0);
    EXPECT_TRUE(sawOtherGroundOnBegin);
    EXPECT_GT(endCount, 0);

    PhysicsEntityEventCallbacks::Unsubscribe(beginId);
    PhysicsEntityEventCallbacks::Unsubscribe(endId);
}

TEST(Physics, EntityCallbacks_TriggerEnterExit)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;

    PhysicsWorldSettings settings = SmallDeterministicSettings();
    ScopedPhysicsWorldService svc(settings);
    auto& world = PhysicsWorldService::Get();

    constexpr uint64 triggerEid = 2001;
    constexpr uint64 ballEid = 2002;

    int triggerEnterFromTrigger = 0;
    int triggerExitFromTrigger = 0;
    int triggerEnterFromOther = 0;
    int triggerExitFromOther = 0;

    PhysicsEntityEventCallbacks::SubscribeTriggerEnter(
        ECS::EntityHandle(static_cast<uint32>(triggerEid)),
        [&](const EntityTriggerEvent& e)
        {
            if (e.selfIsTrigger)
                ++triggerEnterFromTrigger;
        });
    PhysicsEntityEventCallbacks::SubscribeTriggerExit(
        ECS::EntityHandle(static_cast<uint32>(triggerEid)),
        [&](const EntityTriggerEvent& e)
        {
            if (e.selfIsTrigger)
                ++triggerExitFromTrigger;
        });

    PhysicsEntityEventCallbacks::SubscribeTriggerEnter(
        ECS::EntityHandle(static_cast<uint32>(ballEid)),
        [&](const EntityTriggerEvent& e)
        {
            if (!e.selfIsTrigger)
                ++triggerEnterFromOther;
        });
    PhysicsEntityEventCallbacks::SubscribeTriggerExit(
        ECS::EntityHandle(static_cast<uint32>(ballEid)),
        [&](const EntityTriggerEvent& e)
        {
            if (!e.selfIsTrigger)
                ++triggerExitFromOther;
        });

    // Sensor box centered at origin, top face at y=0.
    BoxShapeDef box{};
    box.halfExtents = Vector3(10.0f, 0.5f, 10.0f);
    const ShapeHandle boxShape = world.CreateShape(box);
    ASSERT_TRUE(boxShape.IsValid());

    BodySettings sensor{};
    sensor.shape = boxShape;
    sensor.motionType = MotionType::Static;
    sensor.layer = Layers::Static;
    sensor.isSensor = true;
    sensor.position = WorldPosition{0.0, -0.5, 0.0};
    sensor.userData = triggerEid;
    const BodyHandle sensorBody = world.CreateBody(sensor);
    ASSERT_TRUE(sensorBody.IsValid());

    // Dynamic sphere above the sensor.
    SphereShapeDef sphere{};
    sphere.radius = 0.5f;
    const ShapeHandle sphereShape = world.CreateShape(sphere);
    ASSERT_TRUE(sphereShape.IsValid());

    BodySettings ball{};
    ball.shape = sphereShape;
    ball.motionType = MotionType::Dynamic;
    ball.layer = Layers::Dynamic;
    ball.mass = 1.0f;
    ball.position = WorldPosition{0.0, 5.0, 0.0};
    ball.startAwake = true;
    ball.userData = ballEid;
    const BodyHandle ballBody = world.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    constexpr float32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i)
        world.Step(dt, 1);

    EXPECT_GT(triggerEnterFromTrigger, 0);
    EXPECT_GT(triggerExitFromTrigger, 0);
    EXPECT_GT(triggerEnterFromOther, 0);
    EXPECT_GT(triggerExitFromOther, 0);
}

TEST(Physics, EntityCallbacks_UnsubscribeStopsCallbacks)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;

    PhysicsWorldSettings settings = SmallDeterministicSettings();
    ScopedPhysicsWorldService svc(settings);
    auto& world = PhysicsWorldService::Get();

    constexpr uint64 groundEid = 3001;
    constexpr uint64 ballEid = 3002;

    int beginCount = 0;
    const auto id = PhysicsEntityEventCallbacks::SubscribeContactBegin(
        ECS::EntityHandle(static_cast<uint32>(ballEid)),
        [&](const EntityContactEvent& /*e*/)
        {
            ++beginCount;
        });
    PhysicsEntityEventCallbacks::Unsubscribe(id);

    // Static plane at y=0 with normal +Y.
    PlaneShapeDef plane{};
    plane.normal = Vector3(0.0f, 1.0f, 0.0f);
    plane.d = 0.0f;
    plane.halfExtent = 2000.0f;
    const ShapeHandle planeShape = world.CreateShape(plane);
    ASSERT_TRUE(planeShape.IsValid());

    BodySettings ground{};
    ground.shape = planeShape;
    ground.motionType = MotionType::Static;
    ground.layer = Layers::Static;
    ground.userData = groundEid;
    const BodyHandle groundBody = world.CreateBody(ground);
    ASSERT_TRUE(groundBody.IsValid());

    // Dynamic sphere above the plane.
    SphereShapeDef sphere{};
    sphere.radius = 0.5f;
    const ShapeHandle sphereShape = world.CreateShape(sphere);
    ASSERT_TRUE(sphereShape.IsValid());

    BodySettings ball{};
    ball.shape = sphereShape;
    ball.motionType = MotionType::Dynamic;
    ball.layer = Layers::Dynamic;
    ball.mass = 1.0f;
    ball.position = WorldPosition{0.0, 5.0, 0.0};
    ball.startAwake = true;
    ball.userData = ballEid;
    const BodyHandle ballBody = world.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    constexpr float32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i)
        world.Step(dt, 1);

    EXPECT_EQ(beginCount, 0);
}

namespace
{
// A static plane at y=0 and a bouncing sphere dropped from y=5, with the ECS entity ids as user data.
void DropBallOnPlane(GameEngine::Physics::PhysicsWorld& world, GameEngine::uint64 groundEid, GameEngine::uint64 ballEid)
{
    using namespace GameEngine::Physics;
    PlaneShapeDef plane{};
    plane.normal = Vector3(0.0f, 1.0f, 0.0f);
    plane.halfExtent = 2000.0f;
    BodySettings ground{};
    ground.shape = world.CreateShape(plane);
    ground.motionType = MotionType::Static;
    ground.layer = Layers::Static;
    ground.restitution = 0.9f;
    ground.userData = groundEid;
    ASSERT_TRUE(world.CreateBody(ground).IsValid());

    SphereShapeDef sphere{};
    sphere.radius = 0.5f;
    BodySettings ball{};
    ball.shape = world.CreateShape(sphere);
    ball.motionType = MotionType::Dynamic;
    ball.layer = Layers::Dynamic;
    ball.position = WorldPosition{0.0, 5.0, 0.0};
    ball.restitution = 0.9f;
    ball.userData = ballEid;
    ASSERT_TRUE(world.CreateBody(ball).IsValid());
}
} // namespace

TEST(Physics, EntityCallbacks_HandlerThatUnsubscribesItselfLeavesTheOthersCalled)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    constexpr uint64 groundEid = 3101;
    constexpr uint64 ballEid = 3102;
    const ECS::EntityHandle ballEntity(static_cast<uint32>(ballEid));

    // The first contact reaches both handlers; the first one ends its own subscription while the
    // delivery runs, and the second must still be called for that contact.
    int step = 0;
    int onceCount = 0;
    int onceStep = -1;
    bool otherCalledWithOnce = false;
    PhysicsEntityEventCallbacks::SubscriptionId onceId = 0;
    onceId = PhysicsEntityEventCallbacks::SubscribeContactBegin(ballEntity, [&](const EntityContactEvent&)
                                                                {
                                                                    ++onceCount;
                                                                    onceStep = step;
                                                                    PhysicsEntityEventCallbacks::Unsubscribe(onceId);
                                                                });
    PhysicsEntityEventCallbacks::SubscribeContactBegin(ballEntity, [&](const EntityContactEvent&)
                                                       { otherCalledWithOnce = otherCalledWithOnce || step == onceStep; });

    DropBallOnPlane(PhysicsWorldService::Get(), groundEid, ballEid);
    for (step = 0; step < 600; ++step)
        PhysicsWorldService::Get().Step(1.0f / 60.0f, 1);

    EXPECT_EQ(onceCount, 1);
    EXPECT_TRUE(otherCalledWithOnce);
}

TEST(Physics, EntityCallbacks_EndedRunsOnceWhenTheSubscriptionEnds)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    const ECS::EntityHandle entity(static_cast<uint32>(3201));
    int unsubscribedEnded = 0;
    int clearedEnded = 0;
    const auto unsubscribed = PhysicsEntityEventCallbacks::SubscribeContactBegin(
        entity, [](const EntityContactEvent&) {}, [&] { ++unsubscribedEnded; });
    PhysicsEntityEventCallbacks::SubscribeTriggerEnter(entity, [](const EntityTriggerEvent&) {}, [&] { ++clearedEnded; });

    PhysicsEntityEventCallbacks::Unsubscribe(unsubscribed);
    PhysicsEntityEventCallbacks::Unsubscribe(unsubscribed);
    EXPECT_EQ(unsubscribedEnded, 1);
    EXPECT_EQ(clearedEnded, 0);

    PhysicsEntityEventCallbacks::ClearAll();
    EXPECT_EQ(unsubscribedEnded, 1);
    EXPECT_EQ(clearedEnded, 1);
}

TEST(Physics, EntityCallbacks_WorldShutdownEndsEverySubscription)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    int ended = 0;
    PhysicsEntityEventCallbacks::SubscribeContactBegin(ECS::EntityHandle(static_cast<uint32>(3301)),
                                                       [](const EntityContactEvent&) {}, [&] { ++ended; });

    PhysicsWorldService::Shutdown();

    EXPECT_EQ(ended, 1);
}

// The last subscription ends inside its own handler while another world contact listener exists:
// the router's world listeners stay in place for the rest of that delivery, so the other listener
// still sees the contact.
TEST(Physics, EntityCallbacks_LastSubscriptionEndingInItsHandlerKeepsLaterWorldListeners)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    constexpr uint64 groundEid = 3401;
    constexpr uint64 ballEid = 3402;
    int step = 0;
    int oursStep = -1;
    int ours = 0;
    bool laterSawIt = false;
    PhysicsEntityEventCallbacks::SubscriptionId id = 0;
    id = PhysicsEntityEventCallbacks::SubscribeContactBegin(ECS::EntityHandle(static_cast<uint32>(ballEid)),
                                                            [&](const EntityContactEvent&)
                                                            {
                                                                ++ours;
                                                                oursStep = step;
                                                                PhysicsEntityEventCallbacks::Unsubscribe(id);
                                                            });
    const auto later = PhysicsWorldService::Get().AddContactListener(
        [&](const Physics::ContactEvent& e)
        {
            if (e.state == Physics::ContactState::Begin && step == oursStep)
                laterSawIt = true;
        });
    DropBallOnPlane(PhysicsWorldService::Get(), groundEid, ballEid);
    for (step = 0; step < 600; ++step)
        PhysicsWorldService::Get().Step(1.0f / 60.0f, 1);
    PhysicsWorldService::Get().RemoveContactListener(later);

    EXPECT_EQ(ours, 1);
    EXPECT_TRUE(laterSawIt);
}

namespace
{
// A Simulation system with no access that shares the physics step's wave.
class StepWavePeerSystem : public GameEngine::ECS::ISystem
{
  public:
    static constexpr GameEngine::ECS::SystemCadence Cadence = GameEngine::ECS::SystemCadence::Simulation;
    const char* GetName() const override { return "StepWavePeer"; }
    void Update(GameEngine::ECS::World&, GameEngine::float32) override {}
};
} // namespace

// Event handlers run inside the physics step and call the script API, whose functions are for the
// main thread outside a job. Scheduled in one wave with other systems, the step still runs on the
// thread that updates the systems, outside any job, so a handler that sets a velocity and ends its
// own subscription runs where those calls are allowed.
TEST(Physics, PhysicsStepRunsOnTheCallerOutsideAJobInASharedWave)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    JobSystem::WorkStealingThreadPool pool(2);
    ECS::SystemManager manager(&pool);
    ECS::World world;
    manager.AddSystem<StepWavePeerSystem>();
    manager.AddSystem<PhysicsStepSystem>();
    manager.AddSystem<StepWavePeerSystem>();
    ECS::SystemExecutionPlan plan;
    plan.Waves.push_back({{0, 1, 2}, ECS::SystemCadence::Simulation});
    manager.SetExecutionPlan(std::move(plan));

    constexpr uint64 groundEid = 3701;
    constexpr uint64 ballEid = 3702;
    const std::thread::id caller = std::this_thread::get_id();
    int calls = 0;
    bool onCaller = false;
    bool inJob = true;
    PhysicsEntityEventCallbacks::SubscriptionId id = 0;
    id = PhysicsEntityEventCallbacks::SubscribeContactBegin(
        ECS::EntityHandle(static_cast<uint32>(ballEid)),
        [&](const EntityContactEvent&)
        {
            ++calls;
            onCaller = std::this_thread::get_id() == caller;
            inJob = JobSystem::WorkStealingThreadPool::IsExecutingJob();
            PhysicsEntityEventCallbacks::Unsubscribe(id);
        });
    DropBallOnPlane(PhysicsWorldService::Get(), groundEid, ballEid);
    const float32 fixed = static_cast<float32>(world.GetSimulationClock().GetFixedDeltaSeconds());
    for (int frame = 0; frame < 600 && calls == 0; ++frame)
        manager.Update(world, fixed);

    ASSERT_EQ(calls, 1);
    EXPECT_TRUE(onCaller);
    EXPECT_FALSE(inJob);
}

// The contact normal points from other into self, for each of the two entities.
TEST(Physics, EntityCallbacks_ContactNormalPointsFromOtherIntoSelf)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    constexpr uint64 groundEid = 3501;
    constexpr uint64 ballEid = 3502;
    float ballNormalY = 0.0f;
    float groundNormalY = 0.0f;
    PhysicsEntityEventCallbacks::SubscribeContactBegin(ECS::EntityHandle(static_cast<uint32>(ballEid)),
                                                       [&](const EntityContactEvent& e)
                                                       {
                                                           if (ballNormalY == 0.0f)
                                                               ballNormalY = e.normal.y;
                                                       });
    PhysicsEntityEventCallbacks::SubscribeContactBegin(ECS::EntityHandle(static_cast<uint32>(groundEid)),
                                                       [&](const EntityContactEvent& e)
                                                       {
                                                           if (groundNormalY == 0.0f)
                                                               groundNormalY = e.normal.y;
                                                       });
    DropBallOnPlane(PhysicsWorldService::Get(), groundEid, ballEid);
    for (int i = 0; i < 600; ++i)
        PhysicsWorldService::Get().Step(1.0f / 60.0f, 1);

    EXPECT_GT(ballNormalY, 0.5f) << "self is the ball: from the ground up into the ball";
    EXPECT_LT(groundNormalY, -0.5f) << "self is the ground: from the ball down into the ground";
}

TEST(Physics, TriggerDoesNotBlockFallingSphere)
{
    using namespace GameEngine::Physics;

    PhysicsWorldSettings settings = SmallDeterministicSettings();
    PhysicsWorld world(settings);

    int triggerEnterCount = 0;
    int triggerExitCount = 0;
    world.SetTriggerCallback([&](const TriggerEvent& e)
                             {
                                 if (e.isEntering)
                                     ++triggerEnterCount;
                                 else
                                     ++triggerExitCount;
                             });

    // Sensor box centered at origin, top face at y=0.
    BoxShapeDef box{};
    box.halfExtents = Vector3(10.0f, 0.5f, 10.0f);
    const ShapeHandle boxShape = world.CreateShape(box);
    ASSERT_TRUE(boxShape.IsValid());

    BodySettings sensor{};
    sensor.shape = boxShape;
    sensor.motionType = MotionType::Static;
    sensor.layer = Layers::Static;
    sensor.isSensor = true;
    sensor.position = WorldPosition{0.0, -0.5, 0.0};
    sensor.userData = 333;
    const BodyHandle sensorBody = world.CreateBody(sensor);
    ASSERT_TRUE(sensorBody.IsValid());

    // Dynamic sphere above the sensor.
    SphereShapeDef sphere{};
    sphere.radius = 0.5f;
    const ShapeHandle sphereShape = world.CreateShape(sphere);
    ASSERT_TRUE(sphereShape.IsValid());

    BodySettings ball{};
    ball.shape = sphereShape;
    ball.motionType = MotionType::Dynamic;
    ball.layer = Layers::Dynamic;
    ball.mass = 1.0f;
    ball.position = WorldPosition{0.0, 5.0, 0.0};
    ball.startAwake = true;
    ball.userData = 444;
    const BodyHandle ballBody = world.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    constexpr float32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i)
        world.Step(dt, 1);

    EXPECT_GT(triggerEnterCount, 0);
    EXPECT_GT(triggerExitCount, 0);

    const Transform t = world.GetBodyTransform(ballBody);
    // Should have fallen through the sensor box (below y = -0.5).
    EXPECT_LT(t.position.Y, -1.0f);
}

TEST(Physics, LayerMatrixCanDisableDynamicVsStatic)
{
    using namespace GameEngine::Physics;

    PhysicsWorldSettings settings = SmallDeterministicSettings();
    settings.layerCollisionMatrix.SetPair(Layers::Dynamic, Layers::Static, false);
    PhysicsWorld world(settings);

    PlaneShapeDef plane{};
    plane.normal = Vector3(0.0f, 1.0f, 0.0f);
    plane.d = 0.0f;
    plane.halfExtent = 2000.0f;
    const ShapeHandle planeShape = world.CreateShape(plane);
    ASSERT_TRUE(planeShape.IsValid());

    BodySettings ground{};
    ground.shape = planeShape;
    ground.motionType = MotionType::Static;
    ground.layer = Layers::Static;
    const BodyHandle groundBody = world.CreateBody(ground);
    ASSERT_TRUE(groundBody.IsValid());

    SphereShapeDef sphere{};
    sphere.radius = 0.5f;
    const ShapeHandle sphereShape = world.CreateShape(sphere);
    ASSERT_TRUE(sphereShape.IsValid());

    BodySettings ball{};
    ball.shape = sphereShape;
    ball.motionType = MotionType::Dynamic;
    ball.layer = Layers::Dynamic;
    ball.mass = 1.0f;
    ball.position = WorldPosition{0.0, 5.0, 0.0};
    ball.startAwake = true;
    const BodyHandle ballBody = world.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    constexpr float32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i)
        world.Step(dt, 1);

    const Transform t = world.GetBodyTransform(ballBody);
    // With filtering disabled, it should fall through y=0 plane.
    EXPECT_LT(t.position.Y, -1.0f);
}
TEST(Physics, HeightFieldShapeBlocksSphere)
{
    using namespace GameEngine::Physics;

    PhysicsWorld world(SmallDeterministicSettings());

    // Create a simple 8x8 heightfield at height 10.0 (raw values)
    const uint32 N = 8;
    std::vector<float> samples(N * N, 10.0f);

    HeightFieldShapeDef hfDef;
    hfDef.Samples = samples;
    hfDef.SampleCount = N;
    hfDef.Offset = Vector3(-50.0f, 0.0f, -50.0f);
    hfDef.Scale = Vector3(100.0f / (N - 1), 1.0f, 100.0f / (N - 1));
    // Surface at Y = offset.y + scale.y * 10 = 0 + 1 * 10 = 10

    const ShapeHandle hfShape = world.CreateShape(hfDef);
    ASSERT_TRUE(hfShape.IsValid()) << "HeightField shape creation failed";

    BodySettings ground{};
    ground.shape = hfShape;
    ground.motionType = MotionType::Static;
    ground.layer = Layers::Static;
    ground.position = WorldPosition{};
    const BodyHandle groundBody = world.CreateBody(ground);
    ASSERT_TRUE(groundBody.IsValid());

    world.OptimizeBroadphase();

    // Sphere starting above the heightfield
    SphereShapeDef sphere{};
    sphere.radius = 0.5f;
    const ShapeHandle sphereShape = world.CreateShape(sphere);
    ASSERT_TRUE(sphereShape.IsValid());

    BodySettings ball{};
    ball.shape = sphereShape;
    ball.motionType = MotionType::Dynamic;
    ball.layer = Layers::Dynamic;
    ball.mass = 1.0f;
    ball.position = WorldPosition{0.0, 20.0, 0.0}; // above surface at Y=10
    ball.startAwake = true;
    const BodyHandle ballBody = world.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    // Raycast to verify surface position
    RayCastQuery q{};
    q.origin = {0, 100, 0};
    q.direction = Vector3(0, -1, 0);
    q.maxDistance = 200.0f;
    RayCastResult hit{};
    (void)world.RayCast(q, hit);

    // Step physics
    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i)
        world.Step(dt, 1);

    const Transform t = world.GetBodyTransform(ballBody);

    // The sphere should have landed on the heightfield, NOT fallen through
    EXPECT_GT(t.position.Y, 5.0f)
        << "Sphere at Y=" << t.position.Y << " fell through heightfield (expected Y~10.5)";
}

TEST(Physics, HeightFieldVaryingHeightsRaycast)
{
    // Replicates the editor terrain scenario: 128x128 heightfield with varying
    // heights, large offset, and body at origin.
    using namespace GameEngine::Physics;

    PhysicsWorld world(SmallDeterministicSettings());

    const uint32 N = 128;
    std::vector<float> samples(N * N);
    for (uint32 z = 0; z < N; ++z)
        for (uint32 x = 0; x < N; ++x)
        {
            // Generate varying heights between ~9 and ~53 (matching editor terrain)
            float fx = static_cast<float>(x) / static_cast<float>(N - 1);
            float fz = static_cast<float>(z) / static_cast<float>(N - 1);
            samples[z * N + x] = 10.0f + 40.0f * (0.5f + 0.5f * std::sin(fx * 6.28f) * std::cos(fz * 6.28f));
        }

    const float32 sizeX = 256.0f;
    const float32 sizeZ = 256.0f;
    const float32 scaleX = sizeX / static_cast<float>(N - 1);
    const float32 scaleZ = sizeZ / static_cast<float>(N - 1);

    HeightFieldShapeDef hfDef;
    hfDef.Samples = samples;
    hfDef.SampleCount = N;
    hfDef.Offset = Vector3(-sizeX * 0.5f, 0.0f, -sizeZ * 0.5f);
    hfDef.Scale = Vector3(scaleX, 1.0f, scaleZ);

    const ShapeHandle hfShape = world.CreateShape(hfDef);
    ASSERT_TRUE(hfShape.IsValid());

    BodySettings ground{};
    ground.shape = hfShape;
    ground.motionType = MotionType::Static;
    ground.layer = Layers::Static;
    ground.position = WorldPosition{};
    const BodyHandle groundBody = world.CreateBody(ground);
    ASSERT_TRUE(groundBody.IsValid());

    world.OptimizeBroadphase();

    // Raycast down from well above the terrain
    RayCastQuery q{};
    q.origin = {50, 200, 50};
    q.direction = Vector3(0, -1, 0);
    q.maxDistance = 500.0f;
    RayCastResult hit{};
    bool didHit = world.RayCast(q, hit);

    ASSERT_TRUE(didHit) << "Raycast should hit the heightfield";
    // The surface at (50, ?, 50) should be between 9 and 53
    EXPECT_GT(hit.hitPoint.Y, 5.0f)
        << "Surface hit Y=" << hit.hitPoint.Y << " is too low (expected 9-53 range)";
    EXPECT_LT(hit.hitPoint.Y, 60.0f)
        << "Surface hit Y=" << hit.hitPoint.Y << " is too high (expected 9-53 range)";

    // Also test sphere collision
    SphereShapeDef sphere{};
    sphere.radius = 0.5f;
    const ShapeHandle sphereShape = world.CreateShape(sphere);
    ASSERT_TRUE(sphereShape.IsValid());

    BodySettings ball{};
    ball.shape = sphereShape;
    ball.motionType = MotionType::Dynamic;
    ball.layer = Layers::Dynamic;
    ball.mass = 1.0f;
    ball.position = WorldPosition{0.0, 80.0, 0.0};
    ball.startAwake = true;
    const BodyHandle ballBody = world.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 300; ++i)
        world.Step(dt, 1);

    const Transform t = world.GetBodyTransform(ballBody);
    EXPECT_GT(t.position.Y, 5.0f)
        << "Sphere at Y=" << t.position.Y << " fell through heightfield";
}

} // namespace



// ---- M8: PhysicsWriteback memcmp gate (change-signaling §5 P2) -----------
// PhysicsSleepersStayClean, unit shape: an unmoving body produces zero
// WorldTransform Version bumps and zero dirty-feed entries after the first
// converged writeback; a moving body produces exactly its own.

TEST(Physics, WritebackMemcmpGateSkipsUnchangedPose)
{
    using namespace GameEngine::Physics;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    PhysicsWorldSettings settings = SmallDeterministicSettings();
    ScopedPhysicsWorldService svc(settings);
    auto& pw = GameEngine::PhysicsECS::PhysicsWorldService::Get();

    SphereShapeDef sphereDef{};
    sphereDef.radius = 0.5f;
    const ShapeHandle shape = pw.CreateShape(sphereDef);
    ASSERT_TRUE(shape.IsValid());

    BodySettings bs{};
    bs.shape      = shape;
    bs.motionType = MotionType::Dynamic;
    bs.layer      = Layers::Dynamic;
    bs.mass       = 1.0f;
    bs.position   = WorldPosition{0.0, 5.0, 0.0};
    bs.startAwake = true;
    const BodyHandle body = pw.CreateBody(bs);
    ASSERT_TRUE(body.IsValid());

    GameEngine::ECS::World world;
    world.EnableComponentDirtyFeed(
        GameEngine::ECS::GetComponentTypeId<WorldTransform>());

    auto e = world.CreateEntity();
    world.AddComponentImmediate<Transform>(e, Transform{});
    world.AddComponentImmediate<WorldTransform>(e, WorldTransform{});
    PhysicsBody pb{};
    pb.body        = body;
    pb.initialized = true;
    world.AddComponentImmediate<PhysicsBody>(e, pb);

    GameEngine::PhysicsECS::PhysicsWritebackSystem writeback;
    world.GetComponentDirtyFeed().Reset();  // drop the structural-add entries

    auto feedEntryCount = [&] {
        std::vector<GameEngine::ECS::EntityHandle> out;
        world.GetComponentDirtyFeed().Snapshot(out);
        return out.size();
    };
    auto wtVersion = [&] {
        const auto* wt = world.GetComponent<WorldTransform>(e);
        return wt ? wt->Version : 0u;
    };

    // First writeback: body pose (y=5) differs from the identity
    // WorldTransform → write + bump + emit.
    writeback.Update(world, 1.0f / 60.0f);
    const auto v1 = wtVersion();
    EXPECT_GT(v1, 0u);
    EXPECT_EQ(feedEntryCount(), 1u);

    // No physics step → byte-identical pose → the memcmp gate skips the
    // write, the bump, AND the emission. This is what un-dirties sleeping
    // bodies every frame.
    writeback.Update(world, 1.0f / 60.0f);
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(wtVersion(), v1);
    EXPECT_EQ(feedEntryCount(), 1u);

    // Step → the falling body's pose changed → exactly one new bump + entry.
    pw.Step(1.0f / 60.0f, 1);
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_GT(wtVersion(), v1);
    EXPECT_EQ(feedEntryCount(), 2u);
}

// A resting body keeps its authored non-uniform scale and its stored bytes: after the first write the
// writeback skips the body on its unchanged inputs, so neither Transform nor WorldTransform is written
// again, whatever a GetScale and FromTRS cycle would round to.
TEST(Physics, WritebackLeavesARestingScaledRotatedBodyUnwritten)
{
    using namespace GameEngine::Physics;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    struct Pose
    {
        GameEngine::Mathematics::Vector3 scale;
        GameEngine::Mathematics::Quaternion rotation; // (w, x, y, z), a yaw about +Y
    };
    const Pose poses[] = {
        {{1.69371068f, 2.75987411f, 0.786742866f}, {0.687741101f, 0.0f, 0.725956082f, 0.0f}},
        {{1.96740484f, 1.36932528f, 1.10475266f}, {0.130486533f, 0.0f, 0.991450071f, 0.0f}},
    };

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = GameEngine::PhysicsECS::PhysicsWorldService::Get();

    SphereShapeDef sphereDef{};
    sphereDef.radius = 0.5f;
    const ShapeHandle shape = pw.CreateShape(sphereDef);
    ASSERT_TRUE(shape.IsValid());

    GameEngine::ECS::World world;
    std::vector<GameEngine::ECS::EntityHandle> entities;
    for (const Pose& pose : poses)
    {
        BodySettings bs{};
        bs.shape = shape;
        bs.motionType = MotionType::Dynamic;
        bs.layer = Layers::Dynamic;
        bs.mass = 1.0f;
        bs.position = WorldPosition{0.0, 5.0, 0.0};
        bs.rotation = pose.rotation;
        PhysicsBody pb{};
        pb.body = pw.CreateBody(bs);
        ASSERT_TRUE(pb.body.IsValid());
        pb.initialized = true;

        auto e = world.CreateEntity();
        world.AddComponentImmediate<Transform>(e, Transform::FromTRS(bs.position, pose.rotation, pose.scale));
        world.AddComponentImmediate<WorldTransform>(e, WorldTransform{});
        world.AddComponentImmediate<PhysicsBody>(e, pb);
        entities.push_back(e);
    }

    // No physics step: every body rests at its creation pose for the whole test.
    GameEngine::PhysicsECS::PhysicsWritebackSystem writeback;
    writeback.Update(world, 1.0f / 60.0f);

    std::vector<Transform> matrices;
    std::vector<GameEngine::uint32> versions;
    for (auto e : entities)
    {
        matrices.push_back(*world.GetComponent<Transform>(e));
        versions.push_back(world.GetComponent<WorldTransform>(e)->Version);
    }

    for (int frame = 0; frame < 64; ++frame)
    {
        writeback.Update(world, 1.0f / 60.0f);
    }

    for (size_t i = 0; i < entities.size(); ++i)
    {
        EXPECT_EQ(std::memcmp(&matrices[i], world.GetComponent<Transform>(entities[i]),
                              GameEngine::Components::kTransformPoseBytes),
                  0)
            << "pose " << i << ": the stored Transform was re-written";
        EXPECT_EQ(world.GetComponent<WorldTransform>(entities[i])->Version, versions[i])
            << "pose " << i << ": WorldTransform was re-written";
    }
}

// A body the writeback tests hold still: no physics step runs, so its backend pose changes only when a
// test moves it. Dynamic, because the writeback skips kinematic bodies. Scaled and yawed so the Transform
// is not a plain translation. The caller checks the body handle.
static GameEngine::ECS::EntityHandle AddStillBody(GameEngine::ECS::World& world,
                                                  GameEngine::Physics::PhysicsWorld& pw,
                                                  GameEngine::Physics::MotionType motionType =
                                                      GameEngine::Physics::MotionType::Dynamic)
{
    using namespace GameEngine::Physics;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    SphereShapeDef sphereDef{};
    sphereDef.radius = 0.5f;
    BodySettings bs{};
    bs.shape = pw.CreateShape(sphereDef);
    bs.motionType = motionType;
    bs.layer = motionType == MotionType::Kinematic ? Layers::Kinematic : Layers::Dynamic;
    bs.mass = 1.0f;
    bs.position = WorldPosition{1.0, 5.0, -2.0};
    bs.rotation = Quaternion(0.687741101f, 0.0f, 0.725956082f, 0.0f);
    PhysicsBody pb{};
    pb.motionType = motionType;
    pb.body = pw.CreateBody(bs);
    pb.initialized = true;

    auto e = world.CreateEntity();
    world.AddComponentImmediate<Transform>(
        e, Transform::FromTRS(bs.position, bs.rotation, GameEngine::Mathematics::Vector3(1.5f, 2.5f, 0.75f)));
    world.AddComponentImmediate<WorldTransform>(e, WorldTransform{});
    world.AddComponentImmediate<PhysicsBody>(e, pb);
    if (motionType == MotionType::Kinematic)
        world.AddComponentImmediate(e, GameEngine::Components::KinematicBody{});
    return e;
}

// The writeback decides "unchanged" on its inputs: once a resting body's pose is written, later frames
// skip it before reading the scale back or composing a matrix. A body that moves is composed again.
TEST(Physics, WritebackDoesNotRecomposeARestingBody)
{
    using GameEngine::Components::PhysicsBody;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    GameEngine::ECS::World world;
    const auto e = AddStillBody(world, pw);
    const GameEngine::Physics::BodyHandle body = world.GetComponent<PhysicsBody>(e)->body;
    ASSERT_TRUE(pw.IsBodyValid(body));

    GameEngine::PhysicsECS::PhysicsWritebackSystem writeback;
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 1u);

    for (int frame = 0; frame < 8; ++frame)
    {
        writeback.Update(world, 1.0f / 60.0f);
        EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 0u)
            << "frame " << frame << ": the resting body was re-composed";
    }

    GameEngine::Physics::Transform moved = pw.GetBodyTransform(body);
    moved.position.Y += 1.0;
    pw.SetBodyTransform(body, moved);
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 1u) << "the moved body was skipped";
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 0u);
}

// An edit to a resting body's Transform or WorldTransform does not survive the writeback: the body
// pose wins, as it did before the skip.
TEST(Physics, WritebackSnapsAnEditedTransformBackToTheBody)
{
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    GameEngine::ECS::World world;
    const auto e = AddStillBody(world, pw);
    ASSERT_TRUE(pw.IsBodyValid(world.GetComponent<PhysicsBody>(e)->body));

    // The translation column is the backend position exactly, whatever the scale read-back does.
    const GameEngine::Physics::Transform pose = pw.GetBodyTransform(world.GetComponent<PhysicsBody>(e)->body);

    GameEngine::PhysicsECS::PhysicsWritebackSystem writeback;
    writeback.Update(world, 1.0f / 60.0f);

    world.GetComponentForWrite<Transform>(e)->Position.X += 3.0;
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 1u);
    EXPECT_EQ(world.GetComponent<Transform>(e)->Position.X, pose.position.X) << "the edited Transform kept its edit";

    world.GetComponentForWrite<WorldTransform>(e)->Position.Y += 3.0;
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 1u);
    EXPECT_EQ(world.GetComponent<WorldTransform>(e)->Position.Y, pose.position.Y)
        << "the edited WorldTransform kept its edit";

    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 0u);
}

// Stands in for PhysicsStepSystem's snapshot: the pose the body had before the last step.
static void SetPreviousPhysicsPose(GameEngine::ECS::World& world,
                                   GameEngine::ECS::EntityHandle entity,
                                   const GameEngine::Physics::Transform& previous)
{
    auto* body = world.GetComponentForWrite<GameEngine::Components::PhysicsBody>(entity);
    body->prevPhysicsTransform = previous;
    body->hasPrevPhysicsTransform = true;
}

// With render interpolation the composed pose depends on alpha, so the skip holds only when the
// previous and the current backend poses both equal the pose last applied. A blend between two
// different poses is never recorded as settled: the next frame composes again and lands on the pose.
TEST(Physics, InterpolatedWritebackSkipsOnlyWhenBothPosesEqualTheLastApplied)
{
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::Transform;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    GameEngine::ECS::World world;
    const auto e = AddStillBody(world, pw);
    ASSERT_TRUE(pw.IsBodyValid(world.GetComponent<PhysicsBody>(e)->body));
    auto& clock = world.GetSimulationClock();
    clock.Advance(clock.GetFixedDeltaSeconds() * 0.5);
    ASSERT_FLOAT_EQ(clock.GetAlpha(), 0.5f);

    const GameEngine::Physics::Transform rest = pw.GetBodyTransform(world.GetComponent<PhysicsBody>(e)->body);
    GameEngine::Physics::Transform away = rest;
    away.position.X += 2.0;

    GameEngine::PhysicsECS::PhysicsWritebackSystem writeback;
    SetPreviousPhysicsPose(world, e, rest);
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 1u);
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 0u)
        << "previous and current both at rest: alpha cannot move it";

    SetPreviousPhysicsPose(world, e, away);
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 1u)
        << "the previous pose moved; the current pose alone is not the key";
    EXPECT_DOUBLE_EQ(world.GetComponent<Transform>(e)->GetPosition().X, rest.position.X + 1.0);

    SetPreviousPhysicsPose(world, e, rest);
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 1u)
        << "the last write was a blend; it must not count as settled";
    EXPECT_EQ(world.GetComponent<Transform>(e)->GetPosition().X, rest.position.X);
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 0u);
}

// A kinematic body follows its entity: the step moves the body to the WorldTransform and the writeback
// leaves the Transform where the game put it. A dynamic body keeps the body's pose.
TEST(Physics, KinematicBodyFollowsItsTransformAndADynamicBodyKeepsTheBodyPose)
{
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;
    using GameEngine::Physics::MotionType;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    GameEngine::ECS::World world;
    const auto kinematic = AddStillBody(world, pw, MotionType::Kinematic);
    const auto dynamic = AddStillBody(world, pw, MotionType::Dynamic);
    ASSERT_TRUE(pw.IsBodyValid(world.GetComponent<PhysicsBody>(kinematic)->body));
    ASSERT_TRUE(pw.IsBodyValid(world.GetComponent<PhysicsBody>(dynamic)->body));
    // The kinematic body moves through the start pose they share, so the dynamic body starts away from it.
    GameEngine::Physics::Transform dynamicStart = pw.GetBodyTransform(world.GetComponent<PhysicsBody>(dynamic)->body);
    dynamicStart.position.X -= 10.0;
    pw.SetBodyTransform(world.GetComponent<PhysicsBody>(dynamic)->body, dynamicStart);

    const GameEngine::Mathematics::Vector3 moved{4.0f, 2.0f, 7.0f};
    for (const auto e : {kinematic, dynamic})
    {
        auto* transform = world.GetComponentForWrite<Transform>(e);
        transform->Position = GameEngine::Mathematics::WorldPosition::FromVector3(moved);
        std::memcpy(world.GetComponentForWrite<WorldTransform>(e), transform,
                    GameEngine::Components::kTransformPoseBytes);
    }

    GameEngine::PhysicsECS::PhysicsStepSystem step;
    GameEngine::PhysicsECS::PhysicsWritebackSystem writeback;
    for (int frame = 0; frame < 3; ++frame)
    {
        step.Update(world, 1.0f / 60.0f);
        writeback.Update(world, 1.0f / 60.0f);
    }

    const GameEngine::Physics::Transform kinematicPose =
        pw.GetBodyTransform(world.GetComponent<PhysicsBody>(kinematic)->body);
    const GameEngine::Mathematics::Vector3 kinematicAt = kinematicPose.position - GameEngine::Mathematics::WorldPosition{};
    EXPECT_FLOAT_EQ(kinematicAt.x, moved.x) << "the kinematic body did not follow its Transform";
    EXPECT_FLOAT_EQ(kinematicAt.y, moved.y);
    EXPECT_FLOAT_EQ(kinematicAt.z, moved.z);
    const auto kinematicPosition = world.GetComponent<Transform>(kinematic)->GetPosition();
    EXPECT_DOUBLE_EQ(kinematicPosition.X, moved.x) << "the writeback snapped the kinematic Transform back";
    EXPECT_DOUBLE_EQ(kinematicPosition.Y, moved.y);
    EXPECT_DOUBLE_EQ(kinematicPosition.Z, moved.z);

    const GameEngine::Physics::Transform dynamicPose =
        pw.GetBodyTransform(world.GetComponent<PhysicsBody>(dynamic)->body);
    const auto dynamicPosition = world.GetComponent<Transform>(dynamic)->GetPosition();
    // The writeback blends the last two steps, so the Transform is near the body pose, not on it.
    constexpr GameEngine::float32 kInterpolationTolerance = 0.25f;
    EXPECT_NEAR(dynamicPosition.X, dynamicPose.position.X, kInterpolationTolerance)
        << "the dynamic Transform left its body";
    EXPECT_NEAR(dynamicPosition.Y, dynamicPose.position.Y, kInterpolationTolerance);
    EXPECT_NEAR(dynamicPosition.Z, dynamicPose.position.Z, kInterpolationTolerance);
    EXPECT_GT(std::abs(dynamicPose.position.Z - moved.z), 1.0) << "the dynamic body followed its Transform";
}

// A box body with its Transform and WorldTransform at `position`, for the kinematic tests below.
static GameEngine::ECS::EntityHandle AddBoxBody(GameEngine::ECS::World& world,
                                                GameEngine::Physics::PhysicsWorld& pw,
                                                GameEngine::Physics::MotionType motionType,
                                                const GameEngine::Physics::Vector3& halfExtents,
                                                const GameEngine::Physics::Vector3& position)
{
    using namespace GameEngine::Physics;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    BoxShapeDef boxDef{};
    boxDef.halfExtents = halfExtents;
    BodySettings bs{};
    bs.shape = pw.CreateShape(boxDef);
    bs.motionType = motionType;
    bs.layer = motionType == MotionType::Kinematic ? Layers::Kinematic : Layers::Dynamic;
    bs.mass = 1.0f;
    bs.position = WorldPosition::FromVector3(position);
    PhysicsBody pb{};
    pb.motionType = motionType;
    pb.body = pw.CreateBody(bs);
    pb.initialized = true;

    const auto e = world.CreateEntity();
    world.AddComponentImmediate<Transform>(
        e, Transform::FromTRS(GameEngine::Mathematics::WorldPosition::FromVector3(position), Quaternion::Identity(),
                              Vector3(1.0f, 1.0f, 1.0f)));
    WorldTransform wt{};
    std::memcpy(&wt, world.GetComponent<Transform>(e), GameEngine::Components::kTransformPoseBytes);
    world.AddComponentImmediate<WorldTransform>(e, wt);
    world.AddComponentImmediate<PhysicsBody>(e, pb);
    if (motionType == MotionType::Kinematic)
        world.AddComponentImmediate(e, GameEngine::Components::KinematicBody{});
    return e;
}

static void SetTransformHeight(GameEngine::ECS::World& world, GameEngine::ECS::EntityHandle e, GameEngine::float32 y)
{
    auto* transform = world.GetComponentForWrite<GameEngine::Components::Transform>(e);
    transform->Position.Y = y;
    std::memcpy(world.GetComponentForWrite<GameEngine::Components::WorldTransform>(e), transform,
                GameEngine::Components::kTransformPoseBytes);
}

// A kinematic platform that the game raises through its Transform moves with a velocity, so a dynamic box
// resting on it is carried at the platform's speed and stays on top. When the Transform stops, the platform
// stops on it.
TEST(Physics, KinematicPlatformMovedThroughItsTransformCarriesADynamicBoxAndStops)
{
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Physics::MotionType;
    using GameEngine::Physics::Vector3;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    GameEngine::ECS::World world;
    constexpr GameEngine::float32 kPlatformHalfHeight = 0.1f;
    constexpr GameEngine::float32 kBoxHalfSize = 0.25f;
    const auto platform =
        AddBoxBody(world, pw, MotionType::Kinematic, Vector3(1.0f, kPlatformHalfHeight, 1.0f), Vector3(0.0f, 0.0f, 0.0f));
    const auto box = AddBoxBody(world, pw, MotionType::Dynamic, Vector3(kBoxHalfSize, kBoxHalfSize, kBoxHalfSize),
                                Vector3(0.0f, kPlatformHalfHeight + kBoxHalfSize, 0.0f));
    const auto platformBody = world.GetComponent<PhysicsBody>(platform)->body;
    const auto boxBody = world.GetComponent<PhysicsBody>(box)->body;
    ASSERT_TRUE(pw.IsBodyValid(platformBody));
    ASSERT_TRUE(pw.IsBodyValid(boxBody));

    GameEngine::PhysicsECS::PhysicsStepSystem step;
    GameEngine::PhysicsECS::PhysicsWritebackSystem writeback;
    constexpr GameEngine::float32 kFrame = 1.0f / 60.0f;
    for (int frame = 0; frame < 60; ++frame)
    {
        step.Update(world, kFrame);
        writeback.Update(world, kFrame);
    }

    constexpr GameEngine::float32 kSpeed = 6.0f;
    GameEngine::float32 height = 0.0f;
    for (int frame = 0; frame < 60; ++frame)
    {
        height += kSpeed * kFrame;
        SetTransformHeight(world, platform, height);
        step.Update(world, kFrame);
        writeback.Update(world, kFrame);
    }

    const double platformTop = pw.GetBodyTransform(platformBody).position.Y + kPlatformHalfHeight;
    const double boxBottom = pw.GetBodyTransform(boxBody).position.Y - kBoxHalfSize;
    constexpr GameEngine::float32 kContactSlop = 0.02f;
    EXPECT_NEAR(boxBottom, platformTop, kContactSlop) << "the box left the top of the platform";
    EXPECT_NEAR(pw.GetLinearVelocity(boxBody).y, kSpeed, 0.1f * kSpeed) << "the platform did not carry the box";

    for (int frame = 0; frame < 30; ++frame)
    {
        step.Update(world, kFrame);
        writeback.Update(world, kFrame);
    }
    EXPECT_FLOAT_EQ(static_cast<GameEngine::float32>(pw.GetBodyTransform(platformBody).position.Y), height) << "the platform drifted off its Transform";
    EXPECT_EQ(pw.GetLinearVelocity(platformBody).y, 0.0f) << "the platform kept moving";
}

// Runs one frame the way SystemManager::Update does: the world's clock runs the step once per fixed step,
// then the writeback runs once.
static void RunPhysicsFrame(GameEngine::ECS::World& world,
                            GameEngine::PhysicsECS::PhysicsStepSystem& step,
                            GameEngine::PhysicsECS::PhysicsWritebackSystem& writeback,
                            GameEngine::float32 frameDelta)
{
    auto& clock = world.GetSimulationClock();
    clock.Advance(frameDelta);
    const auto fixedDelta = static_cast<GameEngine::float32>(clock.GetFixedDeltaSeconds());
    while (clock.BeginStep())
        step.Update(world, fixedDelta);
    writeback.Update(world, frameDelta);
}

struct FallResult
{
    GameEngine::float32 VelocityY = 0.0f;
    GameEngine::float32 Alpha = 0.0f;
    GameEngine::uint64 Steps = 0;
};

// Drops a box through frames of a SystemManager that holds the physics step, with the clock at the
// scene's float step as the physics bootstrap sets it.
static FallResult FallThroughFrames(const std::vector<GameEngine::float32>& frameDeltas)
{
    using GameEngine::Physics::MotionType;
    using GameEngine::Physics::Vector3;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    GameEngine::ECS::World world;
    auto& clock = world.GetSimulationClock();
    clock.SetFixedDeltaSeconds(PhysicsWorldService::GetSettings().fixedTimeStep);
    const auto e = AddBoxBody(world, pw, MotionType::Dynamic, Vector3(0.5f, 0.5f, 0.5f), Vector3(0.0f, 50.0f, 0.0f));
    GameEngine::ECS::SystemManager manager;
    manager.AddSystem<GameEngine::PhysicsECS::PhysicsStepSystem>();
    for (const GameEngine::float32 frameDelta : frameDeltas)
        manager.Update(world, frameDelta);
    const auto body = world.GetComponent<GameEngine::Components::PhysicsBody>(e)->body;
    return {pw.GetLinearVelocity(body).y, clock.GetAlpha(), clock.GetTick()};
}

// A frame of three fixed steps runs three steps, also when the float frame delta is a little short of three
// of the scene's float steps, and leaves no part of a step for the render blend.
TEST(Physics, FrameOfThreeFixedStepsRunsThreeSteps)
{
    const FallResult threeFrames = FallThroughFrames({1.0f / 60.0f, 1.0f / 60.0f, 1.0f / 60.0f});
    const FallResult oneFrame = FallThroughFrames({3.0f / 60.0f});
    EXPECT_EQ(oneFrame.Steps, 3u);
    EXPECT_EQ(oneFrame.Alpha, 0.0f);
    EXPECT_EQ(oneFrame.VelocityY, threeFrames.VelocityY) << "the frame did not run the same three steps";

    const FallResult halfFrame = FallThroughFrames({1.0f / 120.0f});
    EXPECT_EQ(halfFrame.Steps, 0u);
    EXPECT_FLOAT_EQ(halfFrame.Alpha, 0.5f);
    EXPECT_EQ(halfFrame.VelocityY, 0.0f);
}

// A scene whose Fixed Time Step is 0 gets an error in the log that names the fix, and the clock keeps
// its step.
TEST(Physics, BootstrapLogsAFixedTimeStepOfZero)
{
    using namespace GameEngine;
    using PhysicsECS::PhysicsWorldService;

    if (PhysicsWorldService::IsInitialized())
        PhysicsWorldService::Shutdown();
    ECS::World world;
    Components::PhysicsWorldSettingsComponent settings{};
    settings.settings.fixedTimeStep = 0.0f;
    world.AddComponentImmediate(world.CreateEntity(), settings);
    PhysicsECS::PhysicsWorldBootstrapSystem bootstrap;

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        bootstrap.Update(world, 0.0f);
        Logger::Log::Flush();
    }
    PhysicsWorldService::Shutdown();
    const bool refused = std::any_of(lines.begin(), lines.end(), [](const std::string& line)
                                     { return line.find("fixed delta 0 s refused; give a value above 0") != std::string::npos; });
    EXPECT_TRUE(refused);
    EXPECT_EQ(world.GetSimulationClock().GetFixedDeltaSeconds(), ECS::SimulationClock::kDefaultFixedDeltaSeconds);
}

// The Monitors series Time/Physics charts the cost of the physics steps of each frame, so a frame
// with no step reports 0.
TEST(Physics, FrameWithNoStepReportsNoStepCost)
{
    using namespace GameEngine;
    using Physics::MotionType;
    using Physics::Vector3;
    using PhysicsECS::PhysicsWorldService;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ECS::World world;
    AddBoxBody(world, PhysicsWorldService::Get(), MotionType::Dynamic, Vector3(0.5f, 0.5f, 0.5f),
               Vector3(0.0f, 50.0f, 0.0f));
    ECS::SystemManager manager;
    manager.AddSystem<PhysicsECS::PhysicsStepSystem>();

    manager.Update(world, 1.0f / 60.0f);
    ASSERT_EQ(world.GetSimulationClock().GetStepsThisFrame(), 1u);
    EXPECT_GT(PhysicsWorldService::TakeStepMs(), 0.0f);
    manager.Update(world, 1.0f / 120.0f);
    ASSERT_EQ(world.GetSimulationClock().GetStepsThisFrame(), 0u);
    EXPECT_EQ(PhysicsWorldService::TakeStepMs(), 0.0f);
}

namespace
{
// A Simulation system that records the clock's tick at each step.
class TickRecordingSystem : public GameEngine::ECS::ISystem
{
  public:
    static constexpr GameEngine::ECS::SystemCadence Cadence = GameEngine::ECS::SystemCadence::Simulation;
    explicit TickRecordingSystem(std::vector<GameEngine::uint64>* ticks) : m_Ticks(ticks) {}
    const char* GetName() const override { return "TickRecording"; }
    void Update(GameEngine::ECS::World& world, GameEngine::float32) override
    {
        m_Ticks->push_back(world.GetSimulationClock().GetTick());
    }

  private:
    std::vector<GameEngine::uint64>* m_Ticks;
};
} // namespace

// The physics bootstrap starts each play session of one world at tick 0 with no stored time, so the
// time the world spent between two sessions does not move the steps of the second session.
TEST(Physics, EveryPlaySessionOfOneWorldStepsTheSameTicks)
{
    using namespace GameEngine;
    using PhysicsECS::PhysicsWorldService;

    if (PhysicsWorldService::IsInitialized())
        PhysicsWorldService::Shutdown();
    ECS::World world;
    ECS::SystemManager manager;
    std::vector<uint64> ticks;
    ECS::SystemScheduleBuilder builder;
    builder.Add<PhysicsECS::PhysicsWorldBootstrapSystem>("PhysicsWorldBootstrap", ECS::SystemPhase::Extraction, 1, {});
    builder.Add<TickRecordingSystem>("TickRecording", ECS::SystemPhase::Extraction, 2, {"PhysicsWorldBootstrap"},
                                     &ticks);
    builder.BuildAndRegisterWithWaves(manager);
    const std::vector<float32> sessionFrames{0.01f, 0.02f, 0.01f, 0.03f, 0.0125f};

    for (const float32 frameDelta : sessionFrames)
        manager.Update(world, frameDelta);
    const std::vector<uint64> firstSession = ticks;
    PhysicsWorldService::Shutdown();

    // Between the sessions the world's frames go on and the clock counts.
    ECS::SimulationClock& clock = world.GetSimulationClock();
    for (int frame = 0; frame < 3; ++frame)
    {
        clock.Advance(0.007);
        while (clock.BeginStep())
        {
        }
    }

    ticks.clear();
    for (const float32 frameDelta : sessionFrames)
        manager.Update(world, frameDelta);
    PhysicsWorldService::Shutdown();
    ASSERT_FALSE(firstSession.empty());
    EXPECT_EQ(firstSession.front(), 1u);
    EXPECT_EQ(ticks, firstSession);
}

// A frame shorter than the fixed step runs no step, and the writeback still blends the last two steps.
// A kinematic Transform the game moved on that frame stays where the game put it.
TEST(Physics, KinematicTransformHoldsOnAFrameWithNoStep)
{
    using GameEngine::Components::Transform;
    using GameEngine::Physics::MotionType;
    using GameEngine::Physics::Vector3;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    GameEngine::ECS::World world;
    const auto e = AddBoxBody(world, pw, MotionType::Kinematic, Vector3(0.5f, 0.5f, 0.5f), Vector3(1.0f, 5.0f, -2.0f));
    ASSERT_TRUE(pw.IsBodyValid(world.GetComponent<GameEngine::Components::PhysicsBody>(e)->body));

    GameEngine::PhysicsECS::PhysicsStepSystem step;
    GameEngine::PhysicsECS::PhysicsWritebackSystem writeback;
    RunPhysicsFrame(world, step, writeback, 1.0f / 60.0f);
    SetTransformHeight(world, e, 9.0f);
    RunPhysicsFrame(world, step, writeback, 1.0f / 120.0f);
    ASSERT_EQ(world.GetSimulationClock().GetStepsThisFrame(), 0u);

    EXPECT_DOUBLE_EQ(world.GetComponent<Transform>(e)->GetPosition().Y, 9.0)
        << "the writeback moved the kinematic Transform";
}

// A kinematic body that stops is placed on its exact pose with no velocity. On the frames after that the
// step does not move it and the writeback does not stamp its chunk, so a resting body costs nothing.
TEST(Physics, KinematicBodyThatStopsIsPlacedOnItsPoseAndLeftAlone)
{
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;
    using GameEngine::Physics::MotionType;
    using GameEngine::Physics::Quaternion;
    using GameEngine::Physics::Vector3;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    GameEngine::ECS::World world;
    const Vector3 start(1.0f, 5.0f, -2.0f);
    const auto e = AddBoxBody(world, pw, MotionType::Kinematic, Vector3(0.5f, 0.5f, 0.5f), start);
    const auto body = world.GetComponent<PhysicsBody>(e)->body;
    ASSERT_TRUE(pw.IsBodyValid(body));

    GameEngine::PhysicsECS::PhysicsStepSystem step;
    GameEngine::PhysicsECS::PhysicsWritebackSystem writeback;
    constexpr GameEngine::float32 kFrame = 1.0f / 60.0f;
    const auto runFrame = [&]
    {
        step.Update(world, kFrame);
        writeback.Update(world, kFrame);
    };
    for (int frame = 1; frame <= 10; ++frame)
    {
        auto* transform = world.GetComponentForWrite<Transform>(e);
        *transform = Transform::FromTRS(
            GameEngine::Mathematics::WorldPosition::FromVector3(start + Vector3(0.1f * frame, 0.0f, 0.0f)),
            Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), 0.05f * frame), Vector3(1.0f, 1.0f, 1.0f));
        std::memcpy(world.GetComponentForWrite<WorldTransform>(e), transform, GameEngine::Components::kTransformPoseBytes);
        runFrame();
    }
    EXPECT_EQ(PhysicsWorldService::GetLastKinematicBodiesMoving(), 1u);
    runFrame();

    const Transform* transform = world.GetComponent<Transform>(e);
    const auto position = transform->GetPosition();
    const auto rotation = transform->GetRotation().GetGLM();
    const GameEngine::Physics::Transform pose = pw.GetBodyTransform(body);
    EXPECT_EQ(pose.position.X, position.X) << "the stopped body is not on its pose";
    EXPECT_EQ(pose.position.Y, position.Y);
    EXPECT_EQ(pose.position.Z, position.Z);
    EXPECT_EQ(pose.rotation.GetGLM().w, rotation.w) << "the stopped body is not on its rotation";
    EXPECT_EQ(pose.rotation.GetGLM().x, rotation.x);
    EXPECT_EQ(pose.rotation.GetGLM().y, rotation.y);
    EXPECT_EQ(pose.rotation.GetGLM().z, rotation.z);
    const Vector3 linear = pw.GetLinearVelocity(body);
    const Vector3 angular = pw.GetAngularVelocity(body);
    EXPECT_EQ(linear.x, 0.0f) << "the stopped body kept a velocity";
    EXPECT_EQ(linear.y, 0.0f);
    EXPECT_EQ(linear.z, 0.0f);
    EXPECT_EQ(angular.x, 0.0f) << "the stopped body kept a spin";
    EXPECT_EQ(angular.y, 0.0f);
    EXPECT_EQ(angular.z, 0.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastKinematicBodiesMoving(), 0u) << "the stopped body is still moving";

    GameEngine::ECS::ChangeGate gate{};
    gate.LastRunVersion = world.GetGlobalSystemVersion();
    runFrame();
    int stampedChunks = 0;
    auto stamped = world.Query<GameEngine::ECS::Read<WorldTransform>>();
    stamped.Changed<WorldTransform>(gate);
    stamped.Each([&](const WorldTransform&) { ++stampedChunks; });
    EXPECT_EQ(stampedChunks, 0) << "an idle frame stamped the resting kinematic body's WorldTransform";
    EXPECT_EQ(PhysicsWorldService::GetLastKinematicBodiesMoving(), 0u);
}

// The step keeps the kinematic bodies it moved on the last step frame. A new physics world reuses body
// handles, so that list is dropped when the world changes: a dynamic body in the new world that takes an
// old moving body's handle is not placed on the old body's target.
TEST(Physics, KinematicMovedListIsDroppedWhenThePhysicsWorldChanges)
{
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Physics::MotionType;
    using GameEngine::Physics::Vector3;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    GameEngine::PhysicsECS::PhysicsStepSystem step;
    constexpr GameEngine::float32 kFrame = 1.0f / 60.0f;
    const Vector3 halfExtents(0.5f, 0.5f, 0.5f);
    GameEngine::Physics::BodyHandle oldBody;
    {
        ScopedPhysicsWorldService svc(SmallDeterministicSettings());
        auto& pw = PhysicsWorldService::Get();
        GameEngine::ECS::World world;
        const auto e = AddBoxBody(world, pw, MotionType::Kinematic, halfExtents, Vector3(0.0f, 0.0f, 0.0f));
        oldBody = world.GetComponent<PhysicsBody>(e)->body;
        step.Update(world, kFrame);
        for (int frame = 1; frame <= 3; ++frame)
        {
            SetTransformHeight(world, e, 5.0f * static_cast<GameEngine::float32>(frame));
            step.Update(world, kFrame);
        }
        ASSERT_EQ(PhysicsWorldService::GetLastKinematicBodiesMoving(), 1u);
    }

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    GameEngine::ECS::World world;
    const Vector3 start(0.0f, 50.0f, 0.0f);
    const auto e = AddBoxBody(world, pw, MotionType::Dynamic, halfExtents, start);
    const auto body = world.GetComponent<PhysicsBody>(e)->body;
    ASSERT_EQ(body.Value(), oldBody.Value()) << "the new world did not reuse the handle, so the test proves nothing";
    step.Update(world, kFrame);
    step.Update(world, kFrame);

    EXPECT_NEAR(pw.GetBodyTransform(body).position.Y, start.y, 0.1f) << "the old world's target moved a body in the new world";
}

// The lines that warn a script velocity on a kinematic body was ignored.
static std::ptrdiff_t CountKinematicVelocityWarnings(const std::vector<std::string>& lines)
{
    return std::count_if(lines.begin(), lines.end(), [](const std::string& line)
                         { return line.find("has a kinematic body, which follows its Transform") != std::string::npos; });
}

// A kinematic body follows its Transform, so the script velocity setters leave it alone and warn once with
// the fix. The body stays on its Transform.
TEST(Physics, ScriptVelocityOnAKinematicBodyIsIgnoredWithOneWarning)
{
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Physics::MotionType;
    using GameEngine::Physics::Vector3;
    using GameEngine::PhysicsECS::PhysicsWorldService;
    namespace Script = GameEngine::PhysicsECS::Script;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    GameEngine::ECS::World world;
    const Vector3 start(0.0f, 2.0f, 0.0f);
    const auto e = AddBoxBody(world, pw, MotionType::Kinematic, Vector3(0.5f, 0.5f, 0.5f), start);
    const auto body = world.GetComponent<PhysicsBody>(e)->body;
    ASSERT_TRUE(pw.IsBodyValid(body));

    GameEngine::PhysicsECS::PhysicsStepSystem step;
    GameEngine::PhysicsECS::PhysicsWritebackSystem writeback;
    constexpr GameEngine::float32 kFrame = 1.0f / 60.0f;
    const auto runFrame = [&]
    {
        step.Update(world, kFrame);
        writeback.Update(world, kFrame);
    };
    for (int frame = 0; frame < 5; ++frame)
        runFrame();

    std::vector<std::string> lines;
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&lines);
        Script::SetLinearVelocity(world, e, Vector3(1.0f, 0.0f, 0.0f));
        Script::SetAngularVelocity(world, e, Vector3(0.0f, 2.0f, 0.0f));
        Script::SetLinearVelocity(world, e, Vector3(1.0f, 0.0f, 0.0f));
        Logger::Log::Flush();
    }
    for (int frame = 0; frame < 30; ++frame)
        runFrame();

    const GameEngine::Physics::Transform pose = pw.GetBodyTransform(body);
    EXPECT_EQ(pose.position.X, start.x) << "a script velocity moved the kinematic body off its Transform";
    EXPECT_EQ(pose.position.Y, start.y);
    EXPECT_EQ(pose.position.Z, start.z);
    EXPECT_EQ(pose.rotation.GetGLM().w, 1.0f) << "a script spin turned the kinematic body";
    EXPECT_EQ(CountKinematicVelocityWarnings(lines), 1) << "the warning is logged once per entity";
}

// The once-per-entity warning starts over in a new physics world, so a second play session warns again for an
// entity that reuses the same id.
TEST(Physics, ScriptVelocityWarningOnAKinematicBodyRepeatsInANewPhysicsWorld)
{
    using GameEngine::Physics::MotionType;
    using GameEngine::Physics::Vector3;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    std::uint32_t entityIds[2] = {};
    for (int session = 0; session < 2; ++session)
    {
        ScopedPhysicsWorldService svc(SmallDeterministicSettings());
        GameEngine::ECS::World world;
        const auto e = AddBoxBody(world, PhysicsWorldService::Get(), MotionType::Kinematic, Vector3(0.5f, 0.5f, 0.5f),
                                  Vector3(0.0f, 2.0f, 0.0f));
        entityIds[session] = e.id;
        std::vector<std::string> lines;
        {
            GameEngine::TestLog::ScopedEngineLogCapture capture(&lines);
            GameEngine::PhysicsECS::Script::SetLinearVelocity(world, e, Vector3(1.0f, 0.0f, 0.0f));
            GameEngine::PhysicsECS::Script::SetLinearVelocity(world, e, Vector3(1.0f, 0.0f, 0.0f));
            Logger::Log::Flush();
        }
        EXPECT_EQ(CountKinematicVelocityWarnings(lines), 1) << "physics world " << session;
    }
    ASSERT_EQ(entityIds[0], entityIds[1]) << "the entity ids differ, so the test proves nothing";
}

// ---- Heightfield collider version gate (terrain live-edit propagation) ----
// A mid-sim terrain edit bumps the provider's data version; PhysicsInitSystem
// must rebuild the collider from fresh samples. Regression test for the gate
// advancing lastBuiltVersion without a rebuild (samples gathered once,
// discarded, never retried — edits never reached a live collider).

namespace
{
std::vector<GameEngine::float32> s_HeightFieldTestSamples;
GameEngine::uint32 s_HeightFieldTestSampleCount = 0;
GameEngine::uint64 s_HeightFieldTestVersion = 0;

// Dirty region reported to consumers whose cursor is behind the current
// version (mirrors TerrainService's DirtyRegionLog-backed provider).
bool s_HeightFieldTestHasRegion = false;
GameEngine::int32 s_HeightFieldTestRegion[4] = {0, 0, 0, 0}; // minX, minZ, maxX, maxZ (exclusive)

GameEngine::PhysicsECS::HeightFieldData HeightFieldTestProvider(GameEngine::uint32 /*handle*/,
                                                                GameEngine::uint32 /*generation*/,
                                                                GameEngine::uint64 sinceVersion)
{
    GameEngine::PhysicsECS::HeightFieldData data;
    data.samples = s_HeightFieldTestSamples.data();
    data.sampleCount = s_HeightFieldTestSampleCount;
    data.version = s_HeightFieldTestVersion;
    if (s_HeightFieldTestHasRegion && sinceVersion < s_HeightFieldTestVersion)
    {
        data.hasRegion = true;
        data.regionMinX = s_HeightFieldTestRegion[0];
        data.regionMinZ = s_HeightFieldTestRegion[1];
        data.regionMaxX = s_HeightFieldTestRegion[2];
        data.regionMaxZ = s_HeightFieldTestRegion[3];
    }
    return data;
}

// Unregisters the provider even when an ASSERT bails out of the test body —
// a dangling provider would point at this file's statics from later tests.
struct ScopedHeightFieldProvider
{
    explicit ScopedHeightFieldProvider(GameEngine::PhysicsECS::HeightFieldDataProvider provider)
    {
        s_HeightFieldTestHasRegion = false;
        GameEngine::PhysicsECS::SetHeightFieldDataProvider(provider);
    }
    ~ScopedHeightFieldProvider()
    {
        GameEngine::PhysicsECS::SetHeightFieldDataProvider(nullptr);
    }
};

// Terrain entity with a heightfield collider sourced from the test provider.
GameEngine::ECS::EntityHandle CreateHeightFieldTerrainEntity(GameEngine::ECS::World& world,
                                                             GameEngine::float32 sizeX,
                                                             GameEngine::float32 sizeZ)
{
    using namespace GameEngine;
    auto e = world.CreateEntity();
    world.AddComponentImmediate<Components::Transform>(e, Components::Transform{});
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    Components::PhysicsCollider collider{};
    collider.layer = Physics::Layers::Static;
    world.AddComponentImmediate<Components::PhysicsCollider>(e, collider);
    Components::HeightFieldColliderShape hf{};
    hf.sizeX = sizeX;
    hf.sizeZ = sizeZ;
    hf.heightScale = 1.0f;
    world.AddComponentImmediate<Components::HeightFieldColliderShape>(e, hf);
    Components::PhysicsBody body{};
    body.motionType = Physics::MotionType::Static;
    world.AddComponentImmediate<Components::PhysicsBody>(e, body);
    return e;
}
} // namespace

TEST(Physics, HeightFieldColliderRebuildsOnProviderVersionBump)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::PhysicsCollider;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    constexpr uint32 kSampleCount = 8;
    constexpr float32 kInitialHeight = 10.0f;
    constexpr float32 kEditedHeight = 30.0f;
    constexpr float32 kDt = 1.0f / 60.0f;

    s_HeightFieldTestSamples.assign(kSampleCount * kSampleCount, kInitialHeight);
    s_HeightFieldTestSampleCount = kSampleCount;
    s_HeightFieldTestVersion = 1;
    ScopedHeightFieldProvider providerGuard(&HeightFieldTestProvider);
    ECS::World world;
    auto e = CreateHeightFieldTerrainEntity(world, 100.0f, 100.0f);

    PhysicsInitSystem init;
    // This test asserts version-gate semantics, not throttling: rebuild on
    // the first frame the stale version is observed.
    init.SetRebuildQuiescenceSeconds(0.0f);

    auto surfaceHeightAtOrigin = [&]() -> float32 {
        RayCastQuery q{};
        q.origin = {0.0, 100.0, 0.0};
        q.direction = Vector3(0.0f, -1.0f, 0.0f);
        q.maxDistance = 500.0f;
        RayCastResult hit{};
        if (!pw.RayCast(q, hit))
            return -1.0f;
        return static_cast<float>(hit.hitPoint.Y);
    };

    // Initial build.
    init.Update(world, kDt);
    pw.OptimizeBroadphase();
    const auto* pb = world.GetComponent<PhysicsBody>(e);
    ASSERT_NE(pb, nullptr);
    ASSERT_TRUE(pb->initialized);
    const BodyHandle initialBody = pb->body;
    EXPECT_NEAR(surfaceHeightAtOrigin(), kInitialHeight, 0.5f);

    // Steady state: step, then re-run init with an unchanged version.
    // The body must be left untouched (early-out, no rebuild).
    pw.Step(kDt, 1);
    init.Update(world, kDt);
    pb = world.GetComponent<PhysicsBody>(e);
    EXPECT_TRUE(pb->body == initialBody);

    // Mid-sim terrain edit: mutate provider samples, bump version.
    std::fill(s_HeightFieldTestSamples.begin(), s_HeightFieldTestSamples.end(), kEditedHeight);
    ++s_HeightFieldTestVersion;

    init.Update(world, kDt);
    pw.OptimizeBroadphase();
    pw.Step(kDt, 1);

    pb = world.GetComponent<PhysicsBody>(e);
    ASSERT_TRUE(pb->initialized);
    // The collider must observe a height that only exists post-edit.
    EXPECT_NEAR(surfaceHeightAtOrigin(), kEditedHeight, 0.5f);

    // The version gate must be quiescent again after consuming the edit.
    const BodyHandle rebuiltBody = pb->body;
    init.Update(world, kDt);
    pb = world.GetComponent<PhysicsBody>(e);
    EXPECT_TRUE(pb->body == rebuiltBody);
}

// A new heightfield body builds its shape on the job pool, not in the update that finds it: a
// 1025^2 tile collider is about 28 ms, too long for the frame thread at each terrain tile
// arrival (#3409). The body is created on a later update from that build. The pool's only
// worker is held busy until the first update returns, so the build cannot land inside it.
TEST(Physics, NewHeightFieldBodyBuildsItsShapeOnTheJobPool)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::PhysicsBody;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    constexpr uint32 kSampleCount = 8;
    constexpr float32 kHeight = 12.0f;
    s_HeightFieldTestSamples.assign(kSampleCount * kSampleCount, kHeight);
    s_HeightFieldTestSampleCount = kSampleCount;
    s_HeightFieldTestVersion = 1;
    ScopedHeightFieldProvider providerGuard(&HeightFieldTestProvider);

    // Declared before the pool: the job can still be inside set_value() after the wait returns.
    std::promise<void> busy;
    JobSystem::WorkStealingThreadPool pool(1);
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    pool.Submit([released, &busy]() {
        busy.set_value();
        released.wait();
    });
    busy.get_future().wait();

    ECS::World world;
    world.SetJobSystem(&pool);
    auto e = CreateHeightFieldTerrainEntity(world, 100.0f, 100.0f);
    PhysicsInitSystem init;
    init.Update(world, 1.0f / 60.0f);
    EXPECT_FALSE(world.GetComponent<PhysicsBody>(e)->initialized)
        << "the update that found the collider built its shape on the frame thread";

    release.set_value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!world.GetComponent<PhysicsBody>(e)->initialized && std::chrono::steady_clock::now() < deadline)
    {
        init.Update(world, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(world.GetComponent<PhysicsBody>(e)->initialized) << "the pool's build never became a body";

    pw.OptimizeBroadphase();
    RayCastQuery q{};
    q.origin = {0.0, 100.0, 0.0};
    q.direction = Vector3(0.0f, -1.0f, 0.0f);
    q.maxDistance = 500.0f;
    RayCastResult hit{};
    ASSERT_TRUE(pw.RayCast(q, hit));
    EXPECT_NEAR(hit.hitPoint.Y, kHeight, 0.5f);
}

namespace
{
// Two heightfield sources at the same version: source 1 is flat at 12 m, source 2 at 30 m.
GameEngine::PhysicsECS::HeightFieldData TwoSourceHeightFieldProvider(GameEngine::uint32 handle,
                                                                     GameEngine::uint32 /*generation*/,
                                                                     GameEngine::uint64 /*sinceVersion*/)
{
    static std::vector<GameEngine::float32> s_Low(8u * 8u, 12.0f);
    static std::vector<GameEngine::float32> s_High(8u * 8u, 30.0f);
    GameEngine::PhysicsECS::HeightFieldData data;
    data.samples = handle == 2u ? s_High.data() : s_Low.data();
    data.sampleCount = 8u;
    data.version = 1u;
    return data;
}
} // namespace

// A collider pointed at other heightfield data while its first shape still builds on the pool gets
// a body from the new data: the build started for the old data is dropped, even when both report
// the same version.
TEST(Physics, APendingHeightFieldBuildRestartsWhenItsColliderChangesData)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::PhysicsBody;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    ScopedHeightFieldProvider providerGuard(&TwoSourceHeightFieldProvider);

    // Declared before the pool: the job can still be inside set_value() after the wait returns.
    std::promise<void> busy;
    JobSystem::WorkStealingThreadPool pool(1);
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    pool.Submit([released, &busy]() {
        busy.set_value();
        released.wait();
    });
    busy.get_future().wait();

    ECS::World world;
    world.SetJobSystem(&pool);
    auto e = CreateHeightFieldTerrainEntity(world, 100.0f, 100.0f);
    world.GetComponentForWrite<HeightFieldColliderShape>(e)->dataHandle = 1u;
    PhysicsInitSystem init;
    init.Update(world, 1.0f / 60.0f);
    ASSERT_FALSE(world.GetComponent<PhysicsBody>(e)->initialized) << "the first build did not wait on the pool";

    world.GetComponentForWrite<HeightFieldColliderShape>(e)->dataHandle = 2u;
    init.Update(world, 1.0f / 60.0f);
    release.set_value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!world.GetComponent<PhysicsBody>(e)->initialized && std::chrono::steady_clock::now() < deadline)
    {
        init.Update(world, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(world.GetComponent<PhysicsBody>(e)->initialized) << "no build became a body";

    pw.OptimizeBroadphase();
    RayCastQuery q{};
    q.origin = {0.0, 100.0, 0.0};
    q.direction = Vector3(0.0f, -1.0f, 0.0f);
    q.maxDistance = 500.0f;
    RayCastResult hit{};
    ASSERT_TRUE(pw.RayCast(q, hit));
    EXPECT_NEAR(hit.hitPoint.Y, 30.0f, 0.5f) << "the body was built from the data the collider left";
}

// A world hands out a heightfield builder a job can call without a check, even a moved-from world,
// which has no backend: its builder fails every build (returns null) instead of being null itself.
TEST(Physics, AWorldWithoutABackendHandsOutABuilderThatFailsEveryBuild)
{
    using namespace GameEngine::Physics;
    PhysicsWorld source;
    const PhysicsWorld moved(std::move(source));
    const HeightFieldShapeBuilder builder = source.GetHeightFieldShapeBuilder(); // NOLINT(bugprone-use-after-move)
    ASSERT_NE(builder, nullptr) << "a moved-from world handed out a null builder";
    HeightFieldShapeDef def;
    def.SampleCount = 2;
    def.Samples = {0.0f, 0.0f, 0.0f, 0.0f};
    EXPECT_EQ(builder(def), nullptr);
    EXPECT_NE(moved.GetHeightFieldShapeBuilder(), nullptr);
}

// One engine frame for the body switch: the lifecycle windows swap (which is where
// GetDisabled<PhysicsBody> is built), then the init system runs.
static void RunPhysicsInitFrame(GameEngine::ECS::World& world, GameEngine::PhysicsECS::PhysicsInitSystem& init)
{
    world.SwapLifecycleEvents();
    init.Update(world, 1.0f / 60.0f);
}

// A body switched off — its own tag or its whole entity — gives its backend body back on the next
// frame (GetDisabled<PhysicsBody>) and keeps its authoring data; switched back on, it is built again
// like any new body.
TEST(Physics, SwitchedOffBodyReleasesItsBackendBodyAndIsRebuiltWhenBackOn)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::PhysicsCollider;
    using GameEngine::Components::SphereColliderShape;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    ECS::World world;
    RegisterPhysicsWorldHooks(world);
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, Transform{});
    world.AddComponentImmediate(e, WorldTransform{});
    PhysicsBody authored{};
    authored.mass = 3.0f;
    world.AddComponentImmediate(e, authored);
    world.AddComponentImmediate(e, PhysicsCollider{});
    world.AddComponentImmediate(e, SphereColliderShape{});

    PhysicsInitSystem init;
    RunPhysicsInitFrame(world, init);
    const Physics::BodyHandle built = world.GetComponent<PhysicsBody>(e)->body;
    ASSERT_TRUE(world.GetComponent<PhysicsBody>(e)->initialized);
    ASSERT_TRUE(pw.IsBodyValid(built));

    ECS::Entity(&world, e).SetEnabled<PhysicsBody>(false);
    RunPhysicsInitFrame(world, init);
    EXPECT_FALSE(world.GetComponent<PhysicsBody>(e)->initialized);
    EXPECT_FALSE(pw.IsBodyValid(built)) << "a switched-off body must leave the simulation";
    EXPECT_FLOAT_EQ(world.GetComponent<PhysicsBody>(e)->mass, 3.0f) << "switching off keeps the data";

    ECS::Entity(&world, e).SetEnabled<PhysicsBody>(true);
    RunPhysicsInitFrame(world, init);
    ASSERT_TRUE(world.GetComponent<PhysicsBody>(e)->initialized);
    const Physics::BodyHandle rebuilt = world.GetComponent<PhysicsBody>(e)->body;
    EXPECT_TRUE(pw.IsBodyValid(rebuilt));

    world.SetEntityEnabledImmediate(e, false);
    RunPhysicsInitFrame(world, init);
    EXPECT_FALSE(world.GetComponent<PhysicsBody>(e)->initialized);
    EXPECT_FALSE(pw.IsBodyValid(rebuilt)) << "an inactive entity's body must leave the simulation";
}

// PhysicsInit tags a body it builds as kinematic with KinematicBody, and takes the tag off when it rebuilds
// the body with another motion type.
TEST(Physics, PhysicsInitTagsAKinematicBodyAndUntagsItWhenItsMotionTypeChanges)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::KinematicBody;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::PhysicsCollider;
    using GameEngine::Components::SphereColliderShape;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ECS::World world;
    RegisterPhysicsWorldHooks(world);
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, Transform{});
    world.AddComponentImmediate(e, WorldTransform{});
    PhysicsBody authored{};
    authored.motionType = Physics::MotionType::Kinematic;
    world.AddComponentImmediate(e, authored);
    world.AddComponentImmediate(e, PhysicsCollider{});
    world.AddComponentImmediate(e, SphereColliderShape{});

    PhysicsInitSystem init;
    RunPhysicsInitFrame(world, init);
    world.ProcessCommands();
    ASSERT_TRUE(world.GetComponent<PhysicsBody>(e)->initialized);
    EXPECT_TRUE(world.HasComponent<KinematicBody>(e)) << "a kinematic body was built without the tag";

    auto* body = world.GetComponentForWrite<PhysicsBody>(e);
    body->motionType = Physics::MotionType::Dynamic;
    body->initialized = false;
    RunPhysicsInitFrame(world, init);
    world.ProcessCommands();
    ASSERT_TRUE(world.GetComponent<PhysicsBody>(e)->initialized);
    EXPECT_FALSE(world.HasComponent<KinematicBody>(e)) << "a dynamic body kept the kinematic tag";
}

// An inspector or debug-server edit asks for a rebuild by clearing initialized and leaves the old
// body alive until the rebuild destroys it. Switched off in that window, the body is still given
// back: the release keys on the backend objects the component holds, not on initialized.
TEST(Physics, BodySwitchedOffWhileAwaitingARebuildIsReleased)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::PhysicsCollider;
    using GameEngine::Components::SphereColliderShape;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    ECS::World world;
    RegisterPhysicsWorldHooks(world);
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, Transform{});
    world.AddComponentImmediate(e, WorldTransform{});
    world.AddComponentImmediate(e, PhysicsBody{});
    world.AddComponentImmediate(e, PhysicsCollider{});
    world.AddComponentImmediate(e, SphereColliderShape{});

    PhysicsInitSystem init;
    RunPhysicsInitFrame(world, init);
    const Physics::BodyHandle built = world.GetComponent<PhysicsBody>(e)->body;
    ASSERT_TRUE(pw.IsBodyValid(built));

    world.GetComponentForWrite<PhysicsBody>(e)->initialized = false;
    ECS::Entity(&world, e).SetEnabled<PhysicsBody>(false);
    RunPhysicsInitFrame(world, init);
    EXPECT_FALSE(pw.IsBodyValid(built)) << "the body awaiting its rebuild must leave the simulation";
    EXPECT_FALSE(world.GetComponent<PhysicsBody>(e)->body.IsValid());
}

// Switched off while the init system was paused, the transition's window is gone; the swap
// generation gap makes the next run sweep every body that is off.
TEST(Physics, BodySwitchedOffDuringAPauseIsReleasedOnResume)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::PhysicsCollider;
    using GameEngine::Components::SphereColliderShape;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    ECS::World world;
    RegisterPhysicsWorldHooks(world);
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, Transform{});
    world.AddComponentImmediate(e, WorldTransform{});
    world.AddComponentImmediate(e, PhysicsBody{});
    world.AddComponentImmediate(e, PhysicsCollider{});
    world.AddComponentImmediate(e, SphereColliderShape{});

    PhysicsInitSystem init;
    RunPhysicsInitFrame(world, init);
    const Physics::BodyHandle built = world.GetComponent<PhysicsBody>(e)->body;
    ASSERT_TRUE(pw.IsBodyValid(built));

    ECS::Entity(&world, e).SetEnabled<PhysicsBody>(false);
    world.SwapLifecycleEvents();
    world.SwapLifecycleEvents();
    ASSERT_TRUE(world.GetDisabled<PhysicsBody>().empty()) << "the transition's window is gone";

    init.Update(world, 1.0f / 60.0f);
    EXPECT_FALSE(pw.IsBodyValid(built));
}

// The x offset of the 300 km tests.
static constexpr double kFarMeters = 300032.0;

// A body 300 km out is built at its entity's exact world position, so a ray cast there hits its collider.
TEST(Physics, BodyFarFromTheOriginIsBuiltAtItsExactWorldPosition)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::BoxColliderShape;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::PhysicsCollider;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    ECS::World world;
    RegisterPhysicsWorldHooks(world);
    const ECS::EntityHandle e = world.CreateEntity();
    const Transform pose = Transform::FromTRS(Mathematics::WorldPosition{kFarMeters + 1.25, 0.0, -1026.0},
                                              Mathematics::Quaternion::Identity(),
                                              Mathematics::Vector3(1.0f, 1.0f, 1.0f));
    WorldTransform poseWorld{};
    poseWorld.Position = pose.Position;
    poseWorld.Basis = pose.Basis;
    world.AddComponentImmediate(e, pose);
    world.AddComponentImmediate(e, poseWorld);
    PhysicsBody authored{};
    authored.motionType = Physics::MotionType::Static;
    world.AddComponentImmediate(e, authored);
    world.AddComponentImmediate(e, PhysicsCollider{});
    world.AddComponentImmediate(e, BoxColliderShape{});

    PhysicsInitSystem init;
    RunPhysicsInitFrame(world, init);
    const PhysicsBody* built = world.GetComponent<PhysicsBody>(e);
    ASSERT_TRUE(built->initialized);

    // Straight down onto the top face of the half-meter box, at y = 0.5.
    Physics::RayCastQuery query{};
    query.origin = Physics::WorldPosition{kFarMeters + 1.25, 10.0, -1026.0};
    query.direction = Physics::Vector3(0.0f, -1.0f, 0.0f);
    query.maxDistance = 20.0f;
    Physics::RayCastResult hit{};
    ASSERT_TRUE(pw.RayCast(query, hit)) << "the ray at the entity's world position missed its body";
    EXPECT_EQ(hit.body.Value(), built->body.Value());
    EXPECT_EQ(hit.hitPoint.X, query.origin.X);
    EXPECT_NEAR(hit.hitPoint.Y, 0.5, 1.0e-4);
    EXPECT_EQ(hit.hitPoint.Z, query.origin.Z);
}

// The writeback writes the body position in double: 300 km out, a tenth of a millimeter is kept.
TEST(Physics, WritebackOfABodyFarFromTheOriginWritesTheExactPosition)
{
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::Transform;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    GameEngine::ECS::World world;
    const auto e = AddStillBody(world, pw);

    constexpr double kFarX = kFarMeters + 100.0001;
    const GameEngine::Physics::BodyHandle body = world.GetComponent<PhysicsBody>(e)->body;
    GameEngine::Physics::Transform moved = pw.GetBodyTransform(body);
    moved.position.X = kFarX;
    pw.SetBodyTransform(body, moved);
    GameEngine::PhysicsECS::PhysicsWritebackSystem writeback;
    writeback.Update(world, 1.0f / 60.0f);

    EXPECT_EQ(world.GetComponent<Transform>(e)->Position.X, kFarX);
}

// A kinematic body follows the double position of its Transform: 300 km out, a tenth of a millimeter is kept.
TEST(Physics, KinematicBodyFarFromTheOriginFollowsTheExactPosition)
{
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    GameEngine::ECS::World world;
    const auto e = AddStillBody(world, pw, GameEngine::Physics::MotionType::Kinematic);
    const GameEngine::Mathematics::WorldPosition target{kFarMeters + 4.0001, 2.0, 7.0};
    world.GetComponentForWrite<Transform>(e)->Position = target;
    world.GetComponentForWrite<WorldTransform>(e)->Position = target;

    GameEngine::PhysicsECS::PhysicsStepSystem step;
    for (int frame = 0; frame < 3; ++frame)
        step.Update(world, 1.0f / 60.0f);

    EXPECT_EQ(pw.GetBodyTransform(world.GetComponent<PhysicsBody>(e)->body).position.X, target.X);
}

// Frames the init-system tests run before they look at a body: enough for every body to build.
static constexpr int kInitSettleFrames = 3;
// Frames an init-system allocation test measures once every body has settled.
static constexpr int kInitMeasuredFrames = 20;

// True where an AllocationCountScope sees Engine's allocations. PhysicsInitSystem runs in
// Engine, and on Windows each image has its own operator new: a Release Engine.dll carries
// no allocation hook, so a zero count there would hold whatever the system does.
#if defined(_WIN32) && !GE_DEBUG_INSTRUMENTATION
static constexpr bool kEngineAllocationsCounted = false;
#else
static constexpr bool kEngineAllocationsCounted = true;
#endif

// A body with a collider and no shape yet; the caller adds the shape.
static GameEngine::ECS::EntityHandle CreateBodyWithCollider(GameEngine::ECS::World& world,
                                                            GameEngine::Physics::MotionType motionType)
{
    const GameEngine::ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, GameEngine::Components::Transform{});
    world.AddComponentImmediate(e, GameEngine::Components::WorldTransform{});
    GameEngine::Components::PhysicsBody body{};
    body.motionType = motionType;
    world.AddComponentImmediate(e, body);
    world.AddComponentImmediate(e, GameEngine::Components::PhysicsCollider{});
    return e;
}

// Allocations the init system makes over kInitMeasuredFrames frames on this thread.
static std::uint64_t CountInitFrameAllocations(GameEngine::ECS::World& world,
                                               GameEngine::PhysicsECS::PhysicsInitSystem& init)
{
    const GameEngine::Memory::AllocationCountScope probe(GameEngine::Memory::CountWindow::ThisThread);
    for (int frame = 0; frame < kInitMeasuredFrames; ++frame)
        init.Update(world, 1.0f / 60.0f);
    return probe.Count();
}

// One body per collider gather: a body that already exists gives the init system nothing to
// build, so its frame allocates nothing; a body added later is still built on its first frame.
TEST(Physics, InitSystemSteadyStateAllocatesNothingAndStillBuildsNewBodies)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::BoxColliderShape;
    using GameEngine::Components::CapsuleColliderShape;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::PlaneColliderShape;
    using GameEngine::Components::SphereColliderShape;

    if (!kEngineAllocationsCounted)
        GTEST_SKIP() << "Engine.dll counts its allocations only in Debug and DebugFast";

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedHeightFieldProvider noTerrain(nullptr);
    auto& pw = PhysicsWorldService::Get();

    ECS::World world;
    RegisterPhysicsWorldHooks(world);
    std::vector<ECS::EntityHandle> bodies;
    bodies.push_back(CreateBodyWithCollider(world, Physics::MotionType::Dynamic));
    world.AddComponentImmediate(bodies.back(), SphereColliderShape{});
    bodies.push_back(CreateBodyWithCollider(world, Physics::MotionType::Dynamic));
    world.AddComponentImmediate(bodies.back(), BoxColliderShape{});
    bodies.push_back(CreateBodyWithCollider(world, Physics::MotionType::Dynamic));
    world.AddComponentImmediate(bodies.back(), CapsuleColliderShape{});
    bodies.push_back(CreateBodyWithCollider(world, Physics::MotionType::Static));
    world.AddComponentImmediate(bodies.back(), PlaneColliderShape{});

    PhysicsInitSystem init;
    for (int frame = 0; frame < kInitSettleFrames; ++frame)
        RunPhysicsInitFrame(world, init);
    for (const ECS::EntityHandle e : bodies)
        ASSERT_TRUE(world.GetComponent<PhysicsBody>(e)->initialized);

    const std::uint64_t allocations = CountInitFrameAllocations(world, init);
    std::printf("[Alloc] PhysicsInitSystem steady state, %zu bodies, %d frames: %llu allocations\n",
                bodies.size(), kInitMeasuredFrames, static_cast<unsigned long long>(allocations));
    EXPECT_EQ(allocations, 0u) << "every body is built, so a frame has nothing to gather";

    const ECS::EntityHandle added = CreateBodyWithCollider(world, Physics::MotionType::Dynamic);
    world.AddComponentImmediate(added, SphereColliderShape{});
    std::uint64_t buildAllocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::ThisThread);
        RunPhysicsInitFrame(world, init);
        buildAllocations = probe.Count();
    }
    EXPECT_GT(buildAllocations, 0u) << "the probe must see the frame that builds a body";
    EXPECT_TRUE(world.GetComponent<PhysicsBody>(added)->initialized);
    EXPECT_TRUE(pw.IsBodyValid(world.GetComponent<PhysicsBody>(added)->body));
}

// A body that still says it is built but whose backend body is gone is built again on the
// next frame, even when every other body is built and the frame would otherwise do nothing.
TEST(Physics, InitSystemRebuildsABuiltBodyWhoseBackendBodyIsGone)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::SphereColliderShape;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedHeightFieldProvider noTerrain(nullptr);
    auto& pw = PhysicsWorldService::Get();

    ECS::World world;
    RegisterPhysicsWorldHooks(world);
    const ECS::EntityHandle a = CreateBodyWithCollider(world, Physics::MotionType::Dynamic);
    world.AddComponentImmediate(a, SphereColliderShape{});
    const ECS::EntityHandle b = CreateBodyWithCollider(world, Physics::MotionType::Dynamic);
    world.AddComponentImmediate(b, SphereColliderShape{});

    PhysicsInitSystem init;
    for (int frame = 0; frame < kInitSettleFrames; ++frame)
        RunPhysicsInitFrame(world, init);
    const Physics::BodyHandle dead = world.GetComponent<PhysicsBody>(a)->body;
    ASSERT_TRUE(pw.IsBodyValid(dead));
    pw.DestroyBody(dead);
    ASSERT_TRUE(world.GetComponent<PhysicsBody>(a)->initialized);

    RunPhysicsInitFrame(world, init);
    EXPECT_TRUE(pw.IsBodyValid(world.GetComponent<PhysicsBody>(a)->body));
    EXPECT_EQ(pw.GetBodyCount(), 2u);
}

// A body with no collider is not work again until a collider is added: the frames beside it allocate nothing, and the frame after the collider
// arrives builds it.
TEST(Physics, InitSystemSkipsABodyWithNoColliderUntilOneIsAdded)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::SphereColliderShape;

    if (!kEngineAllocationsCounted)
        GTEST_SKIP() << "Engine.dll counts its allocations only in Debug and DebugFast";

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedHeightFieldProvider noTerrain(nullptr);
    auto& pw = PhysicsWorldService::Get();

    ECS::World world;
    RegisterPhysicsWorldHooks(world);
    for (int i = 0; i < 2; ++i)
        world.AddComponentImmediate(CreateBodyWithCollider(world, Physics::MotionType::Dynamic),
                                    SphereColliderShape{});
    const ECS::EntityHandle bare = world.CreateEntity();
    world.AddComponentImmediate(bare, GameEngine::Components::Transform{});
    world.AddComponentImmediate(bare, GameEngine::Components::WorldTransform{});
    world.AddComponentImmediate(bare, PhysicsBody{});

    PhysicsInitSystem init;
    for (int frame = 0; frame < kInitSettleFrames; ++frame)
        RunPhysicsInitFrame(world, init);
    ASSERT_EQ(pw.GetBodyCount(), 2u);
    ASSERT_FALSE(world.GetComponent<PhysicsBody>(bare)->initialized);

    const std::uint64_t allocations = CountInitFrameAllocations(world, init);
    std::printf("[Alloc] PhysicsInitSystem beside a body with no collider, %d frames: %llu allocations\n",
                kInitMeasuredFrames, static_cast<unsigned long long>(allocations));
    EXPECT_EQ(allocations, 0u) << "a body with no collider gives the frame nothing to build";

    world.AddComponentImmediate(bare, GameEngine::Components::PhysicsCollider{});
    world.AddComponentImmediate(bare, SphereColliderShape{});
    std::uint64_t buildAllocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::ThisThread);
        RunPhysicsInitFrame(world, init);
        buildAllocations = probe.Count();
    }
    EXPECT_GT(buildAllocations, 0u) << "the probe must see the frame that builds a body";
    EXPECT_TRUE(world.GetComponent<PhysicsBody>(bare)->initialized);
    EXPECT_TRUE(pw.IsBodyValid(world.GetComponent<PhysicsBody>(bare)->body));
}

// A body whose shape the backend refuses (a sphere of radius 0) is built on the frame after
// the shape is edited to a valid one, with no structural change to wake it.
TEST(Physics, InitSystemBuildsABodyOnTheFrameAfterItsRefusedShapeIsFixed)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::SphereColliderShape;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedHeightFieldProvider noTerrain(nullptr);
    auto& pw = PhysicsWorldService::Get();

    ECS::World world;
    RegisterPhysicsWorldHooks(world);
    world.AddComponentImmediate(CreateBodyWithCollider(world, Physics::MotionType::Dynamic),
                                SphereColliderShape{});
    const ECS::EntityHandle refused = CreateBodyWithCollider(world, Physics::MotionType::Dynamic);
    world.AddComponentImmediate(refused, SphereColliderShape{0.0f});

    PhysicsInitSystem init;
    for (int frame = 0; frame < kInitSettleFrames; ++frame)
        RunPhysicsInitFrame(world, init);
    ASSERT_EQ(pw.GetBodyCount(), 1u);
    ASSERT_FALSE(world.GetComponent<PhysicsBody>(refused)->initialized);

    world.GetComponentForWrite<SphereColliderShape>(refused)->radius = 0.5f;
    RunPhysicsInitFrame(world, init);
    EXPECT_TRUE(world.GetComponent<PhysicsBody>(refused)->initialized);
    EXPECT_TRUE(pw.IsBodyValid(world.GetComponent<PhysicsBody>(refused)->body));
}

// Writes a uniform scale into the body's world matrix, as the transform system would.
static void SetUniformWorldScale(GameEngine::ECS::World& world, GameEngine::ECS::EntityHandle e, GameEngine::float32 scale)
{
    GameEngine::Components::WorldTransform* wt = world.GetComponentForWrite<GameEngine::Components::WorldTransform>(e);
    wt->Basis[0][0] = scale;
    wt->Basis[1][1] = scale;
    wt->Basis[2][2] = scale;
}

// A body the backend refuses for its scale (0) is built on the frame after the scale is fixed
// and a rebuild is asked for through PhysicsBody, as the editor's transform and inspector edits
// do, with no structural change to wake it.
TEST(Physics, InitSystemBuildsABodyOnTheFrameAfterItsRefusedScaleIsFixed)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::SphereColliderShape;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedHeightFieldProvider noTerrain(nullptr);
    auto& pw = PhysicsWorldService::Get();

    ECS::World world;
    RegisterPhysicsWorldHooks(world);
    world.AddComponentImmediate(CreateBodyWithCollider(world, Physics::MotionType::Dynamic),
                                SphereColliderShape{});
    const ECS::EntityHandle flat = CreateBodyWithCollider(world, Physics::MotionType::Dynamic);
    world.AddComponentImmediate(flat, SphereColliderShape{});
    SetUniformWorldScale(world, flat, 0.0f);

    PhysicsInitSystem init;
    for (int frame = 0; frame < kInitSettleFrames; ++frame)
        RunPhysicsInitFrame(world, init);
    ASSERT_EQ(pw.GetBodyCount(), 1u);
    ASSERT_FALSE(world.GetComponent<PhysicsBody>(flat)->initialized);

    SetUniformWorldScale(world, flat, 1.0f);
    world.GetComponentForWrite<PhysicsBody>(flat)->initialized = false;
    RunPhysicsInitFrame(world, init);
    EXPECT_TRUE(world.GetComponent<PhysicsBody>(flat)->initialized);
    EXPECT_TRUE(pw.IsBodyValid(world.GetComponent<PhysicsBody>(flat)->body));
}

// A body with no collider is built on the frame after an existing collider's owner is pointed
// at it, with no structural change to wake it.
TEST(Physics, InitSystemBuildsABodyOnTheFrameAfterAColliderIsRetargetedToIt)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::PhysicsColliderOwner;
    using GameEngine::Components::SphereColliderShape;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedHeightFieldProvider noTerrain(nullptr);
    auto& pw = PhysicsWorldService::Get();

    ECS::World world;
    RegisterPhysicsWorldHooks(world);
    const ECS::EntityHandle bare = world.CreateEntity();
    world.AddComponentImmediate(bare, GameEngine::Components::Transform{});
    world.AddComponentImmediate(bare, GameEngine::Components::WorldTransform{});
    world.AddComponentImmediate(bare, PhysicsBody{});
    // Its owner names no body, so it belongs to its own entity, which has no PhysicsBody.
    const ECS::EntityHandle collider = world.CreateEntity();
    world.AddComponentImmediate(collider, GameEngine::Components::Transform{});
    world.AddComponentImmediate(collider, GameEngine::Components::PhysicsCollider{});
    world.AddComponentImmediate(collider, SphereColliderShape{});
    world.AddComponentImmediate(collider, PhysicsColliderOwner{});

    PhysicsInitSystem init;
    for (int frame = 0; frame < kInitSettleFrames; ++frame)
        RunPhysicsInitFrame(world, init);
    ASSERT_EQ(pw.GetBodyCount(), 0u);
    ASSERT_FALSE(world.GetComponent<PhysicsBody>(bare)->initialized);

    world.GetComponentForWrite<PhysicsColliderOwner>(collider)->body = bare;
    RunPhysicsInitFrame(world, init);
    EXPECT_TRUE(world.GetComponent<PhysicsBody>(bare)->initialized);
    EXPECT_TRUE(pw.IsBodyValid(world.GetComponent<PhysicsBody>(bare)->body));
}

// A terrain edit made while the collider is disabled reaches the body when the collider is enabled
// again. The gather skips a disabled collider, so its lastBuiltVersion keeps the version before the edit.
TEST(Physics, HeightFieldColliderDisabledDuringEditRebuildsOnReenable)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::PhysicsCollider;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    constexpr uint32 kSampleCount = 8;
    constexpr float32 kInitialHeight = 10.0f;
    constexpr float32 kEditedHeight = 30.0f;
    constexpr float32 kDt = 1.0f / 60.0f;

    s_HeightFieldTestSamples.assign(kSampleCount * kSampleCount, kInitialHeight);
    s_HeightFieldTestSampleCount = kSampleCount;
    s_HeightFieldTestVersion = 1;
    ScopedHeightFieldProvider providerGuard(&HeightFieldTestProvider);
    ECS::World world;
    auto e = CreateHeightFieldTerrainEntity(world, 100.0f, 100.0f);

    PhysicsInitSystem init;
    // Version-gate semantics under test, not throttling.
    init.SetRebuildQuiescenceSeconds(0.0f);

    auto surfaceHeightAtOrigin = [&]() -> float32 {
        RayCastQuery q{};
        q.origin = {0.0, 100.0, 0.0};
        q.direction = Vector3(0.0f, -1.0f, 0.0f);
        q.maxDistance = 500.0f;
        RayCastResult hit{};
        if (!pw.RayCast(q, hit))
            return -1.0f;
        return static_cast<float>(hit.hitPoint.Y);
    };

    // Initial build.
    init.Update(world, kDt);
    pw.OptimizeBroadphase();
    EXPECT_NEAR(surfaceHeightAtOrigin(), kInitialHeight, 0.5f);

    // Disable the collider, then edit the terrain while it is off.
    GameEngine::ECS::Entity(&world, e).SetEnabled<PhysicsCollider>(false);
    std::fill(s_HeightFieldTestSamples.begin(), s_HeightFieldTestSamples.end(), kEditedHeight);
    ++s_HeightFieldTestVersion;
    init.Update(world, kDt);
    pw.Step(kDt, 1);

    // Re-enable: the version gate must still see the edit and rebuild.
    GameEngine::ECS::Entity(&world, e).SetEnabled<PhysicsCollider>(true);
    init.Update(world, kDt);
    pw.OptimizeBroadphase();
    pw.Step(kDt, 1);

    const auto* pb = world.GetComponent<PhysicsBody>(e);
    ASSERT_NE(pb, nullptr);
    ASSERT_TRUE(pb->initialized);
    EXPECT_NEAR(surfaceHeightAtOrigin(), kEditedHeight, 0.5f);
}

// ---- Terrain E1: in-place heightfield region updates (design doc §7) ------

namespace
{
// World-space X/Z of a sample index for a heightfield of kTerrainSize
// centered on the origin (same Offset/Scale mapping PhysicsInitSystem uses).
GameEngine::float32 SampleToWorld(GameEngine::uint32 sampleIndex,
                                  GameEngine::uint32 sampleCount,
                                  GameEngine::float32 size)
{
    return -size * 0.5f + static_cast<GameEngine::float32>(sampleIndex) * (size / static_cast<GameEngine::float32>(sampleCount - 1));
}
} // namespace

// The review-blocker test: raising terrain in place must refresh the body's
// broadphase entry. SetHeights only rebuilds the shape's local bounds; the
// body AABB refreshes via BodyInterface::NotifyShapeChanged. Without that
// call this test FAILS: the horizontal ray below never enters the body's
// stale (old-max-height) AABB, so broadphase culls the terrain and the
// raised region is invisible to collision until a full rebuild.
TEST(Physics, HeightFieldBroadphaseRefreshAfterInPlaceRaise)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::PhysicsBody;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    constexpr uint32 kN = 64;
    constexpr float32 kSize = 100.0f;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kRaisedHeight = 45.0f; // old max 40 + 5, within the 25% (7.5) encode headroom

    // Gradient along X in [10, 40] — a real span so shape creation pads a
    // usable encode range for in-place raises.
    s_HeightFieldTestSamples.resize(kN * kN);
    for (uint32 z = 0; z < kN; ++z)
        for (uint32 x = 0; x < kN; ++x)
            s_HeightFieldTestSamples[z * kN + x] = 10.0f + 30.0f * static_cast<float32>(x) / static_cast<float32>(kN - 1);
    s_HeightFieldTestSampleCount = kN;
    s_HeightFieldTestVersion = 1;
    ScopedHeightFieldProvider providerGuard(&HeightFieldTestProvider);

    ECS::World world;
    auto e = CreateHeightFieldTerrainEntity(world, kSize, kSize);

    PhysicsInitSystem init;
    init.Update(world, kDt);
    pw.OptimizeBroadphase();

    const auto* pb = world.GetComponent<PhysicsBody>(e);
    ASSERT_NE(pb, nullptr);
    ASSERT_TRUE(pb->initialized);
    const BodyHandle initialBody = pb->body;

    // Raise a 16x16 block (6.25% of samples → tier 1) well above the
    // original max height.
    constexpr int32 kR0 = 40;
    constexpr int32 kR1 = 56; // exclusive
    for (int32 z = kR0; z < kR1; ++z)
        for (int32 x = kR0; x < kR1; ++x)
            s_HeightFieldTestSamples[static_cast<size_t>(z) * kN + x] = kRaisedHeight;
    ++s_HeightFieldTestVersion;
    s_HeightFieldTestHasRegion = true;
    s_HeightFieldTestRegion[0] = kR0;
    s_HeightFieldTestRegion[1] = kR0;
    s_HeightFieldTestRegion[2] = kR1;
    s_HeightFieldTestRegion[3] = kR1;

    init.Update(world, kDt);

    // In-place: same body, version consumed, no rebuild.
    pb = world.GetComponent<PhysicsBody>(e);
    EXPECT_TRUE(pb->body == initialBody);
    EXPECT_EQ(world.GetComponent<HeightFieldColliderShape>(e)->lastBuiltVersion, s_HeightFieldTestVersion);

    // Horizontal ray ABOVE the old terrain max (40) aimed through the raised
    // block — only hits if the body's broadphase AABB grew to the new height.
    const float32 wx = SampleToWorld((kR0 + kR1) / 2, kN, kSize);
    RayCastQuery horizontal{};
    horizontal.origin = {wx, 43.0f, -60.0f};
    horizontal.direction = Vector3(0.0f, 0.0f, 1.0f);
    horizontal.maxDistance = 200.0f;
    RayCastResult hit{};
    ASSERT_TRUE(pw.RayCast(horizontal, hit))
        << "Raised terrain is invisible to collision — broadphase AABB was not refreshed (NotifyShapeChanged missing?)";
    const float32 regionStartZ = SampleToWorld(kR0, kN, kSize);
    EXPECT_NEAR(hit.hitPoint.Z, regionStartZ, 3.0f);

    // And the surface itself reads back at the raised height.
    RayCastQuery down{};
    down.origin = {wx, 100.0f, wx};
    down.direction = Vector3(0.0f, -1.0f, 0.0f);
    down.maxDistance = 500.0f;
    RayCastResult downHit{};
    ASSERT_TRUE(pw.RayCast(down, downHit));
    EXPECT_NEAR(downHit.hitPoint.Y, kRaisedHeight, 0.5f);
}

// The in-place path must convert raw provider samples into the shape's local
// height space with the exact creation mapping (local = offset.y + scale.y *
// raw). Every other test uses heightScale=1 where raw == local, so a
// forward-vs-inverse mapping error or a dropped scale passes them all —
// this test makes the mapping load-bearing.
TEST(Physics, HeightFieldInPlaceUpdateRespectsHeightScale)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::PhysicsBody;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    constexpr uint32 kN = 64;
    constexpr float32 kSize = 100.0f;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kHeightScale = 2.5f;
    constexpr float32 kRaisedRaw = 17.0f; // world 42.5, inside the padded encode range

    // Raw gradient in [4, 16] → world heights [10, 40] under the 2.5 scale.
    s_HeightFieldTestSamples.resize(kN * kN);
    for (uint32 z = 0; z < kN; ++z)
        for (uint32 x = 0; x < kN; ++x)
            s_HeightFieldTestSamples[z * kN + x] = 4.0f + 12.0f * static_cast<float32>(x) / static_cast<float32>(kN - 1);
    s_HeightFieldTestSampleCount = kN;
    s_HeightFieldTestVersion = 1;
    ScopedHeightFieldProvider providerGuard(&HeightFieldTestProvider);

    ECS::World world;
    auto e = CreateHeightFieldTerrainEntity(world, kSize, kSize);
    world.GetComponentForWrite<HeightFieldColliderShape>(e)->heightScale = kHeightScale;

    PhysicsInitSystem init;
    init.Update(world, kDt);
    pw.OptimizeBroadphase();

    const auto* pb = world.GetComponent<PhysicsBody>(e);
    ASSERT_NE(pb, nullptr);
    ASSERT_TRUE(pb->initialized);
    const BodyHandle initialBody = pb->body;

    // Sanity: the built surface already reflects the scale.
    const float32 wx = SampleToWorld(48, kN, kSize);
    auto surfaceAt = [&](float32 worldX, float32 worldZ) -> float32 {
        RayCastQuery down{};
        down.origin = {worldX, 200.0f, worldZ};
        down.direction = Vector3(0.0f, -1.0f, 0.0f);
        down.maxDistance = 500.0f;
        RayCastResult hit{};
        if (!pw.RayCast(down, hit))
            return -1000.0f;
        return static_cast<float>(hit.hitPoint.Y);
    };
    const float32 rawAt48 = 4.0f + 12.0f * 48.0f / static_cast<float32>(kN - 1);
    ASSERT_NEAR(surfaceAt(wx, wx), rawAt48 * kHeightScale, 1.0f);

    // Raise a 16x16 block (tier 1) in RAW units.
    constexpr int32 kR0 = 40;
    constexpr int32 kR1 = 56; // exclusive
    for (int32 z = kR0; z < kR1; ++z)
        for (int32 x = kR0; x < kR1; ++x)
            s_HeightFieldTestSamples[static_cast<size_t>(z) * kN + x] = kRaisedRaw;
    ++s_HeightFieldTestVersion;
    s_HeightFieldTestHasRegion = true;
    s_HeightFieldTestRegion[0] = kR0;
    s_HeightFieldTestRegion[1] = kR0;
    s_HeightFieldTestRegion[2] = kR1;
    s_HeightFieldTestRegion[3] = kR1;

    init.Update(world, kDt);

    // In-place (no rebuild) and the surface reads back at raw × scale —
    // a mapping error would read kRaisedRaw/kHeightScale (6.8) or raw (17).
    pb = world.GetComponent<PhysicsBody>(e);
    EXPECT_TRUE(pb->body == initialBody);
    EXPECT_EQ(world.GetComponent<HeightFieldColliderShape>(e)->lastBuiltVersion, s_HeightFieldTestVersion);
    EXPECT_NEAR(surfaceAt(wx, wx), kRaisedRaw * kHeightScale, 0.5f);
}

// F3: lowering terrain under a sleeping body must wake it. Statics are not
// island members, so without the explicit ActivateBodiesInAABB sweep the
// sleeper hovers over the lowered ground forever.
TEST(Physics, HeightFieldInPlaceLowerWakesSleepingBody)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::PhysicsBody;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    constexpr uint32 kN = 64;
    constexpr float32 kSize = 100.0f;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kSurface = 30.0f;
    constexpr float32 kLowered = 12.0f;

    // Mostly flat at 30 with one low row so shape creation has a real span
    // (encode range [5, 35] after headroom) that admits the lowered height.
    s_HeightFieldTestSamples.assign(kN * kN, kSurface);
    for (uint32 x = 0; x < kN; ++x)
        s_HeightFieldTestSamples[x] = 10.0f;
    s_HeightFieldTestSampleCount = kN;
    s_HeightFieldTestVersion = 1;
    ScopedHeightFieldProvider providerGuard(&HeightFieldTestProvider);

    ECS::World world;
    auto e = CreateHeightFieldTerrainEntity(world, kSize, kSize);

    PhysicsInitSystem init;
    init.Update(world, kDt);
    pw.OptimizeBroadphase();
    const auto* pb = world.GetComponent<PhysicsBody>(e);
    ASSERT_TRUE(pb->initialized);
    const BodyHandle terrainBody = pb->body;

    // A box created asleep on the surface: it never simulates until
    // something activates it.
    BoxShapeDef boxDef{};
    boxDef.halfExtents = Vector3(0.5f, 0.5f, 0.5f);
    const ShapeHandle boxShape = pw.CreateShape(boxDef);
    ASSERT_TRUE(boxShape.IsValid());
    BodySettings boxSettings{};
    boxSettings.shape = boxShape;
    boxSettings.motionType = MotionType::Dynamic;
    boxSettings.layer = Layers::Dynamic;
    boxSettings.mass = 1.0f;
    boxSettings.position = WorldPosition{0.0, kSurface + 0.5f, 0.0};
    boxSettings.startAwake = false;
    const BodyHandle box = pw.CreateBody(boxSettings);
    ASSERT_TRUE(box.IsValid());

    for (int i = 0; i < 30; ++i)
        pw.Step(kDt, 1);
    EXPECT_NEAR(pw.GetBodyTransform(box).position.Y, kSurface + 0.5f, 0.01f)
        << "Sanity: an asleep body must not move on its own";

    // Lower the ground under the box in place (box at origin = sample ~31.5,
    // inside the [24, 40) region).
    constexpr int32 kR0 = 24;
    constexpr int32 kR1 = 40;
    for (int32 z = kR0; z < kR1; ++z)
        for (int32 x = kR0; x < kR1; ++x)
            s_HeightFieldTestSamples[static_cast<size_t>(z) * kN + x] = kLowered;
    ++s_HeightFieldTestVersion;
    s_HeightFieldTestHasRegion = true;
    s_HeightFieldTestRegion[0] = kR0;
    s_HeightFieldTestRegion[1] = kR0;
    s_HeightFieldTestRegion[2] = kR1;
    s_HeightFieldTestRegion[3] = kR1;

    init.Update(world, kDt);
    pb = world.GetComponent<PhysicsBody>(e);
    EXPECT_TRUE(pb->body == terrainBody) << "Expected the in-place path, not a rebuild";

    // The wake sweep must have activated the box; it now falls to the
    // lowered ground.
    for (int i = 0; i < 180; ++i)
        pw.Step(kDt, 1);
    const double finalY = pw.GetBodyTransform(box).position.Y;
    EXPECT_LT(finalY, kSurface - 5.0f)
        << "Sleeping body never woke after the ground was lowered (ActivateBodiesInAABB missing?)";
    EXPECT_NEAR(finalY, kLowered + 0.5f, 1.0f);
}

// A rebuild that raises the ground by more than half a resting body's height puts the body's center under
// the new surface. The body must end on top of the new surface, not under it.
TEST(Physics, HeightFieldRebuildThatRaisesTheGroundByMoreThanHalfABodyLeavesItOnTop)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::PhysicsBody;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    constexpr uint32 kN = 64;
    constexpr float32 kSize = 100.0f;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kHalfExtent = 0.5f;
    constexpr float32 kSurface = 30.0f;
    constexpr float32 kRaised = 30.8f;

    s_HeightFieldTestSamples.assign(kN * kN, kSurface);
    s_HeightFieldTestSampleCount = kN;
    s_HeightFieldTestVersion = 1;
    ScopedHeightFieldProvider providerGuard(&HeightFieldTestProvider);

    ECS::World world;
    auto e = CreateHeightFieldTerrainEntity(world, kSize, kSize);
    PhysicsInitSystem init;
    init.SetRebuildQuiescenceSeconds(0.0f);
    init.Update(world, kDt);
    pw.OptimizeBroadphase();
    const auto* pb = world.GetComponent<PhysicsBody>(e);
    ASSERT_TRUE(pb->initialized);
    const BodyHandle terrainBody = pb->body;

    BoxShapeDef boxDef{};
    boxDef.halfExtents = Vector3(kHalfExtent, kHalfExtent, kHalfExtent);
    BodySettings boxSettings{};
    boxSettings.shape = pw.CreateShape(boxDef);
    boxSettings.motionType = MotionType::Dynamic;
    boxSettings.layer = Layers::Dynamic;
    boxSettings.mass = 1.0f;
    boxSettings.position = WorldPosition{0.0, kSurface + kHalfExtent, 0.0};
    boxSettings.startAwake = false;
    const BodyHandle box = pw.CreateBody(boxSettings);
    ASSERT_TRUE(box.IsValid());

    s_HeightFieldTestSamples.assign(kN * kN, kRaised);
    ++s_HeightFieldTestVersion;
    init.Update(world, kDt);
    pb = world.GetComponent<PhysicsBody>(e);
    ASSERT_TRUE(pb->initialized);
    EXPECT_FALSE(pb->body == terrainBody) << "Expected the rebuild path, not the in-place one";

    for (int step = 0; step < 120; ++step)
        pw.Step(kDt, 1);
    EXPECT_NEAR(pw.GetBodyTransform(box).position.Y, kRaised + kHalfExtent, 0.02f)
        << "the body did not end on top of the raised ground after the rebuild";
}

// Raising beyond the padded encode range must reject the in-place path
// (SetHeights would clamp silently) and fall back to a full rebuild, which
// re-pads the range around the new heights — after which in-place works again.
TEST(Physics, HeightFieldRangeExceededFallsBackToRebuild)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::PhysicsBody;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    constexpr uint32 kN = 64;
    constexpr float32 kSize = 100.0f;
    constexpr float32 kDt = 1.0f / 60.0f;
    // Gradient [10, 40] → headroom pads the encode range to [2.5, 47.5];
    // 60 exceeds it, forcing the rebuild fallback.
    constexpr float32 kBeyondRange = 60.0f;

    s_HeightFieldTestSamples.resize(kN * kN);
    for (uint32 z = 0; z < kN; ++z)
        for (uint32 x = 0; x < kN; ++x)
            s_HeightFieldTestSamples[z * kN + x] = 10.0f + 30.0f * static_cast<float32>(x) / static_cast<float32>(kN - 1);
    s_HeightFieldTestSampleCount = kN;
    s_HeightFieldTestVersion = 1;
    ScopedHeightFieldProvider providerGuard(&HeightFieldTestProvider);
    ECS::World world;
    auto e = CreateHeightFieldTerrainEntity(world, kSize, kSize);

    PhysicsInitSystem init;
    // Rebuild on first observation so the fallback lands this frame.
    init.SetRebuildQuiescenceSeconds(0.0f);
    init.Update(world, kDt);
    pw.OptimizeBroadphase();
    const auto* pb = world.GetComponent<PhysicsBody>(e);
    ASSERT_TRUE(pb->initialized);
    const BodyHandle initialBody = pb->body;

    constexpr int32 kR0 = 40;
    constexpr int32 kR1 = 56;
    auto setRegionHeight = [&](float32 h) {
        for (int32 z = kR0; z < kR1; ++z)
            for (int32 x = kR0; x < kR1; ++x)
                s_HeightFieldTestSamples[static_cast<size_t>(z) * kN + x] = h;
        ++s_HeightFieldTestVersion;
        s_HeightFieldTestHasRegion = true;
        s_HeightFieldTestRegion[0] = kR0;
        s_HeightFieldTestRegion[1] = kR0;
        s_HeightFieldTestRegion[2] = kR1;
        s_HeightFieldTestRegion[3] = kR1;
    };
    auto surfaceHeightAt = [&](float32 wx, float32 wz) -> float32 {
        RayCastQuery q{};
        q.origin = {wx, 200.0f, wz};
        q.direction = Vector3(0.0f, -1.0f, 0.0f);
        q.maxDistance = 500.0f;
        RayCastResult hit{};
        if (!pw.RayCast(q, hit))
            return -1.0f;
        return static_cast<float>(hit.hitPoint.Y);
    };
    const float32 wc = SampleToWorld((kR0 + kR1) / 2, kN, kSize);

    setRegionHeight(kBeyondRange);
    init.Update(world, kDt);
    pw.OptimizeBroadphase();
    pw.Step(kDt, 1);

    // Fallback consumed the edit via a full rebuild: new body, new geometry.
    pb = world.GetComponent<PhysicsBody>(e);
    ASSERT_TRUE(pb->initialized);
    EXPECT_FALSE(pb->body == initialBody) << "Out-of-range edit must take the rebuild path";
    EXPECT_EQ(world.GetComponent<HeightFieldColliderShape>(e)->lastBuiltVersion, s_HeightFieldTestVersion);
    EXPECT_NEAR(surfaceHeightAt(wc, wc), kBeyondRange, 0.5f);

    // The rebuild re-padded the range around [10, 60]; a further raise to 64
    // now fits and takes the in-place path (same body).
    const BodyHandle rebuiltBody = pb->body;
    setRegionHeight(64.0f);
    init.Update(world, kDt);
    pb = world.GetComponent<PhysicsBody>(e);
    EXPECT_TRUE(pb->body == rebuiltBody) << "Post-rebuild raise within the re-padded range should be in-place";
    EXPECT_NEAR(surfaceHeightAt(wc, wc), 64.0f, 0.5f);
}

// Tier-2 rebuilds are throttled on version quiescence: a storm of large
// edits (one per frame) must not pay one Jolt cook per frame — exactly one
// rebuild lands, after the version has been stable for the whole window.
TEST(Physics, HeightFieldThrottleCoalescesRebuildStorm)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::PhysicsBody;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    constexpr uint32 kN = 32;
    constexpr float32 kSize = 100.0f;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kWindow = 0.08f; // ~5 frames at 60 Hz

    s_HeightFieldTestSamples.assign(kN * kN, 20.0f);
    s_HeightFieldTestSampleCount = kN;
    s_HeightFieldTestVersion = 1;
    ScopedHeightFieldProvider providerGuard(&HeightFieldTestProvider);

    ECS::World world;
    auto e = CreateHeightFieldTerrainEntity(world, kSize, kSize);

    PhysicsInitSystem init;
    init.SetRebuildQuiescenceSeconds(kWindow);
    init.Update(world, kDt);
    pw.OptimizeBroadphase();
    const auto* pb = world.GetComponent<PhysicsBody>(e);
    ASSERT_TRUE(pb->initialized);
    const BodyHandle initialBody = pb->body;
    const uint64 builtVersion = world.GetComponent<HeightFieldColliderShape>(e)->lastBuiltVersion;

    // Storm: whole-terrain edits every frame (no region → tier 2). No
    // rebuild may land while versions keep changing.
    for (int i = 1; i <= 5; ++i)
    {
        std::fill(s_HeightFieldTestSamples.begin(), s_HeightFieldTestSamples.end(), 20.0f + static_cast<float32>(i));
        ++s_HeightFieldTestVersion;
        init.Update(world, kDt);
        pb = world.GetComponent<PhysicsBody>(e);
        EXPECT_TRUE(pb->body == initialBody) << "Rebuild leaked into the edit storm at bump " << i;
        EXPECT_EQ(world.GetComponent<HeightFieldColliderShape>(e)->lastBuiltVersion, builtVersion)
            << "Throttled gather must not advance lastBuiltVersion (it would never retry)";
    }

    // Quiet: the version is now stable; the single coalesced rebuild lands
    // once the quiescence window has elapsed.
    int rebuilds = 0;
    BodyHandle lastBody = initialBody;
    for (int frame = 1; frame <= 20; ++frame)
    {
        init.Update(world, kDt);
        pb = world.GetComponent<PhysicsBody>(e);
        if (!(pb->body == lastBody))
        {
            ++rebuilds;
            lastBody = pb->body;
        }
        if (frame <= 3)
        {
            EXPECT_EQ(rebuilds, 0) << "Rebuild landed before the quiescence window elapsed (frame " << frame << ")";
        }
    }
    EXPECT_EQ(rebuilds, 1);
    EXPECT_EQ(world.GetComponent<HeightFieldColliderShape>(e)->lastBuiltVersion, s_HeightFieldTestVersion);

    // The rebuilt collider reflects the final storm state.
    pw.OptimizeBroadphase();
    RayCastQuery q{};
    q.origin = {0.0, 100.0, 0.0};
    q.direction = Vector3(0.0f, -1.0f, 0.0f);
    q.maxDistance = 500.0f;
    RayCastResult hit{};
    ASSERT_TRUE(pw.RayCast(q, hit));
    EXPECT_NEAR(hit.hitPoint.Y, 25.0f, 0.5f);
}

// ============================================================================
// E6: per-tile terrain colliders
// ============================================================================

namespace
{
// Two independent "tiles" backed by file statics, addressed by dataHandle
// (1 -> tile 0, 2 -> tile 1), as the terrain's page bodies are: each tile is
// one collider entity + one body, and an edit bumps only its own version +
// region so PhysicsInit updates only that tile's body.
struct TileProbe
{
    std::vector<GameEngine::float32> Samples;
    GameEngine::uint32 Count = 0;
    GameEngine::uint64 Version = 0;
    bool HasRegion = false;
    GameEngine::int32 Region[4] = {0, 0, 0, 0}; // minX, minZ, maxX, maxZ (exclusive)
};
TileProbe s_TileProbes[2];

GameEngine::PhysicsECS::HeightFieldData PerTileTestProvider(GameEngine::uint32 handle,
                                                            GameEngine::uint32 /*generation*/,
                                                            GameEngine::uint64 sinceVersion)
{
    if (handle < 1 || handle > 2)
        return {};
    const TileProbe& t = s_TileProbes[handle - 1];
    GameEngine::PhysicsECS::HeightFieldData d;
    d.samples = t.Samples.data();
    d.sampleCount = t.Count;
    d.version = t.Version;
    if (t.HasRegion && sinceVersion < t.Version)
    {
        d.hasRegion = true;
        d.regionMinX = t.Region[0];
        d.regionMinZ = t.Region[1];
        d.regionMaxX = t.Region[2];
        d.regionMaxZ = t.Region[3];
    }
    return d;
}

GameEngine::ECS::EntityHandle CreateTileColliderEntity(GameEngine::ECS::World& world,
                                                       GameEngine::uint32 dataHandle,
                                                       GameEngine::float32 sizeXZ,
                                                       GameEngine::float32 heightScale,
                                                       GameEngine::float32 cx,
                                                       GameEngine::float32 cy,
                                                       GameEngine::float32 cz)
{
    using namespace GameEngine;
    auto e = world.CreateEntity();
    Components::Transform t{};
    t.Position = Mathematics::WorldPosition{cx, cy, cz};
    Components::WorldTransform wt{};
    wt.Position = t.Position;
    world.AddComponentImmediate<Components::Transform>(e, t);
    world.AddComponentImmediate<Components::WorldTransform>(e, wt);
    Components::PhysicsCollider collider{};
    collider.layer = Physics::Layers::Static;
    world.AddComponentImmediate<Components::PhysicsCollider>(e, collider);
    Components::HeightFieldColliderShape hf{};
    hf.dataHandle = dataHandle;
    hf.sizeX = sizeXZ;
    hf.sizeZ = sizeXZ;
    hf.heightScale = heightScale;
    world.AddComponentImmediate<Components::HeightFieldColliderShape>(e, hf);
    Components::PhysicsBody body{};
    body.motionType = Physics::MotionType::Static;
    world.AddComponentImmediate<Components::PhysicsBody>(e, body);
    return e;
}
} // namespace

// Mutation-verified per-body collider updates: two tiles = two bodies. Lowering the
// ground under a sleeping body over tile 1 must wake that body and update ONLY
// tile 1's collider in place — tile 0's body is untouched. heightScale != 1
// (scale = 1 hides the offset.y + scale.y*raw mapping).
TEST(Physics, TiledColliderEditUpdatesAndWakesOnlyEditedTile)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::PhysicsBody;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    constexpr uint32 kN = 64;
    constexpr float32 kSize = 100.0f;
    constexpr float32 kScale = 2.0f;        // heightScale != 1
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kSurfaceRaw = 15.0f;  // world surface = 15 * 2 = 30
    constexpr float32 kLoweredRaw = 6.0f;   // world 12

    for (TileProbe& t : s_TileProbes)
    {
        t.Samples.assign(kN * kN, kSurfaceRaw);
        for (uint32 x = 0; x < kN; ++x)
            t.Samples[x] = 5.0f; // one low row so the encode range has a real span
        t.Count = kN;
        t.Version = 1;
        t.HasRegion = false;
    }
    ScopedHeightFieldProvider providerGuard(&PerTileTestProvider);

    ECS::World world;
    // Tile 0 at x=-50 (spans [-100,0]), tile 1 at x=+50 (spans [0,100]) — disjoint.
    const auto tile0 = CreateTileColliderEntity(world, /*handle*/ 1, kSize, kScale, -50.0f, 0.0f, 0.0f);
    const auto tile1 = CreateTileColliderEntity(world, /*handle*/ 2, kSize, kScale, 50.0f, 0.0f, 0.0f);

    PhysicsInitSystem init;
    init.SetRebuildQuiescenceSeconds(0.0f);
    init.Update(world, kDt);
    pw.OptimizeBroadphase();

    const auto* pb0 = world.GetComponent<PhysicsBody>(tile0);
    const auto* pb1 = world.GetComponent<PhysicsBody>(tile1);
    ASSERT_TRUE(pb0->initialized);
    ASSERT_TRUE(pb1->initialized);
    const BodyHandle body0 = pb0->body;
    const BodyHandle body1 = pb1->body;
    EXPECT_FALSE(body0 == body1) << "each tile must get its own body";

    // A box asleep on tile 1's surface (world x=50).
    BoxShapeDef boxDef{};
    boxDef.halfExtents = Vector3(0.5f, 0.5f, 0.5f);
    const ShapeHandle boxShape = pw.CreateShape(boxDef);
    BodySettings boxSettings{};
    boxSettings.shape = boxShape;
    boxSettings.motionType = MotionType::Dynamic;
    boxSettings.layer = Layers::Dynamic;
    boxSettings.mass = 1.0f;
    boxSettings.position = WorldPosition{50.0, kSurfaceRaw * kScale + 0.5f, 0.0};
    boxSettings.startAwake = false;
    const BodyHandle box = pw.CreateBody(boxSettings);
    ASSERT_TRUE(box.IsValid());

    for (int i = 0; i < 30; ++i)
        pw.Step(kDt, 1);
    ASSERT_NEAR(pw.GetBodyTransform(box).position.Y, kSurfaceRaw * kScale + 0.5f, 0.01f)
        << "Sanity: the asleep box must not move on its own";

    const uint64 tile0VersionBefore =
        world.GetComponent<HeightFieldColliderShape>(tile0)->lastBuiltVersion;

    // Lower a region of tile 1 only (box at x=50 -> tile-local sample ~31.5).
    constexpr int32 kR0 = 24, kR1 = 40;
    for (int32 z = kR0; z < kR1; ++z)
        for (int32 x = kR0; x < kR1; ++x)
            s_TileProbes[1].Samples[static_cast<size_t>(z) * kN + x] = kLoweredRaw;
    ++s_TileProbes[1].Version;
    s_TileProbes[1].HasRegion = true;
    s_TileProbes[1].Region[0] = kR0; s_TileProbes[1].Region[1] = kR0;
    s_TileProbes[1].Region[2] = kR1; s_TileProbes[1].Region[3] = kR1;

    init.Update(world, kDt);

    // Tile 1 updated in place (same body, version advanced); tile 0 untouched.
    EXPECT_TRUE(world.GetComponent<PhysicsBody>(tile1)->body == body1)
        << "tile 1 must update in place, not rebuild";
    EXPECT_EQ(world.GetComponent<HeightFieldColliderShape>(tile1)->lastBuiltVersion,
              s_TileProbes[1].Version);
    EXPECT_EQ(world.GetComponent<HeightFieldColliderShape>(tile0)->lastBuiltVersion,
              tile0VersionBefore) << "an edit over tile 1 must not touch tile 0's collider";
    EXPECT_TRUE(world.GetComponent<PhysicsBody>(tile0)->body == body0);

    // The sleeping box woke and falls to the lowered ground.
    for (int i = 0; i < 180; ++i)
        pw.Step(kDt, 1);
    const double finalY = pw.GetBodyTransform(box).position.Y;
    EXPECT_LT(finalY, kSurfaceRaw * kScale - 5.0f)
        << "the box never woke after tile 1 was lowered (ActivateBodiesInAABB missing?)";
    EXPECT_NEAR(finalY, kLoweredRaw * kScale + 0.5f, 1.5f);
}

// A single (untiled) terrain's collider is queued with its shape in one flush; the shape's
// required PhysicsCollider default must not replace the provisioned one.
TEST(Physics, PlanarTerrainColliderKeepsTheStaticLayer)
{
    using namespace GameEngine;
    using GameEngine::Components::PhysicsCollider;
    namespace TE = GameEngine::TerrainECS;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;

    Terrain::TerrainConfig config{};
    config.HeightmapWidth = config.HeightmapHeight = 33;
    config.WorldSizeX = config.WorldSizeZ = 64.0f;
    config.HeightScale = 16.0f;
    config.LODLevels = 2;
    const TE::TerrainHandle data = TE::TerrainService::Get().CreateTerrain(config);
    ASSERT_NE(TE::TerrainService::Get().GetTerrainData(data), nullptr);
    TE::TerrainService::Get().GetTerrainData(data)->MarkFullDirty();

    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::Terrain tc{};
    tc.SizeX = tc.SizeZ = 64.0f;
    tc.HeightScale = 16.0f;
    tc.TerrainDataHandle = data.Index;
    tc.TerrainDataGeneration = data.Generation;
    world.AddComponentImmediate<Components::Terrain>(entity, tc);
    world.AddComponentImmediate<Components::WorldTransform>(entity, Components::WorldTransform{});

    TE::TerrainPhysicsSystem terrainPhysics;
    terrainPhysics.Update(world, 1.0f / 60.0f);
    world.ProcessCommands();

    const auto* collider = world.GetComponent<PhysicsCollider>(entity);
    ASSERT_NE(collider, nullptr);
    EXPECT_EQ(collider->layer, Physics::Layers::Static);
}

// ---- Paged terrain colliders: one body per finest page inside a CPU window (design page-streaming D3) ----

namespace
{
namespace PagedTE = GameEngine::TerrainECS;

// A paged terrain at 1 m on an entity, by default 2048 x 1024 m (16 x 8 level-0 pages, more than one
// tile's pages: colliders only in the windows), its two tiles loaded at Full as well: the oracle's
// heights, which the page bodies do not read. Frame drives the terrain's height pages as the
// extraction does (the dynamic bodies' windows, the request, the update), then TerrainPhysicsSystem
// and the flush.
class PagedColliderTerrain
{
public:
    explicit PagedColliderTerrain(GameEngine::ECS::World& world, GameEngine::float32 sizeX = 2048.0f,
                                  GameEngine::float32 sizeZ = 1024.0f)
        : Paged(PagedTE::Test::CreatePagedTiledTerrain(PagedTE::TerrainService::Get(), sizeX, sizeZ, 64.0f))
    {
        using namespace GameEngine;
        PagedTE::TerrainService& service = PagedTE::TerrainService::Get();
        for (int32 x = 0; x < 2; ++x)
        {
            PagedTE::TerrainTileData* tile = service.LoadTile(Paged.Tiled, PagedTE::TileCoord{x, 0});
            tile->LodState = PagedTE::TileLodState::Full;
            tile->MarkFullDirty();
        }
        Entity = world.CreateEntity();
        world.AddComponentImmediate<Components::Terrain>(Entity, Paged.Component);
        world.AddComponentImmediate<Components::WorldTransform>(Entity, Components::WorldTransform{});
    }

    // The world point of terrain-local meters (x, z) from the footprint's minimum corner.
    GameEngine::Mathematics::Vector3 At(GameEngine::float32 x, GameEngine::float32 z) const
    {
        return {Paged.Request.Shape.OriginX + x, 10.0f, Paged.Request.Shape.OriginZ + z};
    }

    // Frames with one camera at `camera` until `pages` page colliders exist (the window pages land
    // on jobs). False when they never do.
    bool ProvisionUntil(GameEngine::ECS::World& world, PagedTE::TerrainPhysicsSystem& system,
                        const GameEngine::Mathematics::Vector3& camera, int pages)
    {
        for (int attempt = 0; attempt < 2000; ++attempt)
        {
            Frame(world, system, camera);
            if (CountPageColliders(world) == pages)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    void Frame(GameEngine::ECS::World& world, PagedTE::TerrainPhysicsSystem& system,
               const GameEngine::Mathematics::Vector3& camera)
    {
        StreamPages(world, camera);
        system.Update(world, 1.0f / 60.0f);
        world.ProcessCommands();
    }

    // One frame of the height pages alone: the windows the next TerrainPhysicsSystem update reads.
    void StreamPages(GameEngine::ECS::World& world, const GameEngine::Mathematics::Vector3& camera)
    {
        PagedTE::TerrainPages& pages = PagedTE::TerrainService::Get().GetTerrainPages();
        pages.GatherBodyWindows(world);
        pages.Request(Paged.Request, {&camera, 1}, View());
        pages.Update(m_Frame++, 1.0f / 60.0f, false, 2);
    }

    static PagedTE::PageLevelView View()
    {
        return PagedTE::MakePageLevelView(PagedTE::kPageLevelReferenceRows, 1.0f, 11.0f);
    }

    static int CountPageColliders(GameEngine::ECS::World& world)
    {
        int count = 0;
        world.Query<GameEngine::ECS::Read<GameEngine::Components::TerrainPageCollider>>().Each(
            [&count](const GameEngine::Components::TerrainPageCollider&) { ++count; });
        return count;
    }

    static GameEngine::ECS::EntityHandle FindPageCollider(GameEngine::ECS::World& world, GameEngine::uint32 pageX,
                                                         GameEngine::uint32 pageZ)
    {
        GameEngine::ECS::EntityHandle found{};
        world.Query<GameEngine::ECS::Read<GameEngine::Components::TerrainPageCollider>>().Each(
            [&](GameEngine::ECS::EntityHandle entity, const GameEngine::Components::TerrainPageCollider& tag) {
                if (tag.PageX == pageX && tag.PageZ == pageZ)
                    found = entity;
            });
        return found;
    }

    PagedTE::Test::PagedTiledTerrain Paged;
    GameEngine::ECS::EntityHandle Entity{};

private:
    GameEngine::uint64 m_Frame = 1;
};

// A dynamic body entity at world (x, 0, z): it opens a CPU window (GatherBodyWindows).
void AddDynamicBody(GameEngine::ECS::World& world, const GameEngine::Mathematics::Vector3& at)
{
    using namespace GameEngine;
    Components::PhysicsBody body{};
    body.motionType = Physics::MotionType::Dynamic;
    Components::WorldTransform transform{};
    transform.Position.X = at.x;
    transform.Position.Z = at.z;
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, body);
    world.AddComponentImmediate(entity, transform);
}

// A downward ray at world (x, z): the height it meets, or false.
bool GroundUnder(GameEngine::float32 x, GameEngine::float32 z, GameEngine::float32& outY)
{
    using namespace GameEngine::Physics;
    RayCastQuery q{};
    q.origin = {x, 10000.0f, z};
    q.direction = Vector3(0.0f, -1.0f, 0.0f);
    q.maxDistance = 20000.0f;
    RayCastResult hit{};
    if (!GameEngine::PhysicsECS::PhysicsWorldService::Get().RayCast(q, hit) || !hit.HasHit())
        return false;
    outY = static_cast<GameEngine::float32>(hit.hitPoint.Y);
    return true;
}

// Every page collider stands at its page's centre, and a downward ray inside a window page meets the
// height the terrain's query answers there (the ground the renderer draws from the same pages).
void ExpectPageCollidersUnderTheirPages(GameEngine::ECS::World& world, const PagedColliderTerrain& terrain)
{
    using namespace GameEngine;
    using GameEngine::Components::TerrainPageCollider;
    using GameEngine::Components::WorldTransform;

    int colliders = 0;
    world.Query<ECS::Read<TerrainPageCollider>, ECS::Read<WorldTransform>>().Each(
        [&](const TerrainPageCollider& tag, const WorldTransform& transform) {
            ++colliders;
            const Mathematics::Vector3 centre = terrain.At((static_cast<float32>(tag.PageX) + 0.5f) * 128.0f,
                                                           (static_cast<float32>(tag.PageZ) + 0.5f) * 128.0f);
            EXPECT_DOUBLE_EQ(transform.Position.X, centre.x) << "page " << tag.PageX;
            EXPECT_DOUBLE_EQ(transform.Position.Z, centre.z) << "page " << tag.PageZ;
        });
    EXPECT_EQ(colliders, 4);

    const Mathematics::Vector3 point = terrain.At(330.5f, 270.25f);
    PagedTE::SurfaceHeightSample drawn;
    ASSERT_TRUE(PagedTE::TerrainService::Get()
                    .SurfaceQuery(terrain.Paged.Component, Components::WorldTransform{})
                    .SampleHeight(point, PagedTE::SurfaceHeightPolicy::RequireFinest, drawn));
    float32 y = 0.0f;
    ASSERT_TRUE(GroundUnder(point.x, point.z, y)) << "no collider under (" << point.x << ", " << point.z << ")";
    EXPECT_NEAR(y, drawn.Height, 0.5f) << "the collider there is not the page's ground";
}
} // namespace

// The camera at local (300, 300) keeps pages 1..2 x 1..2 on the CPU (its window): each gets one
// built body and no other page does.
TEST(Physics, PagedTerrainGetsABodyPerWindowPageAndNoneOutside)
{
    using namespace GameEngine;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::TerrainPageCollider;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(terrain.ProvisionUntil(world, terrainPhysics, terrain.At(300.0f, 300.0f), 4));
    PhysicsECS::PhysicsInitSystem init;
    init.Update(world, 1.0f / 60.0f);
    world.ProcessCommands();

    world.Query<ECS::Read<TerrainPageCollider>, ECS::Read<PhysicsBody>>().Each(
        [](const TerrainPageCollider& tag, const PhysicsBody& body) {
            EXPECT_TRUE(tag.PageX == 1u || tag.PageX == 2u) << tag.PageX;
            EXPECT_TRUE(tag.PageZ == 1u || tag.PageZ == 2u) << tag.PageZ;
            EXPECT_TRUE(body.initialized) << "page " << tag.PageX << ", " << tag.PageZ;
        });
    EXPECT_EQ(PagedColliderTerrain::CountPageColliders(world), 4);

    float32 y = 0.0f;
    const Mathematics::Vector3 inside = terrain.At(300.0f, 300.0f);
    EXPECT_TRUE(GroundUnder(inside.x, inside.z, y)) << "no ground under a window page";
    const Mathematics::Vector3 outside = terrain.At(1500.0f, 700.0f);
    EXPECT_FALSE(GroundUnder(outside.x, outside.z, y)) << "a page outside every window has a body";
}

// The oracle: a page body and the tile over the same ground, both filled from the same source in the
// test, hold the same samples. Page (7, 3) spans lattice 896..1024 x 384..512 of tile (0, 0); its
// far column, the page's apron, is the tile's last (the seam with tile (1, 0)).
TEST(Physics, APageBodyHoldsTheTileSamplesInsideAndOnTheApron)
{
    using namespace GameEngine;
    using GameEngine::Components::HeightFieldColliderShape;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(terrain.ProvisionUntil(world, terrainPhysics, terrain.At(1000.0f, 500.0f), 4));

    const ECS::EntityHandle page = PagedColliderTerrain::FindPageCollider(world, 7, 3);
    ASSERT_TRUE(page.IsValid());
    const auto* shape = world.GetComponent<HeightFieldColliderShape>(page);
    ASSERT_NE(shape, nullptr);
    EXPECT_FLOAT_EQ(shape->sizeX, 128.0f);
    EXPECT_FLOAT_EQ(shape->heightScale, 64.0f);
    const PhysicsECS::HeightFieldData pageData =
        PhysicsECS::GetHeightFieldDataProvider()(shape->dataHandle, shape->dataGeneration, 0);
    ASSERT_EQ(pageData.sampleCount, PagedTE::kFinestPageGridSamples);

    // What tile (0, 0) holds there: the same source, read through no page.
    const PagedTE::TiledTerrainData* tiled = PagedTE::TerrainService::Get().GetTiledTerrainData(terrain.Paged.Tiled);
    const Terrain::HeightfieldData& tile = tiled->Tiles.at(PagedTE::TileCoord{0, 0})->Heightfield;
    constexpr uint32 kFirstX = 7u * 128u;
    constexpr uint32 kFirstZ = 3u * 128u;
    const auto pageSample = [&pageData](uint32 x, uint32 z) {
        return pageData.samples[static_cast<std::size_t>(z) * PagedTE::kFinestPageGridSamples + x];
    };
    EXPECT_EQ(pageSample(64, 64), tile.GetSample(kFirstX + 64, kFirstZ + 64)) << "an interior sample";
    EXPECT_EQ(pageSample(128, 64), tile.GetSample(kFirstX + 128, kFirstZ + 64)) << "an apron sample on the seam";
    EXPECT_EQ(pageSample(128, 128), tile.GetSample(kFirstX + 128, kFirstZ + 128)) << "the apron corner";
    uint32 different = 0;
    for (uint32 z = 0; z < PagedTE::kFinestPageGridSamples; ++z)
        for (uint32 x = 0; x < PagedTE::kFinestPageGridSamples; ++x)
            different += pageSample(x, z) == tile.GetSample(kFirstX + x, kFirstZ + z) ? 0u : 1u;
    EXPECT_EQ(different, 0u) << "samples differ of " << PagedTE::kFinestPageGridSamples * PagedTE::kFinestPageGridSamples;
}

// A modifier edit over four by four samples of page (1, 1) reaches its body as a region of a new
// version: the body is updated in place (the same body, rebuilt from no whole grid) and a ray there
// meets the raised ground the terrain's query answers.
TEST(Physics, APageEditUpdatesItsBodyInPlace)
{
    using namespace GameEngine;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::PhysicsBody;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    PhysicsECS::PhysicsInitSystem init;
    const Mathematics::Vector3 camera = terrain.At(300.0f, 300.0f);
    ASSERT_TRUE(terrain.ProvisionUntil(world, terrainPhysics, camera, 4));
    init.Update(world, 1.0f / 60.0f);
    world.ProcessCommands();
    const ECS::EntityHandle page = PagedColliderTerrain::FindPageCollider(world, 1, 1);
    ASSERT_TRUE(page.IsValid());
    ASSERT_TRUE(world.GetComponent<PhysicsBody>(page)->initialized);
    const Physics::BodyHandle bodyBefore = world.GetComponent<PhysicsBody>(page)->body;
    const Mathematics::Vector3 point = terrain.At(201.0f, 201.0f);
    float32 groundBefore = 0.0f;
    ASSERT_TRUE(GroundUnder(point.x, point.z, groundBefore));

    // Lattice samples 200..203 on both axes: page (1, 1)'s grid samples 72..75.
    const PageStreaming::PageSampleRect rect{200, 200, 203, 203};
    constexpr float32 kRaise = 0.25f; // normalized: 16 m at a height scale of 64
    PagedTE::PagedTerrainRequest& request = terrain.Paged.Request;
    auto layer = std::make_shared<PageStreaming::HeightPageLayer>(
        1u, PageStreaming::BuildPageStoreLevels(request.Shape.SamplesX, request.Shape.SamplesZ));
    layer->WriteLevel0(0, rect, std::vector<float32>(static_cast<std::size_t>(rect.Width()) * rect.Height(), kRaise));
    request.Overlay = layer;
    request.OverlayVersion = 1;
    request.OverlayChanged = rect;
    request.OverlayChangedSince = 0;
    terrain.Frame(world, terrainPhysics, camera);

    const auto* shape = world.GetComponent<HeightFieldColliderShape>(page);
    ASSERT_NE(shape, nullptr);
    const PhysicsECS::HeightFieldData data =
        PhysicsECS::GetHeightFieldDataProvider()(shape->dataHandle, shape->dataGeneration, shape->lastBuiltVersion);
    EXPECT_EQ(data.version, shape->lastBuiltVersion + 1u) << "the edit made no new version of the body";
    ASSERT_TRUE(data.hasRegion);
    EXPECT_EQ(data.regionMinX, 72);
    EXPECT_EQ(data.regionMinZ, 72);
    EXPECT_EQ(data.regionMaxX, 76);
    EXPECT_EQ(data.regionMaxZ, 76);

    init.Update(world, 1.0f / 60.0f);
    world.ProcessCommands();
    EXPECT_EQ(world.GetComponent<HeightFieldColliderShape>(page)->lastBuiltVersion, data.version);
    EXPECT_EQ(world.GetComponent<PhysicsBody>(page)->body, bodyBefore) << "the body was rebuilt, not updated";
    PagedTE::SurfaceHeightSample drawn;
    ASSERT_TRUE(PagedTE::TerrainService::Get()
                    .SurfaceQuery(terrain.Paged.Component, Components::WorldTransform{})
                    .SampleHeight(point, PagedTE::SurfaceHeightPolicy::RequireFinest, drawn));
    float32 groundAfter = 0.0f;
    ASSERT_TRUE(GroundUnder(point.x, point.z, groundAfter));
    EXPECT_NEAR(groundAfter, drawn.Height, 0.5f) << "the body is not the edited ground";
    EXPECT_NEAR(groundAfter - groundBefore, kRaise * 64.0f, 0.5f);
}

// A dynamic body far from the camera keeps the finest body under it (design D3: the body windows are
// independent of the view; "physics might need some accuracy even if not in the field of view").
TEST(Physics, ADynamicBodyFarFromTheCameraKeepsTheFinestPageBodyUnderIt)
{
    using namespace GameEngine;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world);
    // Local (1800, 700): page (14, 5), its window 13..14 x 4..5.
    AddDynamicBody(world, terrain.At(1800.0f, 700.0f));
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(terrain.ProvisionUntil(world, terrainPhysics, terrain.At(300.0f, 300.0f), 8));
    EXPECT_TRUE(PagedColliderTerrain::FindPageCollider(world, 14, 5).IsValid()) << "no body under the far body";
    EXPECT_TRUE(PagedColliderTerrain::FindPageCollider(world, 13, 4).IsValid());
    EXPECT_TRUE(PagedColliderTerrain::FindPageCollider(world, 2, 2).IsValid()) << "and the camera keeps its own";
}

// A terrain with at most one tile's level-0 pages (1024 x 512 m: 9 x 5 pages) has a body on every
// page that owns a cell wherever the camera is, so a ray at the corner farthest from the camera
// meets the ground. The last page column and row own only the shared edge sample: 8 x 4 bodies.
TEST(Physics, ATerrainOfOneTileOrLessHasABodyOnEveryPageWhereverTheCameraIs)
{
    using namespace GameEngine;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world, 1024.0f, 512.0f);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(terrain.ProvisionUntil(world, terrainPhysics, terrain.At(10.0f, 10.0f), 8 * 4))
        << PagedColliderTerrain::CountPageColliders(world) << " page colliders";
    PhysicsECS::PhysicsInitSystem init;
    init.Update(world, 1.0f / 60.0f);
    world.ProcessCommands();

    const Mathematics::Vector3 farCorner = terrain.At(1020.0f, 508.0f);
    PagedTE::SurfaceHeightSample drawn;
    ASSERT_TRUE(PagedTE::TerrainService::Get()
                    .SurfaceQuery(terrain.Paged.Component, Components::WorldTransform{})
                    .SampleHeight(farCorner, PagedTE::SurfaceHeightPolicy::RequireFinest, drawn));
    float32 y = 0.0f;
    ASSERT_TRUE(GroundUnder(farCorner.x, farCorner.z, y)) << "no ground at the far corner";
    EXPECT_NEAR(y, drawn.Height, 0.5f) << "the collider there is not the page's ground";
}

// A 512 m terrain at 2 samples a meter on a decoded heightmap base with no cooked store, paged by the
// extraction's driver (RequestTiledTerrainPages): its query answers the heightmap's height, its whole
// level 0 (9 x 9 pages) stays in the CPU window, and a body stands on each of the 8 x 8 pages that own a cell.
TEST(Physics, AHeightmapTerrainWithoutACookedStoreSamplesItsHeightmapAndHasABodyOnEveryPage)
{
    using namespace GameEngine;

    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    std::unique_ptr<Rendering::IDevice> device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device: the driver reads the CBT arm from it";

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    PagedTE::TerrainService& service = PagedTE::TerrainService::Get();

    // A ramp: normalized height 0.5 u + 0.25 v over the footprint, which a bilinear sample keeps exact.
    constexpr uint32 kRampSamples = 65;
    constexpr float32 kHeightScale = 64.0f;
    constexpr float32 kSize = 512.0f;
    Terrain::HeightfieldData ramp(kRampSamples, kRampSamples);
    for (uint32 z = 0; z < kRampSamples; ++z)
        for (uint32 x = 0; x < kRampSamples; ++x)
            ramp.SetSample(x, z, (0.5f * static_cast<float32>(x) + 0.25f * static_cast<float32>(z)) /
                                     static_cast<float32>(kRampSamples - 1));
    const GUID heightmap = GUID::Generate();
    service.SeedDecodedHeightmapForTests(heightmap, ramp);

    Components::Terrain component;
    component.SizeX = kSize;
    component.SizeZ = kSize;
    component.HeightScale = kHeightScale;
    component.SamplesPerMeter = 2.0f;
    component.Domain = Components::TerrainDomain::Planar;
    component.BaseSource = Components::TerrainBaseSource::HeightmapAsset;
    component.TerrainAssetGuid.Set(heightmap);
    const PagedTE::TiledTerrainHandle tiledHandle = service.CreateTiledTerrain(service.BuildTiledTerrainConfig(component, "", std::nullopt));
    PagedTE::TiledTerrainData* tiled = service.GetTiledTerrainData(tiledHandle);
    ASSERT_NE(tiled, nullptr);
    Terrain::TerrainConfig renderConfig{};
    renderConfig.HeightmapWidth = 3;
    renderConfig.HeightmapHeight = 3;
    renderConfig.WorldSizeX = kSize;
    renderConfig.WorldSizeZ = kSize;
    const PagedTE::TerrainHandle render = service.CreateTerrain(renderConfig);
    tiled->GlobalGpuHandleIndex = render.Index;
    tiled->GlobalGpuHandleGeneration = render.Generation;
    component.TiledTerrainHandle = tiledHandle.Index;
    component.TiledTerrainGeneration = tiledHandle.Generation;
    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate<Components::Terrain>(entity, component);
    world.AddComponentImmediate<Components::WorldTransform>(entity, Components::WorldTransform{});

    const float32 originX = tiled->WorldOriginX;
    const float32 originZ = tiled->WorldOriginZ;
    const PagedTE::TiledRenderExtent extent = PagedTE::ComputeTiledRenderExtent(tiled->Config);
    const Mathematics::Vector3 camera{originX + 10.0f, 10.0f, originZ + 10.0f};
    const Mathematics::Vector3 farPoint{originX + 500.0f, 10.0f, originZ + 380.0f};
    PagedTE::TerrainPages& pages = service.GetTerrainPages();
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    for (uint64 frame = 1; frame < 2000 && PagedColliderTerrain::CountPageColliders(world) != 8 * 8; ++frame)
    {
        pages.GatherBodyWindows(world);
        PagedTE::RequestTiledTerrainPages(service, *device, *tiled, extent, render, "Heightmap512", 0.0f, kHeightScale,
                                          component.TargetPixelError, {&camera, 1}, {}, 0.0f);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        terrainPhysics.Update(world, 1.0f / 60.0f);
        world.ProcessCommands();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const PagedTE::SurfaceHeightQuery query = service.SurfaceQuery(component, Components::WorldTransform{});
    EXPECT_NE(query.Edits().Source, 0u) << "the heightmap terrain's pages have no source";
    for (const Mathematics::Vector3& point : {camera, farPoint})
    {
        const float32 u = (point.x - originX) / kSize;
        const float32 v = (point.z - originZ) / kSize;
        PagedTE::SurfaceHeightSample sample;
        ASSERT_TRUE(query.SampleHeight(point, PagedTE::SurfaceHeightPolicy::RequireFinest, sample))
            << "no finest height at (" << point.x << ", " << point.z << ")";
        EXPECT_NEAR(sample.Height, kHeightScale * (0.5f * u + 0.25f * v), 0.01f);
    }

    ASSERT_EQ(PagedColliderTerrain::CountPageColliders(world), 8 * 8);
    PhysicsECS::PhysicsInitSystem init;
    init.Update(world, 1.0f / 60.0f);
    world.ProcessCommands();
    float32 y = 0.0f;
    ASSERT_TRUE(GroundUnder(farPoint.x, farPoint.z, y)) << "no ground away from the camera";
    EXPECT_NEAR(y, kHeightScale * (0.5f * 500.0f / kSize + 0.25f * 380.0f / kSize), 0.5f);
}

namespace
{
// A tilted plane plus a paraboloid in terrain-local meters whose lowest point, (250, 250), lies
// inside a 512 m footprint: a capsule stands there without a slope to roll on.
class BowlOnAPlane final : public GameEngine::TerrainECS::ITerrainHeightFunction
{
public:
    explicit BowlOnAPlane(const GameEngine::TerrainECS::TerrainHeightFunctionContext&) {}

    static GameEngine::float32 Height(GameEngine::float32 x, GameEngine::float32 z)
    {
        const GameEngine::float32 dx = x - 500.0f;
        const GameEngine::float32 dz = z - 400.0f;
        return 20.0f + 0.05f * x + 0.03f * z + 1.0e-4f * (dx * dx + dz * dz);
    }

    void HeightRow(GameEngine::float32 z, std::span<const GameEngine::float32> x, GameEngine::float32,
                   std::span<GameEngine::float32> outMeters) const override
    {
        for (std::size_t i = 0; i < x.size(); ++i)
            outMeters[i] = Height(x[i], z);
    }

    GameEngine::TerrainECS::TerrainHeightRange HeightRange(GameEngine::float32, GameEngine::float32,
                                                           GameEngine::float32, GameEngine::float32,
                                                           GameEngine::float32) const override
    {
        return {0.0f, 128.0f};
    }
};
} // namespace

// A terrain whose base is a registered height function: the CPU query answers the function's height
// in meters, and a capsule dropped on its page colliders comes to rest on that height.
TEST(Physics, ACapsuleOnAFunctionTerrainStandsOnTheFunctionsHeight)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;

    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    std::unique_ptr<Rendering::IDevice> device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device: the driver reads the CBT arm from it";

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    PagedTE::TerrainService& service = PagedTE::TerrainService::Get();
    constexpr const char* kFunctionName = "PhysicsSmokeTests.BowlOnAPlane";
    ASSERT_TRUE(PagedTE::RegisterTerrainHeightFunction(
        kFunctionName, &PagedTE::MakeTerrainHeightFunction<BowlOnAPlane>));

    constexpr float32 kHeightScale = 128.0f;
    constexpr float32 kSize = 512.0f;
    Components::Terrain component;
    component.SizeX = kSize;
    component.SizeZ = kSize;
    component.HeightScale = kHeightScale;
    component.SamplesPerMeter = 2.0f;
    component.Domain = Components::TerrainDomain::Planar;
    component.BaseSource = Components::TerrainBaseSource::Function;
    std::strncpy(component.HeightFunction, kFunctionName, sizeof(component.HeightFunction) - 1u);
    component.BaseSeed = 3u;
    ASSERT_TRUE(service.IsTiledTerrainBaseReady(component));
    const PagedTE::TiledTerrainConfig config = service.BuildTiledTerrainConfig(component, "", std::nullopt);
    ASSERT_NE(config.Base.HeightFunction, nullptr) << "the registered function was not built";
    const PagedTE::TiledTerrainHandle tiledHandle = service.CreateTiledTerrain(config);
    PagedTE::TiledTerrainData* tiled = service.GetTiledTerrainData(tiledHandle);
    ASSERT_NE(tiled, nullptr);
    Terrain::TerrainConfig renderConfig{};
    renderConfig.HeightmapWidth = 3;
    renderConfig.HeightmapHeight = 3;
    renderConfig.WorldSizeX = kSize;
    renderConfig.WorldSizeZ = kSize;
    const PagedTE::TerrainHandle render = service.CreateTerrain(renderConfig);
    tiled->GlobalGpuHandleIndex = render.Index;
    tiled->GlobalGpuHandleGeneration = render.Generation;
    component.TiledTerrainHandle = tiledHandle.Index;
    component.TiledTerrainGeneration = tiledHandle.Generation;
    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate<Components::Terrain>(entity, component);
    world.AddComponentImmediate<Components::WorldTransform>(entity, Components::WorldTransform{});

    const float32 originX = tiled->WorldOriginX;
    const float32 originZ = tiled->WorldOriginZ;
    const PagedTE::TiledRenderExtent extent = PagedTE::ComputeTiledRenderExtent(tiled->Config);
    const Mathematics::Vector3 camera{originX + 10.0f, 10.0f, originZ + 10.0f};
    PagedTE::TerrainPages& pages = service.GetTerrainPages();
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    for (uint64 frame = 1; frame < 2000 && PagedColliderTerrain::CountPageColliders(world) != 8 * 8; ++frame)
    {
        pages.GatherBodyWindows(world);
        PagedTE::RequestTiledTerrainPages(service, *device, *tiled, extent, render, "Function512", 0.0f, kHeightScale,
                                          component.TargetPixelError, {&camera, 1}, {}, 0.0f);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        terrainPhysics.Update(world, 1.0f / 60.0f);
        world.ProcessCommands();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const PagedTE::SurfaceHeightQuery query = service.SurfaceQuery(component, Components::WorldTransform{});
    const Mathematics::Vector3 points[] = {camera, {originX + 470.3f, 10.0f, originZ + 81.7f}};
    for (const Mathematics::Vector3& point : points)
    {
        PagedTE::SurfaceHeightSample sample;
        ASSERT_TRUE(query.SampleHeight(point, PagedTE::SurfaceHeightPolicy::RequireFinest, sample))
            << "no finest height at (" << point.x << ", " << point.z << ")";
        EXPECT_NEAR(sample.Height, BowlOnAPlane::Height(point.x - originX, point.z - originZ), 1.0e-3f);
    }

    ASSERT_EQ(PagedColliderTerrain::CountPageColliders(world), 8 * 8);
    PhysicsECS::PhysicsInitSystem init;
    init.Update(world, 1.0f / 60.0f);
    world.ProcessCommands();

    constexpr float32 kLowestX = 250.0f;
    constexpr float32 kLowestZ = 250.0f;
    const float32 ground = BowlOnAPlane::Height(kLowestX, kLowestZ);
    PhysicsWorld& pw = PhysicsECS::PhysicsWorldService::Get();
    CapsuleShapeDef capsuleDef{};
    BodySettings capsuleSettings{};
    capsuleSettings.shape = pw.CreateShape(capsuleDef);
    capsuleSettings.motionType = MotionType::Dynamic;
    capsuleSettings.layer = Layers::Dynamic;
    capsuleSettings.mass = 1.0f;
    const float32 standing = capsuleDef.halfHeight + capsuleDef.radius;
    capsuleSettings.position = WorldPosition{originX + kLowestX, ground + standing + 0.25f, originZ + kLowestZ};
    const BodyHandle capsule = pw.CreateBody(capsuleSettings);
    ASSERT_TRUE(capsule.IsValid());
    for (int step = 0; step < 120; ++step)
        pw.Step(1.0f / 60.0f, 1);
    EXPECT_NEAR(pw.GetBodyTransform(capsule).position.Y, ground + standing, 0.02)
        << "the capsule did not come to rest on the function's height";
}

// A 576 x 300 m terrain paged on its tile lattice, which reaches past x = 576 m: the page that
// crosses the authored edge has ground up to the edge and none past it.
TEST(Physics, ThePageColliderOverTheAuthoredEdgeHasNoGroundPastIt)
{
    using namespace GameEngine;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world, 576.0f, 300.0f);
    const PagedTE::TiledTerrainData* tiled = PagedTE::TerrainService::Get().GetTiledTerrainData(terrain.Paged.Tiled);
    ASSERT_NE(tiled, nullptr);
    const PagedTE::TiledRenderExtent extent = PagedTE::ComputeTiledRenderExtent(tiled->Config);
    ASSERT_GE(extent.WorldSizeX, 588.0f) << "the premise: the tile lattice reaches past the authored edge";
    PagedTE::PageRequestTerrain& shape = terrain.Paged.Request.Shape;
    shape.SamplesX = extent.UnifiedWidth;
    shape.SamplesZ = extent.UnifiedHeight;
    shape.Level0TexelX = extent.WorldSizeX / static_cast<float32>(extent.UnifiedWidth - 1u);
    shape.Level0TexelZ = extent.WorldSizeZ / static_cast<float32>(extent.UnifiedHeight - 1u);

    // Page 4 on X (lattice samples 512 to 640) crosses the edge at 576.
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    const Mathematics::Vector3 camera = terrain.At(560.0f, 150.0f);
    for (int attempt = 0; attempt < 2000 && !PagedColliderTerrain::FindPageCollider(world, 4, 1).IsValid(); ++attempt)
    {
        terrain.Frame(world, terrainPhysics, camera);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(PagedColliderTerrain::FindPageCollider(world, 4, 1).IsValid()) << "page (4, 1) never got a collider";
    PhysicsECS::PhysicsInitSystem init;
    init.Update(world, 1.0f / 60.0f);
    world.ProcessCommands();

    float32 y = 0.0f;
    const Mathematics::Vector3 inside = terrain.At(575.5f, 150.0f);
    const Mathematics::Vector3 nextCell = terrain.At(576.5f, 150.0f);
    const Mathematics::Vector3 past = terrain.At(588.0f, 150.0f);
    EXPECT_TRUE(GroundUnder(inside.x, inside.z, y)) << "no ground inside the authored edge";
    EXPECT_FALSE(GroundUnder(nextCell.x, nextCell.z, y)) << "ground in the cell past the authored edge, at y = " << y;
    EXPECT_FALSE(GroundUnder(past.x, past.z, y)) << "ground 12 m past the authored edge, at y = " << y;
}

// When the camera moves on, the pages it left lose their colliders and their page slots, and the
// pages it reached get theirs.
TEST(Physics, APageColliderIsTornDownWhenItsPageLeavesEveryWindow)
{
    using namespace GameEngine;
    using GameEngine::Components::TerrainPageCollider;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(terrain.ProvisionUntil(world, terrainPhysics, terrain.At(300.0f, 300.0f), 4));
    const ECS::EntityHandle left = PagedColliderTerrain::FindPageCollider(world, 2, 2);
    ASSERT_TRUE(left.IsValid());
    const TerrainPageCollider tag = *world.GetComponent<TerrainPageCollider>(left);

    // Local (1500, 700): pages 11..12 x 5..6.
    ASSERT_TRUE(terrain.ProvisionUntil(world, terrainPhysics, terrain.At(1500.0f, 700.0f), 4));
    terrain.Frame(world, terrainPhysics, terrain.At(1500.0f, 700.0f));
    EXPECT_EQ(PagedColliderTerrain::CountPageColliders(world), 4);
    EXPECT_FALSE(world.IsValid(left)) << "the page the camera left kept its collider";
    EXPECT_EQ(PagedTE::TerrainService::Get().ResolvePageCollider(tag.PhysicsHandleIndex, tag.PhysicsHandleGeneration),
              nullptr)
        << "the page slot was not released";
    EXPECT_TRUE(PagedColliderTerrain::FindPageCollider(world, 11, 5).IsValid());
}

// A page slot release checks the generation: a handle kept past its release frees nothing, even
// when a new slot took its index. ReleaseAllPagePhysicsHandles (Play exit) frees every slot.
TEST(Physics, AStalePageSlotReleaseFreesNothingAndReleaseAllFreesEverySlot)
{
    using namespace GameEngine;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world);
    PagedTE::TerrainService& service = PagedTE::TerrainService::Get();
    // The camera at local (300, 300) brings pages 1..2 x 1..2 to the CPU.
    PagedTE::TerrainService::PagePhysicsHandle stale{};
    for (int attempt = 0; attempt < 2000 && stale.Index == 0; ++attempt)
    {
        terrain.StreamPages(world, terrain.At(300.0f, 300.0f));
        stale = service.AcquirePagePhysicsHandle(
            service.SurfaceQuery(terrain.Paged.Component, Components::WorldTransform{}), 1, 1);
        if (stale.Index == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_NE(stale.Index, 0u) << "page (1, 1) never reached the CPU";
    service.ReleasePagePhysicsHandle(stale.Index, stale.Generation);

    const PagedTE::SurfaceHeightQuery query = service.SurfaceQuery(terrain.Paged.Component, Components::WorldTransform{});
    const PagedTE::TerrainService::PagePhysicsHandle reused = service.AcquirePagePhysicsHandle(query, 1, 1);
    const PagedTE::TerrainService::PagePhysicsHandle other = service.AcquirePagePhysicsHandle(query, 2, 2);
    ASSERT_EQ(reused.Index, stale.Index) << "the new slot did not take the released index";
    ASSERT_NE(reused.Generation, stale.Generation);
    ASSERT_NE(other.Index, 0u);

    service.ReleasePagePhysicsHandle(stale.Index, stale.Generation);
    EXPECT_NE(service.ResolvePageCollider(reused.Index, reused.Generation), nullptr)
        << "a stale release freed the slot that took its index";
    EXPECT_NE(service.ResolvePageCollider(other.Index, other.Generation), nullptr);

    service.ReleaseAllPagePhysicsHandles();
    EXPECT_EQ(service.ResolvePageCollider(reused.Index, reused.Generation), nullptr);
    EXPECT_EQ(service.ResolvePageCollider(other.Index, other.Generation), nullptr);
}

// Page collider entities are engine-created runtime state: they carry RuntimeOnlyEntity, and a saved
// scene holds the terrain and none of them.
TEST(Physics, PageColliderEntitiesAreRuntimeOnlyAndExcludedFromSave)
{
    using namespace GameEngine;
    using GameEngine::Components::RuntimeOnlyEntity;
    using GameEngine::Components::TerrainPageCollider;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(terrain.ProvisionUntil(world, terrainPhysics, terrain.At(300.0f, 300.0f), 4));

    int tagged = 0;
    world.Query<ECS::Read<TerrainPageCollider>>().Each([&](ECS::EntityHandle entity, const TerrainPageCollider&) {
        tagged += world.HasComponent<RuntimeOnlyEntity>(entity) ? 1 : 0;
    });
    EXPECT_EQ(tagged, 4);

    const auto scenePath = std::filesystem::temp_directory_path() / "ge_page_collider_runtime_only.scene";
    ASSERT_TRUE(Scene::WriteSceneFile(world, scenePath, Scene::SceneWriteOptions{}));
    ECS::World reloaded;
    ASSERT_TRUE(Scene::ReadSceneInto(reloaded, scenePath, Scene::SceneReadOptions{}));
    int reloadedTotal = 0;
    int reloadedColliders = 0;
    for (auto* arch : reloaded.GetAllArchetypes())
    {
        if (!arch)
            continue;
        for (const auto& h : arch->CollectEntities())
        {
            if (!h.IsValid() || !reloaded.IsValid(h))
                continue;
            ++reloadedTotal;
            reloadedColliders += reloaded.HasComponent<TerrainPageCollider>(h) ? 1 : 0;
        }
    }
    EXPECT_GE(reloadedTotal, 1) << "the authored terrain entity must survive the round-trip";
    EXPECT_EQ(reloadedColliders, 0);
    std::error_code ec;
    std::filesystem::remove(scenePath, ec);
}

// Provisioning, the deferred flush and PhysicsInit called directly: each page collider lands with
// the transform and the static layer the provisioning queued, under its page.
TEST(Physics, PageCollidersStandUnderTheirPages)
{
    using namespace GameEngine;
    using GameEngine::Components::PhysicsCollider;
    using GameEngine::Components::TerrainPageCollider;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(terrain.ProvisionUntil(world, terrainPhysics, terrain.At(300.0f, 300.0f), 4));
    world.Query<ECS::Read<TerrainPageCollider>, ECS::Read<PhysicsCollider>>().Each(
        [](const TerrainPageCollider& tag, const PhysicsCollider& collider) {
            EXPECT_EQ(collider.layer, Physics::Layers::Static) << "page " << tag.PageX << ", " << tag.PageZ;
        });
    PhysicsECS::PhysicsInitSystem init;
    init.Update(world, 1.0f / 60.0f);
    world.ProcessCommands();
    ExpectPageCollidersUnderTheirPages(world, terrain);
}

// Through the modules' own schedule (TerrainPhysics, TransformHierarchy, PhysicsInit, the flush
// before each frame), with the height pages streamed first as the extraction streams them: the page
// colliders a frame provisions are built under their pages on the next.
TEST(Physics, PageCollidersStandUnderTheirPagesThroughTheFrameSchedule)
{
    using namespace GameEngine;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world);
    ASSERT_TRUE(PagedTE::Test::StreamPagesUntilTheCameraPageLands(PagedTE::TerrainService::Get(), terrain.Paged,
                                                                  terrain.At(300.0f, 300.0f)));

    ECS::SystemScheduleBuilder builder;
    builder.Add<Engine::Renderer::TransformHierarchySystem>("TransformHierarchy", ECS::SystemPhase::Extraction, 1);
    PhysicsECS::AddPhysicsSystemsToSchedule(builder);
    PagedTE::AddTerrainSystemsToSchedule(builder, nullptr);
    ECS::SystemManager manager;
    // Partial: the rendering, spline and CBT systems the terrain systems also name are absent.
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Partial);
    ASSERT_TRUE(manager.GetExecutionPlan().IsValid());
    for (int frame = 0; frame < 2; ++frame)
    {
        world.ProcessCommands();
        manager.Update(world, 1.0f / 60.0f);
    }
    ExpectPageCollidersUnderTheirPages(world, terrain);
}

// When a page leaves every window its body is freed at once and its entity destroyed at the next
// flush; until then the entity's body state is cleared, so nothing touches the freed Jolt handle.
TEST(Physics, APageColliderLeftByItsWindowClearsItsBodyStateBeforeTheDeferredDestroy)
{
    using namespace GameEngine;
    using GameEngine::Components::PhysicsBody;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(terrain.ProvisionUntil(world, terrainPhysics, terrain.At(300.0f, 300.0f), 4));
    PhysicsECS::PhysicsInitSystem init;
    init.Update(world, 1.0f / 60.0f);
    world.ProcessCommands();
    const ECS::EntityHandle orphan = PagedColliderTerrain::FindPageCollider(world, 2, 2);
    ASSERT_TRUE(orphan.IsValid());
    ASSERT_TRUE(world.GetComponent<PhysicsBody>(orphan)->initialized);

    // The window moves away; the sweep runs, the flush does not.
    terrain.StreamPages(world, terrain.At(1500.0f, 700.0f));
    terrainPhysics.Update(world, 1.0f / 60.0f);

    ASSERT_TRUE(world.IsValid(orphan)) << "DestroyEntity should still be pending (deferred)";
    const auto* body = world.GetComponent<PhysicsBody>(orphan);
    ASSERT_NE(body, nullptr);
    EXPECT_FALSE(body->initialized);
    EXPECT_FALSE(body->body.IsValid());
    EXPECT_FALSE(body->shape.IsValid());
    world.ProcessCommands();
    EXPECT_FALSE(world.IsValid(orphan));
}

// Two terrain entities sharing one tiled handle (an editor duplicate before re-provisioning) get one
// collider per window page, not two.
TEST(Physics, DuplicatedPagedTerrainEntitiesProvisionPageCollidersOnce)
{
    using namespace GameEngine;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    PagedColliderTerrain terrain(world);
    const ECS::EntityHandle duplicate = world.CreateEntity();
    world.AddComponentImmediate<Components::Terrain>(duplicate, terrain.Paged.Component);
    world.AddComponentImmediate<Components::WorldTransform>(duplicate, Components::WorldTransform{});
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(terrain.ProvisionUntil(world, terrainPhysics, terrain.At(300.0f, 300.0f), 4));
    terrain.Frame(world, terrainPhysics, terrain.At(300.0f, 300.0f));
    EXPECT_EQ(PagedColliderTerrain::CountPageColliders(world), 4);
}

// ---- Streamed planet page bodies (design page-streaming-planets-r4-colliders 5) ----

namespace
{
using GameEngine::Components::TerrainPlanetPageCollider;

// A streamed 2 km planet centered on `center`, its pages generated at a constant height with
// `level0Texel` meters between level-0 samples. The test streams its pages each frame as the
// extraction does, with one camera relative to the center.
class StreamedColliderPlanet
{
public:
    static constexpr GameEngine::float64 kRadius = 2000.0;

    StreamedColliderPlanet(GameEngine::ECS::World& world, GameEngine::float32 height,
                           const GameEngine::Mathematics::Vector3& center = {},
                           GameEngine::float32 level0Texel = 2.0f)
    {
        using namespace GameEngine;
        Entity = world.CreateEntity();
        Components::Terrain terrain{};
        terrain.Domain = Components::TerrainDomain::Spherical;
        terrain.PlanetRadius = static_cast<float32>(kRadius);
        world.AddComponentImmediate<Components::Terrain>(Entity, terrain);
        Components::WorldTransform transform{};
        transform.Position = Mathematics::WorldPosition::FromVector3(center);
        world.AddComponentImmediate<Components::WorldTransform>(Entity, transform);
        Request.Center = center;
        Request.Planet = static_cast<uint32>(Entity.id);
        Request.GpuPages = false;
        Request.Name = "planet";
        Request.Shape.ReferenceRadius = kRadius;
        Request.Shape.FaceSamples = PagedTE::PlanetFaceSamples(kRadius, level0Texel);
        SetHeight(height, 1);
    }

    // Generates the pages at `height` meters above the reference radius; a new `identity` restarts the
    // stream, as a relief or radius edit does.
    void SetHeight(GameEngine::float32 height, GameEngine::uint64 identity)
    {
        using namespace GameEngine;
        PageStreaming::GeneratedPlanetLattice lattice;
        lattice.ReferenceRadius = kRadius;
        lattice.FaceSamples = Request.Shape.FaceSamples;
        lattice.HeightRow = [height](std::span<const Mathematics::Vector3>, float32, std::span<float32> outHeights) {
            std::fill(outHeights.begin(), outHeights.end(), height);
        };
        lattice.HeightRange = [height](const Mathematics::Vector3&, float32, float32) {
            return PageStreaming::PageHeightRange{height, height};
        };
        Request.SourceIdentity = identity;
        Request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
    }

    void Frame(GameEngine::ECS::World& world, PagedTE::TerrainPhysicsSystem& system,
               const GameEngine::Mathematics::Vector3& camera)
    {
        StreamPages(world, camera);
        system.Update(world, 1.0f / 60.0f);
        world.ProcessCommands();
    }

    void StreamPages(GameEngine::ECS::World& world, const GameEngine::Mathematics::Vector3& camera)
    {
        PagedTE::TerrainPages& pages = PagedTE::TerrainService::Get().GetTerrainPages();
        pages.GatherBodyWindows(world);
        pages.RequestPlanet(Request, {&camera, 1}, PagedColliderTerrain::View());
        pages.Update(m_Frame++, 1.0f / 60.0f, false, 2);
    }

    // The pages that get a body in the planet's CPU windows now.
    size_t BodyPageCount() const
    {
        PagedTE::PlanetCpuView view;
        const PagedTE::TerrainPages* pages = PagedTE::TerrainService::Get().FindTerrainPages();
        return pages && pages->FindPlanetCpuView(Request.Planet, view) ? view.BodyPages.size() : 0;
    }

    // Frames until every body page has a body on the current stream. False when that never happens.
    bool ProvisionUntil(GameEngine::ECS::World& world, PagedTE::TerrainPhysicsSystem& system,
                        const GameEngine::Mathematics::Vector3& camera)
    {
        for (int attempt = 0; attempt < 2000; ++attempt)
        {
            Frame(world, system, camera);
            const size_t wanted = BodyPageCount();
            if (wanted != 0 && CountBodies(world, Request.SourceIdentity) == static_cast<int>(wanted))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    // The planet page bodies whose samples came from stream `identity`; 0 counts every body.
    static int CountBodies(GameEngine::ECS::World& world, GameEngine::uint64 identity = 0)
    {
        int count = 0;
        world.Query<GameEngine::ECS::Read<TerrainPlanetPageCollider>>().Each(
            [&count, identity](const TerrainPlanetPageCollider& tag) {
                if (identity == 0 || tag.Epoch == identity)
                    ++count;
            });
        return count;
    }

    static std::vector<TerrainPlanetPageCollider> Bodies(GameEngine::ECS::World& world)
    {
        std::vector<TerrainPlanetPageCollider> tags;
        world.Query<GameEngine::ECS::Read<TerrainPlanetPageCollider>>().Each(
            [&tags](const TerrainPlanetPageCollider& tag) { tags.push_back(tag); });
        return tags;
    }

    GameEngine::ECS::EntityHandle Entity{};
    PagedTE::PlanetPagedRequest Request;

private:
    GameEngine::uint64 m_Frame = 1;
};

// A camera 5 m above the +Y pole of the planet.
const GameEngine::Mathematics::Vector3 kPoleCamera{0.0f, 2015.0f, 0.0f};

// The center of a planet whose +Y pole stands about 300 km from the origin.
constexpr GameEngine::float64 kFarPlanetCenterY = 297990.0;
const GameEngine::Mathematics::Vector3 kFarPlanetCenter{0.0f, static_cast<GameEngine::float32>(kFarPlanetCenterY), 0.0f};

// Builds the backend bodies of the collider entities and flushes their components.
void BuildBodies(GameEngine::ECS::World& world)
{
    GameEngine::PhysicsECS::PhysicsInitSystem init;
    init.Update(world, 1.0f / 60.0f);
    world.ProcessCommands();
}
} // namespace

// The camera above the pole brings its window pages to the CPU: each gets one body, placed in its
// chart frame, and a ray down near the pole meets the drawn surface.
TEST(Physics, APlanetWindowPageGetsABodyAndARayDownMeetsTheDrawnSurface)
{
    using namespace GameEngine;
    constexpr float32 kHeight = 10.0f;
    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    StreamedColliderPlanet planet(world, kHeight);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(planet.ProvisionUntil(world, terrainPhysics, kPoleCamera));
    BuildBodies(world);

    for (const TerrainPlanetPageCollider& tag : StreamedColliderPlanet::Bodies(world))
        EXPECT_EQ(tag.Face, 2u) << "a window page near the +Y pole lies on another face";
    for (const float32 offset : {0.0f, 3.25f, -11.5f})
    {
        float32 y = 0.0f;
        ASSERT_TRUE(GroundUnder(offset, -offset * 0.5f, y)) << "no planet body under offset " << offset;
        const float64 expected =
            std::sqrt(std::pow(StreamedColliderPlanet::kRadius + kHeight, 2.0) - 1.25 * offset * offset);
        EXPECT_NEAR(y, expected, 0.02) << "the planet body at offset " << offset << " is not the drawn surface";
    }
}

// The pole's page bodies of a planet 300 km from the origin are built at their exact positions: a ray
// down meets the drawn surface there within a millimeter.
TEST(Physics, APlanetPageBodyFarFromTheOriginMeetsTheDrawnSurfaceWithinAMillimeter)
{
    using namespace GameEngine;
    constexpr float32 kHeight = 10.0f;
    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    StreamedColliderPlanet planet(world, kHeight, kFarPlanetCenter);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(planet.ProvisionUntil(world, terrainPhysics, kPoleCamera));
    BuildBodies(world);

    for (const float64 offset : {0.0, 3.25, -11.5, 7.125, 19.75})
    {
        Physics::RayCastQuery query{};
        query.origin = Physics::WorldPosition{offset, kFarPlanetCenterY + 2100.0, -offset * 0.5};
        query.direction = Physics::Vector3(0.0f, -1.0f, 0.0f);
        query.maxDistance = 200.0f;
        Physics::RayCastResult hit{};
        ASSERT_TRUE(PhysicsECS::PhysicsWorldService::Get().RayCast(query, hit) && hit.HasHit())
            << "no planet body under offset " << offset;
        const float64 expected = kFarPlanetCenterY +
            std::sqrt(std::pow(StreamedColliderPlanet::kRadius + kHeight, 2.0) - 1.25 * offset * offset);
        EXPECT_NEAR(hit.hitPoint.Y, expected, 0.001) << "the planet body at offset " << offset << " is not the drawn surface";
    }
}

// A camera that leaves the pole for the other side of the planet takes the pole's pages out of every
// window: their bodies are torn down and their slots released.
TEST(Physics, APlanetPageBodyIsReleasedWhenItsPageLeavesEveryWindow)
{
    using namespace GameEngine;
    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    StreamedColliderPlanet planet(world, 10.0f);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(planet.ProvisionUntil(world, terrainPhysics, kPoleCamera));
    const std::vector<TerrainPlanetPageCollider> poleBodies = StreamedColliderPlanet::Bodies(world);
    ASSERT_FALSE(poleBodies.empty());

    const Mathematics::Vector3 southCamera{0.0f, -2015.0f, 0.0f};
    ASSERT_TRUE(planet.ProvisionUntil(world, terrainPhysics, southCamera));
    planet.Frame(world, terrainPhysics, southCamera);
    for (const TerrainPlanetPageCollider& tag : StreamedColliderPlanet::Bodies(world))
        EXPECT_EQ(tag.Face, 3u) << "a body of a page outside every window is still alive";
    const PagedTE::TerrainService& service = PagedTE::TerrainService::Get();
    for (const TerrainPlanetPageCollider& tag : poleBodies)
        EXPECT_EQ(service.ResolvePageCollider(tag.PhysicsHandleIndex, tag.PhysicsHandleGeneration), nullptr)
            << "the slot of pole page (" << tag.PageX << ", " << tag.PageZ << ") was not released";
}

// A stream restart (a new relief) keeps a body under a probe at every frame: the old body stays until
// the new stream's body for its page is built, then the probe meets the new surface.
TEST(Physics, APlanetStreamRestartKeepsABodyUnderAProbeAtEveryFrame)
{
    using namespace GameEngine;
    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    StreamedColliderPlanet planet(world, 10.0f);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(planet.ProvisionUntil(world, terrainPhysics, kPoleCamera));
    BuildBodies(world);

    planet.SetHeight(20.0f, 2);
    bool onNewSurface = false;
    for (int frame = 0; frame < 2000 && !onNewSurface; ++frame)
    {
        planet.Frame(world, terrainPhysics, kPoleCamera);
        BuildBodies(world);
        float32 y = 0.0f;
        ASSERT_TRUE(GroundUnder(0.5f, 0.25f, y)) << "no body under the probe at frame " << frame << " of the restart";
        onNewSurface = std::abs(y - 2020.0f) < 0.05f;
        if (!onNewSurface)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(onNewSurface) << "the probe never met the restarted stream's surface";
    planet.Frame(world, terrainPhysics, kPoleCamera);
    BuildBodies(world);
    planet.Frame(world, terrainPhysics, kPoleCamera);
    EXPECT_EQ(StreamedColliderPlanet::CountBodies(world, 1), 0) << "a body of the old stream outlived its hand-over";
}

// A body asleep on the old surface wakes when the hand-over tears the old body down, and comes to rest
// on the restarted stream's lower surface.
TEST(Physics, APlanetHandOverWakesABodyAsleepOnTheOldSurface)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kHalfExtent = 0.5f;
    constexpr float32 kOldSurface = 2010.0f;
    constexpr float32 kNewSurface = 2009.7f;
    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    StreamedColliderPlanet planet(world, kOldSurface - 2000.0f);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(planet.ProvisionUntil(world, terrainPhysics, kPoleCamera));
    BuildBodies(world);

    PhysicsWorld& pw = PhysicsECS::PhysicsWorldService::Get();
    BoxShapeDef boxDef{};
    boxDef.halfExtents = Vector3(kHalfExtent, kHalfExtent, kHalfExtent);
    BodySettings boxSettings{};
    boxSettings.shape = pw.CreateShape(boxDef);
    boxSettings.motionType = MotionType::Dynamic;
    boxSettings.layer = Layers::Dynamic;
    boxSettings.mass = 1.0f;
    boxSettings.position = WorldPosition{0.0, kOldSurface + kHalfExtent, 0.0};
    boxSettings.startAwake = false;
    const BodyHandle box = pw.CreateBody(boxSettings);
    ASSERT_TRUE(box.IsValid());

    planet.SetHeight(kNewSurface - 2000.0f, 2);
    for (int frame = 0; frame < 2000 && StreamedColliderPlanet::CountBodies(world, 1) > 0; ++frame)
    {
        planet.Frame(world, terrainPhysics, kPoleCamera);
        BuildBodies(world);
        pw.Step(kDt, 1);
        if (StreamedColliderPlanet::CountBodies(world, 1) > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(StreamedColliderPlanet::CountBodies(world, 1), 0) << "the old stream's bodies were never handed over";
    for (int step = 0; step < 60; ++step)
        pw.Step(kDt, 1);
    EXPECT_NEAR(pw.GetBodyTransform(box).position.Y, kNewSurface + kHalfExtent, 0.02f)
        << "the body stayed asleep above the new surface after the hand-over";
}

namespace
{
// A hand-over raises the surface under a body asleep on the pole of a planet centered on `center`, by
// more than half the body's height. The body must end on top of the new surface.
void ExpectARaisingHandOverLeavesTheBodyOnTop(const GameEngine::Mathematics::Vector3& center)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kHalfExtent = 0.5f;
    constexpr float32 kOldSurface = 2010.0f;
    constexpr float32 kNewSurface = 2010.8f;
    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    StreamedColliderPlanet planet(world, kOldSurface - 2000.0f, center);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(planet.ProvisionUntil(world, terrainPhysics, kPoleCamera));
    BuildBodies(world);

    PhysicsWorld& pw = PhysicsECS::PhysicsWorldService::Get();
    BoxShapeDef boxDef{};
    boxDef.halfExtents = Vector3(kHalfExtent, kHalfExtent, kHalfExtent);
    BodySettings boxSettings{};
    boxSettings.shape = pw.CreateShape(boxDef);
    boxSettings.motionType = MotionType::Dynamic;
    boxSettings.layer = Layers::Dynamic;
    boxSettings.mass = 1.0f;
    const WorldPosition planetCenter = WorldPosition::FromVector3(center);
    boxSettings.position = planetCenter + Vector3(0.0f, kOldSurface + kHalfExtent, 0.0f);
    boxSettings.startAwake = false;
    const BodyHandle box = pw.CreateBody(boxSettings);
    ASSERT_TRUE(box.IsValid());

    planet.SetHeight(kNewSurface - 2000.0f, 2);
    for (int frame = 0; frame < 2000 && StreamedColliderPlanet::CountBodies(world, 1) > 0; ++frame)
    {
        planet.Frame(world, terrainPhysics, kPoleCamera);
        BuildBodies(world);
        pw.Step(kDt, 1);
        if (StreamedColliderPlanet::CountBodies(world, 1) > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(StreamedColliderPlanet::CountBodies(world, 1), 0) << "the old stream's bodies were never handed over";
    for (int step = 0; step < 120; ++step)
        pw.Step(kDt, 1);
    EXPECT_NEAR(pw.GetBodyTransform(box).position.Y - planetCenter.Y, kNewSurface + kHalfExtent, 0.02)
        << "the body did not end on top of the raised surface after the hand-over";
}
} // namespace

TEST(Physics, APlanetHandOverThatRaisesTheSurfaceByMoreThanHalfABodyLeavesItOnTop)
{
    ExpectARaisingHandOverLeavesTheBodyOnTop({});
}

// The settle reads the page bodies' sectors: the same hand-over on a planet 300 km from the origin.
TEST(Physics, APlanetHandOverFarFromTheOriginLeavesTheBodyOnTop)
{
    ExpectARaisingHandOverLeavesTheBodyOnTop(kFarPlanetCenter);
}

namespace
{
// A level-0 texel that gives the 2 km planet charts two pages wide, so a page seam inside a chart exists.
constexpr GameEngine::float32 kTwoPageChartTexel = 1.0f;

// The seams on the +Y face past the pole along the face's X axis, in level-0 samples: the first page
// seam that is not a chart seam, and the first chart seam.
struct PoleFaceSeams
{
    GameEngine::uint8 Face = 0;
    GameEngine::uint32 PoleZ = 0;
    GameEngine::uint32 PageSeam = 0;
    GameEngine::uint32 ChartSeam = 0;
};

PoleFaceSeams FindPoleFaceSeams(const StreamedColliderPlanet& planet)
{
    using namespace GameEngine;
    const uint32 faceSamples = planet.Request.Shape.FaceSamples;
    const PageStreaming::PageFacePoint pole =
        PageStreaming::EqualAngleFacePointOf(Mathematics::Vector3(0.0f, 1.0f, 0.0f), faceSamples);
    const PagedTE::PlanetChartLayout layout =
        PagedTE::ComputePlanetChartLayout(StreamedColliderPlanet::kRadius, faceSamples);
    const uint32 chartSamples = (1u << layout.Shift) * PageStreaming::kPageOwnedSamples;
    const uint32 poleX = static_cast<uint32>(pole.X);
    PoleFaceSeams seams;
    seams.Face = pole.Face;
    seams.PoleZ = static_cast<uint32>(pole.Z);
    seams.PageSeam = (poleX / PageStreaming::kPageOwnedSamples + 1u) * PageStreaming::kPageOwnedSamples;
    if (seams.PageSeam % chartSamples == 0u)
        seams.PageSeam += PageStreaming::kPageOwnedSamples;
    seams.ChartSeam = (poleX / chartSamples + 1u) * chartSamples;
    return seams;
}

// The world point `height` meters above the planet's center at level-0 sample (x, z) of `face`.
GameEngine::Mathematics::Vector3 PlanetSamplePoint(const StreamedColliderPlanet& planet, GameEngine::uint8 face,
                                                   GameEngine::uint32 x, GameEngine::uint32 z,
                                                   GameEngine::float32 height)
{
    return GameEngine::PageStreaming::EqualAngleSampleDirection(face, x, z, planet.Request.Shape.FaceSamples) * height;
}

// The level-0 sample coordinate along the face's X axis of the direction of `point`.
GameEngine::float64 FaceXOf(const StreamedColliderPlanet& planet, const GameEngine::Mathematics::Vector3& point)
{
    return GameEngine::PageStreaming::EqualAngleFacePointOf(point.Normalize(), planet.Request.Shape.FaceSamples).X;
}

// The offset of the body position `position` from the planet's center.
GameEngine::Mathematics::Vector3 FromPlanetCenter(const StreamedColliderPlanet& planet,
                                                  const GameEngine::Mathematics::WorldPosition& position)
{
    return position - GameEngine::Mathematics::WorldPosition::FromVector3(planet.Request.Center);
}
} // namespace

// A capsule walks across a chart seam near the +Y pole: the two pages' bodies stand in different chart
// frames, and the capsule stays grounded on the drawn surface with no step at the seam.
TEST(Physics, ACapsuleWalksAcrossAChartSeamNearThePoleWithoutAStep)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kSurface = 2010.0f;
    constexpr uint32 kReach = 16; // level-0 samples on each side of the seam, about 16 m
    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    StreamedColliderPlanet planet(world, kSurface - 2000.0f, {}, kTwoPageChartTexel);
    const PoleFaceSeams seams = FindPoleFaceSeams(planet);
    const Mathematics::Vector3 start = PlanetSamplePoint(planet, seams.Face, seams.ChartSeam - kReach, seams.PoleZ, kSurface);
    const Mathematics::Vector3 target =
        PlanetSamplePoint(planet, seams.Face, seams.ChartSeam + kReach, seams.PoleZ, kSurface);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(planet.ProvisionUntil(world, terrainPhysics, start.Normalize() * (kSurface + 5.0f)));
    BuildBodies(world);

    PhysicsWorld& pw = PhysicsECS::PhysicsWorldService::Get();
    CharacterSettings capsule{};
    capsule.motor = CharacterMotor::Kinematic;
    capsule.position = WorldPosition::FromVector3(start.Normalize() * (kSurface + 0.2f));
    capsule.radius = 0.4f;
    capsule.height = 1.8f;
    capsule.maxSlopeAngleDegrees = 45.0f;
    capsule.maxStepHeight = 0.4f;
    capsule.layer = Layers::Kinematic;
    const CharacterHandle character = pw.CreateCharacter(capsule);
    ASSERT_TRUE(character.IsValid());
    for (int step = 0; step < 60; ++step)
        pw.Step(kDt, 1);
    ASSERT_TRUE(pw.IsCharacterGrounded(character)) << "the capsule never landed on the planet";

    float32 previous = FromPlanetCenter(planet, pw.GetCharacterTransform(character).position).Length();
    for (int frame = 0;
         frame < 1200 && FaceXOf(planet, FromPlanetCenter(planet, pw.GetCharacterTransform(character).position)) <
                             static_cast<float64>(seams.ChartSeam + kReach / 2u);
         ++frame)
    {
        const Mathematics::Vector3 position = FromPlanetCenter(planet, pw.GetCharacterTransform(character).position);
        planet.Frame(world, terrainPhysics, position.Normalize() * (kSurface + 5.0f));
        BuildBodies(world);
        const Mathematics::Vector3 toTarget = target - position;
        Mathematics::Vector3 velocity = pw.GetCharacterLinearVelocity(character);
        const float32 across = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);
        velocity.x = 2.0f * toTarget.x / across;
        velocity.z = 2.0f * toTarget.z / across;
        pw.SetCharacterLinearVelocity(character, velocity);
        pw.Step(kDt, 1);

        const Mathematics::Vector3 moved = FromPlanetCenter(planet, pw.GetCharacterTransform(character).position);
        const float32 radial = moved.Length();
        const float64 faceX = FaceXOf(planet, moved);
        ASSERT_TRUE(pw.IsCharacterGrounded(character)) << "the capsule left the ground at face X " << faceX;
        ASSERT_NEAR(radial, kSurface, 0.05f) << "the capsule is off the surface at face X " << faceX;
        ASSERT_NEAR(radial, previous, 0.02f) << "the capsule stepped at face X " << faceX << ", the seam at "
                                             << seams.ChartSeam;
        previous = radial;
    }
    EXPECT_GE(FaceXOf(planet, FromPlanetCenter(planet, pw.GetCharacterTransform(character).position)),
              static_cast<float64>(seams.ChartSeam + kReach / 2u))
        << "the capsule did not cross the chart seam";
}

// A sphere rolls across a page seam and a chart seam near the +Y pole: near each seam its step is no
// worse than elsewhere. The chart seam is the worst point on the step and the radius, so this finds
// only a seam step above the path's own 0.023 m bounce; #6342 fixes the bounce and restores both bounds.
TEST(Physics, ASphereRollsAcrossAPageSeamAndAChartSeamWithoutAHop)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kSurface = 2010.0f;
    constexpr float32 kBallRadius = 0.5f;
    constexpr float64 kSeamReach = 8.0;
    // A float32 coordinate near 2010 m rounds to steps of 1/8192 m. A second difference of three
    // rounded points moves by up to two steps on each axis, so four steps cover its length.
    constexpr float32 kTraceQuantum = 4.0f / 8192.0f;
    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    StreamedColliderPlanet planet(world, kSurface - 2000.0f, {}, kTwoPageChartTexel);
    const PoleFaceSeams seams = FindPoleFaceSeams(planet);
    const Mathematics::Vector3 start = PlanetSamplePoint(planet, seams.Face, seams.PageSeam - 16u, seams.PoleZ, kSurface);
    const Mathematics::Vector3 target =
        PlanetSamplePoint(planet, seams.Face, seams.ChartSeam + 16u, seams.PoleZ, kSurface);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(planet.ProvisionUntil(world, terrainPhysics, start.Normalize() * (kSurface + 5.0f)));
    BuildBodies(world);

    PhysicsWorld& pw = PhysicsECS::PhysicsWorldService::Get();
    BodySettings ball{};
    ball.shape = pw.CreateShape(SphereShapeDef{kBallRadius});
    ball.motionType = MotionType::Dynamic;
    ball.layer = Layers::Dynamic;
    ball.position = WorldPosition::FromVector3(start.Normalize() * (kSurface + kBallRadius));
    ball.allowSleep = false;
    const Mathematics::Vector3 toTarget = target - start;
    const float32 across = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);
    ball.linearVelocity = Vector3(6.0f * toTarget.x / across, 0.0f, 6.0f * toTarget.z / across);
    const BodyHandle body = pw.CreateBody(ball);
    ASSERT_TRUE(body.IsValid());

    std::vector<Mathematics::Vector3> trace;
    for (int frame = 0; frame < 3600 && FaceXOf(planet, FromPlanetCenter(planet, pw.GetBodyTransform(body).position)) <
                                            static_cast<float64>(seams.ChartSeam + 8u);
         ++frame)
    {
        const Mathematics::Vector3 position = FromPlanetCenter(planet, pw.GetBodyTransform(body).position);
        planet.Frame(world, terrainPhysics, position.Normalize() * (kSurface + 5.0f));
        BuildBodies(world);
        pw.Step(kDt, 1);
        trace.push_back(FromPlanetCenter(planet, pw.GetBodyTransform(body).position));
    }
    ASSERT_GE(FaceXOf(planet, FromPlanetCenter(planet, pw.GetBodyTransform(body).position)),
              static_cast<float64>(seams.ChartSeam + 8u))
        << "the sphere did not roll across both seams";

    // Index 0 is within kSeamReach of the page seam, 1 of the chart seam, 2 is the rest of the path.
    std::array<float32, 3> worstSecond{};
    for (std::size_t index = 0; index < trace.size(); ++index)
    {
        EXPECT_GT(trace[index].Length(), kSurface) << "the sphere fell through the surface at frame " << index;
        const float64 faceX = FaceXOf(planet, trace[index]);
        std::size_t region = 2;
        if (std::abs(faceX - static_cast<float64>(seams.PageSeam)) <= kSeamReach)
            region = 0;
        else if (std::abs(faceX - static_cast<float64>(seams.ChartSeam)) <= kSeamReach)
            region = 1;
        if (index < 1 || index + 1 >= trace.size())
            continue;
        const Mathematics::Vector3 second = trace[index + 1] - trace[index] * 2.0f + trace[index - 1];
        worstSecond[region] = std::max(worstSecond[region], second.Length());
    }
    ASSERT_GT(worstSecond[0], 0.0f) << "no trace sample lies within " << kSeamReach << " samples of the page seam";
    ASSERT_GT(worstSecond[1], 0.0f) << "no trace sample lies within " << kSeamReach << " samples of the chart seam";
    for (std::size_t seam = 0; seam < 2; ++seam)
    {
        EXPECT_LE(worstSecond[seam], worstSecond[2] + kTraceQuantum)
            << "the sphere hops at the " << (seam == 0 ? "page" : "chart") << " seam: second difference "
            << worstSecond[seam] << " m, elsewhere " << worstSecond[2] << " m";
    }
}

namespace
{
// The calls of profiler scope `name` in the last whole frame.
GameEngine::uint32 ScopeCalls(std::string_view name)
{
    std::vector<std::pair<std::string_view, GameEngine::Profiling::CpuProfiler::Sample>> samples;
    GameEngine::Profiling::CpuProfiler::Get().CopyFrameSamples(samples);
    for (const auto& [scope, sample] : samples)
        if (scope == name)
            return sample.count;
    return 0;
}
} // namespace

// A resting body over a settled stream asks for no window page: the CPU windows pass only hands the
// residency its window again. A body or the camera moving one meter builds the window once.
TEST(Physics, ARestingBodyOverASettledPlanetStreamBuildsNoWindowPages)
{
#if !GE_ENABLE_CPU_PROFILING
    GTEST_SKIP() << "the test reads the CPU profiler's scope counts; ENABLE_CPU_PROFILING is OFF";
#else
    using namespace GameEngine;
    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    StreamedColliderPlanet planet(world, 10.0f);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    const ECS::EntityHandle body = world.CreateEntity();
    Components::PhysicsBody physicsBody{};
    physicsBody.motionType = Physics::MotionType::Dynamic;
    world.AddComponentImmediate<Components::PhysicsBody>(body, physicsBody);
    Components::WorldTransform resting{};
    resting.Position.X = 3.0;
    resting.Position.Y = 2010.5;
    world.AddComponentImmediate<Components::WorldTransform>(body, resting);
    ASSERT_TRUE(planet.ProvisionUntil(world, terrainPhysics, kPoleCamera));

    Profiling::CpuProfiler& profiler = Profiling::CpuProfiler::Get();
    const bool wasEnabled = profiler.IsEnabled();
    profiler.SetEnabled(true);
    constexpr int kRestingFrames = 30;
    uint32 windowPasses = 0;
    uint32 windowBuilds = 0;
    profiler.BeginFrame();
    for (int frame = 0; frame < kRestingFrames; ++frame)
    {
        planet.Frame(world, terrainPhysics, kPoleCamera);
        profiler.BeginFrame();
        windowPasses += ScopeCalls("Terrain.PlanetCpuWindows");
        windowBuilds += ScopeCalls("Terrain.PlanetCpuWindowPages");
    }
    Components::WorldTransform moved = resting;
    moved.Position.X = 4.0;
    world.AddComponentImmediate<Components::WorldTransform>(body, moved);
    planet.Frame(world, terrainPhysics, kPoleCamera);
    profiler.BeginFrame();
    const uint32 movedBuilds = ScopeCalls("Terrain.PlanetCpuWindowPages");
    planet.Frame(world, terrainPhysics, kPoleCamera + Mathematics::Vector3(1.0f, 0.0f, 0.0f));
    profiler.BeginFrame();
    const uint32 cameraBuilds = ScopeCalls("Terrain.PlanetCpuWindowPages");
    profiler.SetEnabled(wasEnabled);

    EXPECT_EQ(windowPasses, static_cast<uint32>(kRestingFrames)) << "the CPU windows pass did not run every frame";
    EXPECT_EQ(windowBuilds, 0u) << "the CPU windows pass built window pages while nothing moved";
    EXPECT_EQ(movedBuilds, 1u) << "the body moved one meter and its window was not built again";
    EXPECT_EQ(cameraBuilds, 1u) << "the camera moved one meter and its window was not built again";
#endif
}

// Play exit releases every planet page slot (ReleaseAllPagePhysicsHandles), as it does the planar ones.
TEST(Physics, ReleasingEveryPageSlotReleasesThePlanetPageSlots)
{
    using namespace GameEngine;
    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    StreamedColliderPlanet planet(world, 10.0f);
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    ASSERT_TRUE(planet.ProvisionUntil(world, terrainPhysics, kPoleCamera));
    PagedTE::TerrainService& service = PagedTE::TerrainService::Get();
    service.ReleaseAllPagePhysicsHandles();
    for (const TerrainPlanetPageCollider& tag : StreamedColliderPlanet::Bodies(world))
        EXPECT_EQ(service.ResolvePageCollider(tag.PhysicsHandleIndex, tag.PhysicsHandleGeneration), nullptr);
}

// A terrain switched from Planar to Spherical sheds the planar collider it kept on itself.
TEST(Physics, APlanetShedsThePlanarColliderOfItsPlanarPast)
{
    using namespace GameEngine;
    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    ScopedTerrainService terrainService;
    ECS::World world;
    StreamedColliderPlanet planet(world, 10.0f);
    world.AddComponentImmediate(planet.Entity, Components::HeightFieldColliderShape{});
    world.AddComponentImmediate(planet.Entity, Components::PhysicsBody{});
    world.AddComponentImmediate(planet.Entity, Components::PhysicsCollider{});
    PagedTE::TerrainPhysicsSystem terrainPhysics;
    planet.Frame(world, terrainPhysics, kPoleCamera);
    EXPECT_FALSE(world.HasComponent<Components::HeightFieldColliderShape>(planet.Entity));
    EXPECT_FALSE(world.HasComponent<Components::PhysicsBody>(planet.Entity));
    EXPECT_FALSE(world.HasComponent<Components::PhysicsCollider>(planet.Entity));
}

// ---------------------------------------------------------------------------
// Scene close releases Jolt bodies (design §4.3, commit 5).
//
// Every DestroyBody site in production is reached by querying LIVE entities, so
// World::Clear erased the entities and the orphan reapers never saw them — the
// bodies leaked for the process lifetime. The OnRemove<PhysicsBody> hook closes
// that, and these pin it on the real deviceless physics world.
//
// The constraint that shapes the hook is not Jolt's. Removal hooks run with the
// world's mutex held EXCLUSIVELY, and it is a non-recursive std::shared_mutex
// that World::GetComponent takes a shared lock on. A hook that resolved a
// sibling component off the dying entity would therefore self-deadlock on the
// first call, unconditionally — which is why each hook is registered against
// the plain `void(*)(T&)` overload and is self-sufficient from its own
// component. HooksNeverResolveASiblingComponent below pins that, because the
// failure mode is a hung editor rather than a red assertion.
// ---------------------------------------------------------------------------

namespace
{
using GameEngine::Components::PhysicsBody;

// A PhysicsBody as PhysicsInitSystem leaves it: a live Jolt body plus its shape.
PhysicsBody MakeLiveBody(GameEngine::Physics::PhysicsWorld& pw)
{
    using namespace GameEngine::Physics;
    SphereShapeDef sphere{};
    sphere.radius = 0.5f;

    PhysicsBody body{};
    body.shape = pw.CreateShape(sphere);

    BodySettings settings{};
    settings.shape = body.shape;
    settings.motionType = MotionType::Dynamic;
    settings.layer = Layers::Dynamic;
    settings.mass = 1.0f;
    settings.position = WorldPosition{0.0, 5.0, 0.0};
    body.body = pw.CreateBody(settings);
    body.initialized = true;
    return body;
}
} // namespace

TEST(PhysicsWorldHooks, WorldClearDestroysBodies)
{
    using namespace GameEngine;
    ScopedPhysicsWorldService scoped(SmallDeterministicSettings());
    auto& pw = PhysicsECS::PhysicsWorldService::Get();

    ECS::World world;
    PhysicsECS::RegisterPhysicsWorldHooks(world);

    const PhysicsBody body = MakeLiveBody(pw);
    ASSERT_TRUE(body.body.IsValid());
    ASSERT_TRUE(pw.IsBodyValid(body.body));
    const uint32 bodiesBefore = pw.GetBodyCount();

    const auto entity = world.CreateEntity();
    world.AddComponentImmediate(entity, body);

    world.Clear();

    EXPECT_FALSE(pw.IsBodyValid(body.body)) << "scene close must destroy the Jolt body";
    EXPECT_LT(pw.GetBodyCount(), bodiesBefore);
}

TEST(PhysicsWorldHooks, DestroyEntityDestroysBodies)
{
    using namespace GameEngine;
    ScopedPhysicsWorldService scoped(SmallDeterministicSettings());
    auto& pw = PhysicsECS::PhysicsWorldService::Get();

    ECS::World world;
    PhysicsECS::RegisterPhysicsWorldHooks(world);

    const PhysicsBody body = MakeLiveBody(pw);
    const auto entity = world.CreateEntity();
    world.AddComponentImmediate(entity, body);

    world.DestroyEntityImmediate(entity);

    EXPECT_FALSE(pw.IsBodyValid(body.body));
}

TEST(PhysicsWorldHooks, RemoveComponentDestroysBodies)
{
    using namespace GameEngine;
    ScopedPhysicsWorldService scoped(SmallDeterministicSettings());
    auto& pw = PhysicsECS::PhysicsWorldService::Get();

    ECS::World world;
    PhysicsECS::RegisterPhysicsWorldHooks(world);

    const PhysicsBody body = MakeLiveBody(pw);
    const auto entity = world.CreateEntity();
    world.AddComponentImmediate(entity, body);

    world.RemoveComponentImmediate<PhysicsBody>(entity);

    EXPECT_FALSE(pw.IsBodyValid(body.body));
}

// Duplicating an entity copies the handles, so two components can name one body.
// The first release frees it; the second must not free whatever now owns the id.
TEST(PhysicsWorldHooks, DoubleReleaseIsANoOp)
{
    using namespace GameEngine;
    ScopedPhysicsWorldService scoped(SmallDeterministicSettings());
    auto& pw = PhysicsECS::PhysicsWorldService::Get();

    PhysicsBody body = MakeLiveBody(pw);
    PhysicsECS::ReleasePhysicsBody(body);

    EXPECT_FALSE(body.body.IsValid()) << "the release must clear the handles it freed";
    EXPECT_FALSE(body.shape.IsValid());
    EXPECT_FALSE(body.initialized);

    PhysicsECS::ReleasePhysicsBody(body); // must not fault or free anything
    EXPECT_FALSE(body.body.IsValid());
}

// The hook also fires from ~World at process shutdown, when the physics world
// may already be gone.
TEST(PhysicsWorldHooks, ReleaseWithWorldTornDownDoesNotCrash)
{
    using namespace GameEngine;
    if (PhysicsECS::PhysicsWorldService::IsInitialized())
        PhysicsECS::PhysicsWorldService::Shutdown();

    PhysicsBody body{};
    body.initialized = true;
    body.childShapeCount = 2;
    PhysicsECS::ReleasePhysicsBody(body);

    EXPECT_FALSE(body.initialized);
    EXPECT_EQ(body.childShapeCount, 0u);
}

// Terrain collider handles are released by their own per-type hooks, and an
// entity carrying a collider AND a PhysicsBody fires both — which is the whole
// reason neither hook needs to resolve the other's component.
TEST(PhysicsWorldHooks, ColliderAndBodyHooksBothFireForOneEntity)
{
    using namespace GameEngine;
    ScopedPhysicsWorldService scoped(SmallDeterministicSettings());
    auto& pw = PhysicsECS::PhysicsWorldService::Get();

    ECS::World world;
    PhysicsECS::RegisterPhysicsWorldHooks(world);
    TerrainECS::RegisterTerrainWorldHooks(world);

    const PhysicsBody body = MakeLiveBody(pw);
    Components::TerrainPageCollider collider{};
    collider.PhysicsHandleIndex = 7;
    collider.PhysicsHandleGeneration = 1;

    const auto entity = world.CreateEntity();
    world.AddComponentImmediate(entity, body);
    world.AddComponentImmediate(entity, collider);

    world.Clear();

    EXPECT_FALSE(pw.IsBodyValid(body.body)) << "the PhysicsBody hook did not fire";
}

// The sharp constraint, pinned as source. A sibling lookup inside a hook body
// self-deadlocks instantly — the hook already holds worldMutex exclusively and
// GetComponent takes a shared lock on the same non-recursive mutex. It would
// present as a hung editor on every scene close, which no assertion can catch,
// so the shape is enforced here instead: the hooks take only their component.
TEST(PhysicsWorldHooks, HooksNeverResolveASiblingComponent)
{
    // Discovered, not listed: a NEW module's *WorldHooks.cpp is exactly the case
    // this guards, and a hard-coded roster lets that one through silently.
    //
    // Implementation files only. The constraint binds hook BODIES, which live in
    // the .cpp; the matching headers document the rule in prose and name
    // GetComponent while doing so, which a raw substring scan cannot tell from a
    // call.
    const std::filesystem::path repoRoot(GE_REPO_SOURCE_DIR);
    static constexpr std::string_view kSuffix = "WorldHooks.cpp";
    std::vector<std::filesystem::path> hookSources;
    for (const char* area : {"Engine", "Apps"})
    {
        const std::filesystem::path root = repoRoot / area;
        if (!std::filesystem::exists(root))
            continue;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
                continue;
            const std::string name = entry.path().filename().string();
            if (name.size() >= kSuffix.size() &&
                std::string_view(name).substr(name.size() - kSuffix.size()) == kSuffix)
                hookSources.push_back(entry.path());
        }
    }
    std::sort(hookSources.begin(), hookSources.end());

    // A scan that matched nothing is a staging failure wearing a green tick. The
    // floor is what exists today; it only ever moves up.
    ASSERT_GE(hookSources.size(), 3u)
        << "found " << hookSources.size()
        << " *WorldHooks.cpp under Engine/ and Apps/ — the scan is broken or the sources "
           "moved, so this test proved nothing";

    for (const auto& path : hookSources)
    {
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in.is_open()) << path.string();
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string src = ss.str();

        EXPECT_EQ(src.find("GetComponent"), std::string::npos)
            << path.filename().string()
            << ": a removal hook runs under worldMutex held EXCLUSIVELY, and GetComponent "
               "takes a shared lock on that same non-recursive mutex — resolving a sibling "
               "component here self-deadlocks the editor on every scene close. Every hook "
               "must be self-sufficient from its own component.";
        EXPECT_EQ(src.find("world.Get"), std::string::npos) << path.filename().string();
    }
}

TEST(Physics, ProceduralMeshReaderSurvivesReleaseAndRejectsStaleHandle)
{
    GameEngine::Physics::MeshShapeDef mesh;
    mesh.vertices = {{-1, 0, -1}, {1, 0, -1}, {0, 0, 1}};
    mesh.indices = {0, 2, 1};
    const auto handle = GameEngine::PhysicsECS::ProceduralMeshColliders::Register(mesh);
    const auto reader = GameEngine::PhysicsECS::ProceduralMeshColliders::Read(handle);
    ASSERT_NE(reader, nullptr);
    GameEngine::PhysicsECS::ProceduralMeshColliders::Release(handle);
    EXPECT_EQ(reader->indices.size(), 3u);
    EXPECT_EQ(GameEngine::PhysicsECS::ProceduralMeshColliders::Read(handle), nullptr);
    const auto replacement = GameEngine::PhysicsECS::ProceduralMeshColliders::Register(mesh);
    EXPECT_NE(replacement, handle);
    EXPECT_EQ(GameEngine::PhysicsECS::ProceduralMeshColliders::Read(handle), nullptr);
    GameEngine::PhysicsECS::ProceduralMeshColliders::Release(replacement);
    mesh.indices = {0, 2, 99};
    EXPECT_FALSE(GameEngine::PhysicsECS::ProceduralMeshColliders::Register(mesh).IsValid());
}

// The height where a ray straight down at (x, z) hits the physics world; -100 for a miss.
static float MeshSurfaceHeightAt(float x, float z)
{
    GameEngine::Physics::RayCastQuery query{};
    query.origin = {x, 10.0f, z};
    query.direction = GameEngine::Mathematics::Vector3(0.0f, -1.0f, 0.0f);
    GameEngine::Physics::RayCastResult hit{};
    return GameEngine::PhysicsECS::PhysicsWorldService::Get().RayCast(query, hit) ? static_cast<float>(hit.hitPoint.Y)
                                                                                   : -100.0f;
}

TEST(Physics, ProceduralMeshColliderBuildsAndRebuildsWithoutARenderer)
{
    ScopedPhysicsWorldService service(SmallDeterministicSettings());
    GameEngine::ECS::World world;
    GameEngine::PhysicsECS::RegisterPhysicsWorldHooks(world);
    const auto entity = world.CreateEntity();
    world.AddComponentImmediate(entity, GameEngine::Components::Transform{});
    world.AddComponentImmediate(entity, GameEngine::Components::WorldTransform{});
    GameEngine::Components::PhysicsBody body;
    body.motionType = GameEngine::Physics::MotionType::Static;
    world.AddComponentImmediate(entity, body);
    GameEngine::Physics::MeshShapeDef mesh;
    mesh.vertices = {{-1, 0, -1}, {1, 0, -1}, {0, 0, 1}};
    mesh.indices = {0, 2, 1};
    const auto first = GameEngine::PhysicsECS::ProceduralMeshColliders::Register(mesh);
    GameEngine::Components::MeshColliderShape collider;
    collider.Mesh = first;
    world.AddComponentImmediate(entity, collider);
    GameEngine::PhysicsECS::PhysicsInitSystem init;
    RunPhysicsInitFrame(world, init);
    ASSERT_TRUE(world.GetComponent<GameEngine::Components::PhysicsBody>(entity)->initialized);
    EXPECT_EQ(world.GetComponent<GameEngine::Components::MeshColliderShape>(entity)->BuiltMesh, first);
    EXPECT_NEAR(MeshSurfaceHeightAt(-0.5f, -0.9f), 0.0f, 1e-3f);
    // The new mesh takes the released slot, so only the handle generation tells the two apart.
    auto* written = world.GetComponentForWrite<GameEngine::Components::MeshColliderShape>(entity);
    written->Mesh = {};
    GameEngine::PhysicsECS::ProceduralMeshColliders::Release(first);
    mesh.vertices[0].y = 1;
    const auto replacement = GameEngine::PhysicsECS::ProceduralMeshColliders::Register(mesh);
    ASSERT_EQ(replacement.Index(), first.Index());
    written->Mesh = replacement;
    RunPhysicsInitFrame(world, init);
    EXPECT_TRUE(world.GetComponent<GameEngine::Components::PhysicsBody>(entity)->initialized);
    EXPECT_EQ(world.GetComponent<GameEngine::Components::MeshColliderShape>(entity)->BuiltMesh, replacement);
    // The rebuilt body has the raised triangle: its plane is at height 0.725 above that point.
    EXPECT_NEAR(MeshSurfaceHeightAt(-0.5f, -0.9f), 0.725f, 1e-3f);
    world.DestroyEntityImmediate(entity);
    GameEngine::PhysicsECS::ProceduralMeshColliders::Release(replacement);
}
