#include <gtest/gtest.h>

#include "EngineLogCapture.h"
#include "PagedTiledTerrain.h"
#include "PageStreaming/HeightPageOverlay.h"
#include "PageStreaming/PageStoreFormat.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Physics/PhysicsWorld.h"
#include "PhysicsECS/HeightFieldDataProvider.h"
#include "PhysicsECS/PhysicsEntityEventCallbacks.h"
#include "PhysicsECS/PhysicsScriptApi.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/KinematicBody.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "PhysicsECS/Systems/PhysicsInitSystem.h"
#include "PhysicsECS/Systems/PhysicsStepSystem.h"
#include "PhysicsECS/Systems/PhysicsWorldHooks.h"
#include "PhysicsECS/Systems/PhysicsWritebackSystem.h"
#include "PhysicsECS/Systems/RegisterPhysicsSystems.h"

#include "TerrainECS/TerrainService.h"
#include "TerrainECS/PlanetFaceCollider.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/Systems/RegisterTerrainSystems.h"
#include "TerrainECS/Systems/TerrainPhysicsSystem.h"
#include "TerrainECS/Systems/TerrainWorldHooks.h"
#include "TerrainECS/Components/TerrainPageCollider.h"
#include "TerrainECS/Components/TerrainPlanetFaceCollider.h"
#include "CBTTerrain/SphereSculptPaging.h"      // SphereSculptSampler (empty view = no sculpt)
#include "CBTTerrain/SphereAnalyticModifiers.h" // SphereAnalyticModifierSet (empty = no modifiers)
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/RuntimeOnlyEntity.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Scene/SceneIO.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
    ball.position = Vector3(0.0f, 5.0f, 0.0f);
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
    EXPECT_LT(t.position.y, 5.0f);
    // Should not tunnel through the plane.
    EXPECT_GT(t.position.y, radius * 0.5f);
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
    ball.position = Vector3(0.0f, 5.0f, 0.0f);
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
    sensor.position = Vector3(0.0f, -0.5f, 0.0f);
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
    ball.position = Vector3(0.0f, 5.0f, 0.0f);
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
    ball.position = Vector3(0.0f, 5.0f, 0.0f);
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
    ball.position = Vector3(0.0f, 5.0f, 0.0f);
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
// A system with no access that shares the physics step's wave.
class StepWavePeerSystem : public GameEngine::ECS::ISystem
{
  public:
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
    plan.Waves.push_back({{0, 1, 2}});
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
    const float32 fixed = PhysicsWorldService::GetSettings().fixedTimeStep > 0.0f
                              ? PhysicsWorldService::GetSettings().fixedTimeStep
                              : 1.0f / 60.0f;
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
    sensor.position = Vector3(0.0f, -0.5f, 0.0f);
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
    ball.position = Vector3(0.0f, 5.0f, 0.0f);
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
    EXPECT_LT(t.position.y, -1.0f);
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
    ball.position = Vector3(0.0f, 5.0f, 0.0f);
    ball.startAwake = true;
    const BodyHandle ballBody = world.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    constexpr float32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i)
        world.Step(dt, 1);

    const Transform t = world.GetBodyTransform(ballBody);
    // With filtering disabled, it should fall through y=0 plane.
    EXPECT_LT(t.position.y, -1.0f);
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
    ground.position = Vector3(0, 0, 0);
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
    ball.position = Vector3(0.0f, 20.0f, 0.0f); // above surface at Y=10
    ball.startAwake = true;
    const BodyHandle ballBody = world.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    // Raycast to verify surface position
    RayCastQuery q{};
    q.ray = Ray3D(Vector3(0, 100, 0), Vector3(0, -1, 0));
    q.maxDistance = 200.0f;
    RayCastResult hit{};
    (void)world.RayCast(q, hit);

    // Step physics
    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i)
        world.Step(dt, 1);

    const Transform t = world.GetBodyTransform(ballBody);

    // The sphere should have landed on the heightfield, NOT fallen through
    EXPECT_GT(t.position.y, 5.0f)
        << "Sphere at Y=" << t.position.y << " fell through heightfield (expected Y~10.5)";
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
    ground.position = Vector3(0, 0, 0);
    const BodyHandle groundBody = world.CreateBody(ground);
    ASSERT_TRUE(groundBody.IsValid());

    world.OptimizeBroadphase();

    // Raycast down from well above the terrain
    RayCastQuery q{};
    q.ray = Ray3D(Vector3(50, 200, 50), Vector3(0, -1, 0));
    q.maxDistance = 500.0f;
    RayCastResult hit{};
    bool didHit = world.RayCast(q, hit);

    ASSERT_TRUE(didHit) << "Raycast should hit the heightfield";
    // The surface at (50, ?, 50) should be between 9 and 53
    EXPECT_GT(hit.hitPoint.y, 5.0f)
        << "Surface hit Y=" << hit.hitPoint.y << " is too low (expected 9-53 range)";
    EXPECT_LT(hit.hitPoint.y, 60.0f)
        << "Surface hit Y=" << hit.hitPoint.y << " is too high (expected 9-53 range)";

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
    ball.position = Vector3(0.0f, 80.0f, 0.0f);
    ball.startAwake = true;
    const BodyHandle ballBody = world.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 300; ++i)
        world.Step(dt, 1);

    const Transform t = world.GetBodyTransform(ballBody);
    EXPECT_GT(t.position.y, 5.0f)
        << "Sphere at Y=" << t.position.y << " fell through heightfield";
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
    bs.position   = Vector3(0.0f, 5.0f, 0.0f);
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
        bs.position = Vector3(0.0f, 5.0f, 0.0f);
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

    std::vector<std::array<GameEngine::float32, 16>> matrices;
    std::vector<GameEngine::uint32> versions;
    for (auto e : entities)
    {
        std::array<GameEngine::float32, 16> m{};
        std::memcpy(m.data(), world.GetComponent<Transform>(e)->matrix, sizeof(Transform::matrix));
        matrices.push_back(m);
        versions.push_back(world.GetComponent<WorldTransform>(e)->Version);
    }

    for (int frame = 0; frame < 64; ++frame)
    {
        writeback.Update(world, 1.0f / 60.0f);
    }

    for (size_t i = 0; i < entities.size(); ++i)
    {
        EXPECT_EQ(std::memcmp(matrices[i].data(), world.GetComponent<Transform>(entities[i])->matrix,
                              sizeof(Transform::matrix)),
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
    bs.position = Vector3(1.0f, 5.0f, -2.0f);
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
    moved.position.y += 1.0f;
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

    world.GetComponentForWrite<Transform>(e)->matrix[12] += 3.0f;
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 1u);
    EXPECT_EQ(world.GetComponent<Transform>(e)->matrix[12], pose.position.x) << "the edited Transform kept its edit";

    world.GetComponentForWrite<WorldTransform>(e)->matrix[13] += 3.0f;
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 1u);
    EXPECT_EQ(world.GetComponent<WorldTransform>(e)->matrix[13], pose.position.y)
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
    PhysicsWorldService::SetRenderInterpolationAlpha(0.5f);

    const GameEngine::Physics::Transform rest = pw.GetBodyTransform(world.GetComponent<PhysicsBody>(e)->body);
    GameEngine::Physics::Transform away = rest;
    away.position.x += 2.0f;

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
    EXPECT_FLOAT_EQ(world.GetComponent<Transform>(e)->GetPosition().x, rest.position.x + 1.0f);

    SetPreviousPhysicsPose(world, e, rest);
    writeback.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(PhysicsWorldService::GetLastBodiesComposed(), 1u)
        << "the last write was a blend; it must not count as settled";
    EXPECT_EQ(world.GetComponent<Transform>(e)->GetPosition().x, rest.position.x);
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
    dynamicStart.position.x -= 10.0f;
    pw.SetBodyTransform(world.GetComponent<PhysicsBody>(dynamic)->body, dynamicStart);

    const GameEngine::Mathematics::Vector3 moved{4.0f, 2.0f, 7.0f};
    for (const auto e : {kinematic, dynamic})
    {
        auto* transform = world.GetComponentForWrite<Transform>(e);
        transform->matrix[12] = moved.x;
        transform->matrix[13] = moved.y;
        transform->matrix[14] = moved.z;
        std::memcpy(world.GetComponentForWrite<WorldTransform>(e)->matrix, transform->matrix,
                    sizeof(transform->matrix));
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
    EXPECT_FLOAT_EQ(kinematicPose.position.x, moved.x) << "the kinematic body did not follow its Transform";
    EXPECT_FLOAT_EQ(kinematicPose.position.y, moved.y);
    EXPECT_FLOAT_EQ(kinematicPose.position.z, moved.z);
    const auto kinematicPosition = world.GetComponent<Transform>(kinematic)->GetPosition();
    EXPECT_FLOAT_EQ(kinematicPosition.x, moved.x) << "the writeback snapped the kinematic Transform back";
    EXPECT_FLOAT_EQ(kinematicPosition.y, moved.y);
    EXPECT_FLOAT_EQ(kinematicPosition.z, moved.z);

    const GameEngine::Physics::Transform dynamicPose =
        pw.GetBodyTransform(world.GetComponent<PhysicsBody>(dynamic)->body);
    const auto dynamicPosition = world.GetComponent<Transform>(dynamic)->GetPosition();
    // The writeback blends the last two steps, so the Transform is near the body pose, not on it.
    constexpr GameEngine::float32 kInterpolationTolerance = 0.25f;
    EXPECT_NEAR(dynamicPosition.x, dynamicPose.position.x, kInterpolationTolerance)
        << "the dynamic Transform left its body";
    EXPECT_NEAR(dynamicPosition.y, dynamicPose.position.y, kInterpolationTolerance);
    EXPECT_NEAR(dynamicPosition.z, dynamicPose.position.z, kInterpolationTolerance);
    EXPECT_GT(std::abs(dynamicPose.position.z - moved.z), 1.0f) << "the dynamic body followed its Transform";
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
    bs.position = position;
    PhysicsBody pb{};
    pb.motionType = motionType;
    pb.body = pw.CreateBody(bs);
    pb.initialized = true;

    const auto e = world.CreateEntity();
    world.AddComponentImmediate<Transform>(e, Transform::FromTRS(position, Quaternion::Identity(), Vector3(1.0f, 1.0f, 1.0f)));
    WorldTransform wt{};
    std::memcpy(wt.matrix, world.GetComponent<Transform>(e)->matrix, sizeof(wt.matrix));
    world.AddComponentImmediate<WorldTransform>(e, wt);
    world.AddComponentImmediate<PhysicsBody>(e, pb);
    if (motionType == MotionType::Kinematic)
        world.AddComponentImmediate(e, GameEngine::Components::KinematicBody{});
    return e;
}

static void SetTransformHeight(GameEngine::ECS::World& world, GameEngine::ECS::EntityHandle e, GameEngine::float32 y)
{
    auto* transform = world.GetComponentForWrite<GameEngine::Components::Transform>(e);
    transform->matrix[13] = y;
    std::memcpy(world.GetComponentForWrite<GameEngine::Components::WorldTransform>(e)->matrix, transform->matrix,
                sizeof(transform->matrix));
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

    const GameEngine::float32 platformTop = pw.GetBodyTransform(platformBody).position.y + kPlatformHalfHeight;
    const GameEngine::float32 boxBottom = pw.GetBodyTransform(boxBody).position.y - kBoxHalfSize;
    constexpr GameEngine::float32 kContactSlop = 0.02f;
    EXPECT_NEAR(boxBottom, platformTop, kContactSlop) << "the box left the top of the platform";
    EXPECT_NEAR(pw.GetLinearVelocity(boxBody).y, kSpeed, 0.1f * kSpeed) << "the platform did not carry the box";

    for (int frame = 0; frame < 30; ++frame)
    {
        step.Update(world, kFrame);
        writeback.Update(world, kFrame);
    }
    EXPECT_FLOAT_EQ(pw.GetBodyTransform(platformBody).position.y, height) << "the platform drifted off its Transform";
    EXPECT_EQ(pw.GetLinearVelocity(platformBody).y, 0.0f) << "the platform kept moving";
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
    step.Update(world, 1.0f / 60.0f);
    writeback.Update(world, 1.0f / 60.0f);
    SetTransformHeight(world, e, 9.0f);
    step.Update(world, 1.0f / 120.0f);
    writeback.Update(world, 1.0f / 120.0f);

    EXPECT_FLOAT_EQ(world.GetComponent<Transform>(e)->GetPosition().y, 9.0f)
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
        *transform = Transform::FromTRS(start + Vector3(0.1f * frame, 0.0f, 0.0f),
                                        Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), 0.05f * frame),
                                        Vector3(1.0f, 1.0f, 1.0f));
        std::memcpy(world.GetComponentForWrite<WorldTransform>(e)->matrix, transform->matrix, sizeof(transform->matrix));
        runFrame();
    }
    EXPECT_EQ(PhysicsWorldService::GetLastKinematicBodiesMoving(), 1u);
    runFrame();

    const Transform* transform = world.GetComponent<Transform>(e);
    const auto position = transform->GetPosition();
    const auto rotation = transform->GetRotation().GetGLM();
    const GameEngine::Physics::Transform pose = pw.GetBodyTransform(body);
    EXPECT_EQ(pose.position.x, position.x) << "the stopped body is not on its pose";
    EXPECT_EQ(pose.position.y, position.y);
    EXPECT_EQ(pose.position.z, position.z);
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

    EXPECT_NEAR(pw.GetBodyTransform(body).position.y, start.y, 0.1f) << "the old world's target moved a body in the new world";
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
    EXPECT_EQ(pose.position.x, start.x) << "a script velocity moved the kinematic body off its Transform";
    EXPECT_EQ(pose.position.y, start.y);
    EXPECT_EQ(pose.position.z, start.z);
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
        q.ray = Ray3D(Vector3(0.0f, 100.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
        q.maxDistance = 500.0f;
        RayCastResult hit{};
        if (!pw.RayCast(q, hit))
            return -1.0f;
        return hit.hitPoint.y;
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

    JobSystem::WorkStealingThreadPool pool(1);
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::promise<void> busy;
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
    q.ray = Ray3D(Vector3(0.0f, 100.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
    q.maxDistance = 500.0f;
    RayCastResult hit{};
    ASSERT_TRUE(pw.RayCast(q, hit));
    EXPECT_NEAR(hit.hitPoint.y, kHeight, 0.5f);
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

    JobSystem::WorkStealingThreadPool pool(1);
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::promise<void> busy;
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
    q.ray = Ray3D(Vector3(0.0f, 100.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
    q.maxDistance = 500.0f;
    RayCastResult hit{};
    ASSERT_TRUE(pw.RayCast(q, hit));
    EXPECT_NEAR(hit.hitPoint.y, 30.0f, 0.5f) << "the body was built from the data the collider left";
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

// A terrain edit that lands while the collider is disabled must still reach
// the collider once it is re-enabled. Regression test for the gather
// advancing lastBuiltVersion for a collider that addCollider then drops —
// which would resurrect the pre-edit shape forever.
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
        q.ray = Ray3D(Vector3(0.0f, 100.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
        q.maxDistance = 500.0f;
        RayCastResult hit{};
        if (!pw.RayCast(q, hit))
            return -1.0f;
        return hit.hitPoint.y;
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
    horizontal.ray = Ray3D(Vector3(wx, 43.0f, -60.0f), Vector3(0.0f, 0.0f, 1.0f));
    horizontal.maxDistance = 200.0f;
    RayCastResult hit{};
    ASSERT_TRUE(pw.RayCast(horizontal, hit))
        << "Raised terrain is invisible to collision — broadphase AABB was not refreshed (NotifyShapeChanged missing?)";
    const float32 regionStartZ = SampleToWorld(kR0, kN, kSize);
    EXPECT_NEAR(hit.hitPoint.z, regionStartZ, 3.0f);

    // And the surface itself reads back at the raised height.
    RayCastQuery down{};
    down.ray = Ray3D(Vector3(wx, 100.0f, wx), Vector3(0.0f, -1.0f, 0.0f));
    down.maxDistance = 500.0f;
    RayCastResult downHit{};
    ASSERT_TRUE(pw.RayCast(down, downHit));
    EXPECT_NEAR(downHit.hitPoint.y, kRaisedHeight, 0.5f);
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
        down.ray = Ray3D(Vector3(worldX, 200.0f, worldZ), Vector3(0.0f, -1.0f, 0.0f));
        down.maxDistance = 500.0f;
        RayCastResult hit{};
        if (!pw.RayCast(down, hit))
            return -1000.0f;
        return hit.hitPoint.y;
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
    boxSettings.position = Vector3(0.0f, kSurface + 0.5f, 0.0f);
    boxSettings.startAwake = false;
    const BodyHandle box = pw.CreateBody(boxSettings);
    ASSERT_TRUE(box.IsValid());

    for (int i = 0; i < 30; ++i)
        pw.Step(kDt, 1);
    EXPECT_NEAR(pw.GetBodyTransform(box).position.y, kSurface + 0.5f, 0.01f)
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
    const float32 finalY = pw.GetBodyTransform(box).position.y;
    EXPECT_LT(finalY, kSurface - 5.0f)
        << "Sleeping body never woke after the ground was lowered (ActivateBodiesInAABB missing?)";
    EXPECT_NEAR(finalY, kLowered + 0.5f, 1.0f);
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
        q.ray = Ray3D(Vector3(wx, 200.0f, wz), Vector3(0.0f, -1.0f, 0.0f));
        q.maxDistance = 500.0f;
        RayCastResult hit{};
        if (!pw.RayCast(q, hit))
            return -1.0f;
        return hit.hitPoint.y;
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
    q.ray = Ray3D(Vector3(0.0f, 100.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
    q.maxDistance = 500.0f;
    RayCastResult hit{};
    ASSERT_TRUE(pw.RayCast(q, hit));
    EXPECT_NEAR(hit.hitPoint.y, 25.0f, 0.5f);
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
    t.matrix[12] = cx; t.matrix[13] = cy; t.matrix[14] = cz;
    Components::WorldTransform wt{};
    wt.matrix[12] = cx; wt.matrix[13] = cy; wt.matrix[14] = cz;
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
    boxSettings.position = Vector3(50.0f, kSurfaceRaw * kScale + 0.5f, 0.0f);
    boxSettings.startAwake = false;
    const BodyHandle box = pw.CreateBody(boxSettings);
    ASSERT_TRUE(box.IsValid());

    for (int i = 0; i < 30; ++i)
        pw.Step(kDt, 1);
    ASSERT_NEAR(pw.GetBodyTransform(box).position.y, kSurfaceRaw * kScale + 0.5f, 0.01f)
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
    const float32 finalY = pw.GetBodyTransform(box).position.y;
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

// A 2048 x 1024 m paged terrain (16 x 8 level-0 pages at 1 m) on an entity, its two tiles loaded at
// Full as well: the oracle's heights, which the page bodies do not read. Frame drives the
// terrain's height pages as the extraction does (the dynamic bodies' windows, the request, the
// update), then TerrainPhysicsSystem and the flush.
class PagedColliderTerrain
{
public:
    explicit PagedColliderTerrain(GameEngine::ECS::World& world)
        : Paged(PagedTE::Test::CreatePagedTiledTerrain(PagedTE::TerrainService::Get(), 2048.0f, 1024.0f, 64.0f))
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
        PagedTE::TerrainHeightPages& pages = PagedTE::TerrainService::Get().GetHeightPages();
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
    transform.matrix[12] = at.x;
    transform.matrix[14] = at.z;
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, body);
    world.AddComponentImmediate(entity, transform);
}

// A downward ray at world (x, z): the height it meets, or false.
bool GroundUnder(GameEngine::float32 x, GameEngine::float32 z, GameEngine::float32& outY)
{
    using namespace GameEngine::Physics;
    RayCastQuery q{};
    q.ray = Ray3D(Vector3(x, 10000.0f, z), Vector3(0.0f, -1.0f, 0.0f));
    q.maxDistance = 20000.0f;
    RayCastResult hit{};
    if (!GameEngine::PhysicsECS::PhysicsWorldService::Get().RayCast(q, hit) || !hit.HasHit())
        return false;
    outY = hit.hitPoint.y;
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
            EXPECT_FLOAT_EQ(transform.matrix[12], centre.x) << "page " << tag.PageX;
            EXPECT_FLOAT_EQ(transform.matrix[14], centre.z) << "page " << tag.PageZ;
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
    request.Overlay = std::make_shared<const PageStreaming::HeightPageOverlay>(
        PageStreaming::BuildPageStoreLevels(request.Shape.SamplesX, request.Shape.SamplesZ), rect,
        std::vector<float32>(static_cast<std::size_t>(rect.Width()) * rect.Height(), kRaise));
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
    ASSERT_TRUE(Scene::SaveSceneToFile(world, scenePath, Scene::SaveOptions{}));
    ECS::World reloaded;
    ASSERT_TRUE(Scene::LoadSceneFromFile(reloaded, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
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

// ---- Planet (spherical terrain) per-face colliders ----

namespace
{
using GameEngine::Components::TerrainPlanetFaceCollider;

// A spherical terrain entity at the world origin (the planet is centred there).
GameEngine::ECS::EntityHandle CreatePlanetEntity(GameEngine::ECS::World& world, float radius,
                                                 float reliefAmp)
{
    using namespace GameEngine;
    auto e = world.CreateEntity();
    Components::Terrain t{};
    t.Domain = Components::TerrainDomain::Spherical;
    t.PlanetRadius = radius;
    world.AddComponentImmediate<Components::Terrain>(e, t);
    // Base relief is the companion component the planet-collider provisioner reads —
    // the collider height field must match what the renderer draws (height-match).
    Components::TerrainPlanetRelief relief{};
    relief.Amplitude = reliefAmp;
    relief.Frequency = 6.0f;
    relief.Octaves = 3u;
    world.AddComponentImmediate<Components::TerrainPlanetRelief>(e, relief);
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    return e;
}

int CountPlanetFaceColliders(GameEngine::ECS::World& world)
{
    using namespace GameEngine;
    int n = 0;
    world.Query<ECS::Read<TerrainPlanetFaceCollider>>()
        .Each([&](ECS::EntityHandle, const TerrainPlanetFaceCollider&) { ++n; });
    return n;
}

// World surface point of the collider at grid cell (i,j) of `face` (planet at origin) —
// reconstructed from the SAME frame + sample the provisioner bakes, so tests can place a
// body exactly on an off-centre patch cell.
GameEngine::Vector3 FaceSurfaceWorld(GameEngine::uint32 face, GameEngine::uint32 i,
                                     GameEngine::uint32 j, float radius, float reliefAmp)
{
    using namespace GameEngine;
    TerrainECS::PlanetColliderParams p{};
    p.Radius = radius;
    p.ReliefAmplitude = reliefAmp;
    p.ReliefFrequency = 6.0f;
    p.ReliefOctaves = 3u;
    const uint32 dim = TerrainECS::kPlanetFaceColliderDim;
    std::vector<float> patch(static_cast<size_t>(dim) * dim, 0.0f);
    float lo = 0.0f, hi = 0.0f;
    TerrainECS::GeneratePlanetFacePatch(face, p, dim, CBTTerrain::SphereSculptSampler{},
                                        CBTTerrain::SphereAnalyticModifierSet{}, 0, 0,
                                        static_cast<int32_t>(dim) - 1,
                                        static_cast<int32_t>(dim) - 1, patch, lo, hi);
    const float sample = patch[static_cast<size_t>(j) * dim + i];
    std::array<float, 3> ta, n, tb;
    TerrainECS::ComputePlanetFaceFrame(face, ta, n, tb);
    const float R = radius;
    const float scale = 2.0f * R / static_cast<float>(dim - 1);
    const float lx = -R + static_cast<float>(i) * scale;
    const float lz = -R + static_cast<float>(j) * scale;
    const float ly = sample;
    return Vector3(ta[0] * lx + n[0] * ly + tb[0] * lz, ta[1] * lx + n[1] * ly + tb[1] * lz,
                   ta[2] * lx + n[2] * ly + tb[2] * lz);
}

// The +Y face patch centre sample (the pole surface height), regenerated from the same
// params + no sculpt the provisioner uses — the render/collider height a body must rest on.
float PoleSurfaceHeight(float radius, float reliefAmp)
{
    using namespace GameEngine;
    TerrainECS::PlanetColliderParams p{};
    p.Radius = radius;
    p.ReliefAmplitude = reliefAmp;
    p.ReliefFrequency = 6.0f;
    p.ReliefOctaves = 3u;
    const uint32 dim = TerrainECS::kPlanetFaceColliderDim;
    std::vector<float> patch(static_cast<size_t>(dim) * dim, 0.0f);
    float lo = 0.0f, hi = 0.0f;
    TerrainECS::GeneratePlanetFacePatch(2 /*+Y*/, p, dim, CBTTerrain::SphereSculptSampler{},
                                        CBTTerrain::SphereAnalyticModifierSet{}, 0, 0,
                                        static_cast<int32_t>(dim) - 1,
                                        static_cast<int32_t>(dim) - 1, patch, lo, hi);
    return patch[(dim / 2) * dim + (dim / 2)];
}
} // namespace

// Play-enter provisions exactly six per-face colliders (one Jolt body each); a domain
// switch back to Planar releases them; a double cycle holds 6 -> 0 -> 6 -> 0 without
// leaking planet-face slots (the E6 4->0->4->0 proof, generalized to the sphere).
TEST(Physics, PlanetTerrainProvisionsSixFaceCollidersLifecycle)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::PhysicsBody;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G
    {
        ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); }
    } g;

    ECS::World world;
    auto planet = CreatePlanetEntity(world, 2000.0f, 60.0f);

    TE::TerrainPhysicsSystem physics;
    PhysicsInitSystem init;
    init.SetRebuildQuiescenceSeconds(0.0f);

    // Enter: six face collider entities, each building a Jolt body.
    physics.Update(world, kDt);
    world.ProcessCommands();
    EXPECT_EQ(CountPlanetFaceColliders(world), 6) << "one collider entity per cube face";
    init.Update(world, kDt);
    world.ProcessCommands();
    int built = 0;
    world.Query<ECS::Read<TerrainPlanetFaceCollider>, ECS::Read<PhysicsBody>>()
        .Each([&](ECS::EntityHandle, const TerrainPlanetFaceCollider&, const PhysicsBody& b)
              { if (b.initialized) ++built; });
    EXPECT_EQ(built, 6) << "each face collider must build a Jolt body";
    EXPECT_EQ(TE::TerrainService::Get().GetPlanetFacePhysicsSlotCountForTests(), 6u);

    // Domain switch Spherical -> Planar: the six sphere faces are torn down.
    world.GetComponentForWrite<Components::Terrain>(planet)->Domain =
        Components::TerrainDomain::Planar;
    physics.Update(world, kDt);
    world.ProcessCommands();
    EXPECT_EQ(CountPlanetFaceColliders(world), 0) << "faces released on Spherical->Planar";

    // Back to Spherical: re-provision six.
    world.GetComponentForWrite<Components::Terrain>(planet)->Domain =
        Components::TerrainDomain::Spherical;
    physics.Update(world, kDt);
    world.ProcessCommands();
    EXPECT_EQ(CountPlanetFaceColliders(world), 6);
    init.Update(world, kDt);
    world.ProcessCommands();

    // Second teardown (the double cycle) — 0 again, pool never grew past six.
    world.GetComponentForWrite<Components::Terrain>(planet)->Domain =
        Components::TerrainDomain::Planar;
    physics.Update(world, kDt);
    world.ProcessCommands();
    EXPECT_EQ(CountPlanetFaceColliders(world), 0);
    EXPECT_LE(TE::TerrainService::Get().GetPlanetFacePhysicsSlotCountForTests(), 6u)
        << "the double domain cycle must not leak planet-face slots";
}

// A terrain that switches Planar -> Spherical must shed the planar heightfield collider the
// single-terrain branch put on the entity, then provision the six faces — no stray planar
// body left under the planet.
TEST(Physics, PlanetProvisioningShedsStalePlanarColliderOnDomainSwitch)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::PhysicsCollider;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G
    {
        ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); }
    } g;

    ECS::World world;
    auto planet = CreatePlanetEntity(world, 2000.0f, 60.0f);
    // Simulate the leftover planar collider a Planar->Spherical switch would strand.
    HeightFieldColliderShape stale{};
    stale.sizeX = 512.0f;
    stale.sizeZ = 512.0f;
    stale.heightScale = 1.0f;
    world.AddComponentImmediate<HeightFieldColliderShape>(planet, stale);
    world.AddComponentImmediate<PhysicsBody>(planet, PhysicsBody{});
    world.AddComponentImmediate<PhysicsCollider>(planet, PhysicsCollider{});
    ASSERT_TRUE(world.HasComponent<HeightFieldColliderShape>(planet));

    TE::TerrainPhysicsSystem physics;
    physics.Update(world, kDt);
    world.ProcessCommands();

    EXPECT_FALSE(world.HasComponent<HeightFieldColliderShape>(planet))
        << "the stale planar heightfield collider must be shed on a planet";
    EXPECT_EQ(CountPlanetFaceColliders(world), 6) << "the six face colliders still provision";
}

// A dynamic box dropped onto the pole of a small planet rests on the RENDERED surface
// (base radius + relief), not a diverged approximation — the physics HEIGHT-MATCH oracle.
TEST(Physics, PlanetColliderDroppedBoxLandsOnRenderedPoleSurface)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kRadius = 2000.0f;
    constexpr float32 kRelief = 60.0f;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G
    {
        ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); }
    } g;

    ECS::World world;
    CreatePlanetEntity(world, kRadius, kRelief);

    TE::TerrainPhysicsSystem physics;
    PhysicsInitSystem init;
    init.SetRebuildQuiescenceSeconds(0.0f);
    physics.Update(world, kDt);
    world.ProcessCommands();
    init.Update(world, kDt);
    world.ProcessCommands();

    const float poleH = PoleSurfaceHeight(kRadius, kRelief);

    BoxShapeDef boxDef{};
    boxDef.halfExtents = Vector3(0.5f, 0.5f, 0.5f);
    const ShapeHandle boxShape = pw.CreateShape(boxDef);
    BodySettings bs{};
    bs.shape = boxShape;
    bs.motionType = MotionType::Dynamic;
    bs.layer = Layers::Dynamic;
    bs.mass = 1.0f;
    bs.position = Vector3(0.0f, poleH + 3.0f, 0.0f); // just above the +Y pole
    bs.startAwake = true;
    const BodyHandle box = pw.CreateBody(bs);
    ASSERT_TRUE(box.IsValid());
    pw.OptimizeBroadphase();

    for (int i = 0; i < 400; ++i)
        pw.Step(kDt, 1);

    const float y = pw.GetBodyTransform(box).position.y;
    EXPECT_NEAR(y, poleH + 0.5f, 0.5f)
        << "box rest height must match the rendered pole surface (didn't tunnel / float)";
}

// A sculpt raise on ONE face refreshes only that face's collider (count oracle), and a box
// asleep on the raised region wakes and settles on the new, higher surface (the E1 in-place
// SetHeights + ActivateBodiesInAABB seam, driven by the sphere sculpt mirror).
TEST(Physics, PlanetColliderSculptRefreshUpdatesOnlyDirtyFaceAndWakesBox)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kRadius = 2000.0f;
    constexpr float32 kRelief = 60.0f;
    constexpr uint32 kPlusY = 2u;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G
    {
        ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); }
    } g;
    auto& terrain = TE::TerrainService::Get();

    ECS::World world;
    CreatePlanetEntity(world, kRadius, kRelief);

    TE::TerrainPhysicsSystem physics;
    PhysicsInitSystem init;
    init.SetRebuildQuiescenceSeconds(0.0f);
    physics.Update(world, kDt);
    world.ProcessCommands();
    init.Update(world, kDt);
    world.ProcessCommands();

    // Sleep a box on the pole. A tall box (half-extent 2 m) so the modest sculpt raise
    // (< half-extent) lifts it without the one-sided heightfield surface passing over its
    // centre — a single-frame raise larger than the box would drop it through.
    const float poleH = PoleSurfaceHeight(kRadius, kRelief);
    constexpr float kHalf = 2.0f;
    BoxShapeDef boxDef{};
    boxDef.halfExtents = Vector3(kHalf, kHalf, kHalf);
    const ShapeHandle boxShape = pw.CreateShape(boxDef);
    BodySettings bs{};
    bs.shape = boxShape;
    bs.motionType = MotionType::Dynamic;
    bs.layer = Layers::Dynamic;
    bs.mass = 1.0f;
    bs.position = Vector3(0.0f, poleH + kHalf, 0.0f);
    bs.startAwake = false;
    const BodyHandle box = pw.CreateBody(bs);
    ASSERT_TRUE(box.IsValid());
    pw.OptimizeBroadphase();
    for (int i = 0; i < 60; ++i)
        pw.Step(kDt, 1);
    const float ySlept = pw.GetBodyTransform(box).position.y;
    ASSERT_NEAR(ySlept, poleH + kHalf, 0.2f) << "box must be asleep on the pole before the edit";

    // Record each face collider's built version, keyed by face.
    uint64 builtBefore[TE::kPlanetFaceCount] = {};
    world.Query<ECS::Read<TerrainPlanetFaceCollider>, ECS::Read<HeightFieldColliderShape>>()
        .Each([&](ECS::EntityHandle, const TerrainPlanetFaceCollider& tag,
                  const HeightFieldColliderShape& s)
              { if (tag.Face < TE::kPlanetFaceCount) builtBefore[tag.Face] = s.lastBuiltVersion; });

    // Raise the +Y face centre with a broad sculpt dab: strength kRaise peaks at the pole, and
    // angularRadius 0.4 rad stays well inside the +Y cube face (edge is ~0.785 rad away) so only
    // that face's collider updates, while being >> the 2 m box footprint so the raise under the
    // box is ~uniform kRaise. The raise is under the box half-extent so the box is lifted rather
    // than passed through.
    constexpr float kRaise = 1.0f;
    terrain.SeedPlanetSculptMirrorForTests(/*dir*/ 0.0f, 1.0f, 0.0f, /*angularRadius*/ 0.4f, kRaise,
                                           /*lower*/ false);

    physics.Update(world, kDt);      // regenerates the +Y face region -> bumps its slot version
    world.ProcessCommands();
    init.Update(world, kDt);         // provider reports the region -> in-place SetHeights + wake
    world.ProcessCommands();

    // Only the +Y face's collider advanced; the other five are untouched.
    uint64 builtAfter[TE::kPlanetFaceCount] = {};
    world.Query<ECS::Read<TerrainPlanetFaceCollider>, ECS::Read<HeightFieldColliderShape>>()
        .Each([&](ECS::EntityHandle, const TerrainPlanetFaceCollider& tag,
                  const HeightFieldColliderShape& s)
              { if (tag.Face < TE::kPlanetFaceCount) builtAfter[tag.Face] = s.lastBuiltVersion; });
    for (uint32 f = 0; f < TE::kPlanetFaceCount; ++f)
    {
        if (f == kPlusY)
            EXPECT_GT(builtAfter[f], builtBefore[f]) << "the edited +Y face collider must update";
        else
            EXPECT_EQ(builtAfter[f], builtBefore[f]) << "face " << f << " must be untouched";
    }

    // The box wakes and is lifted onto the raised surface. Its PEAK height in the frames
    // right after the edit is the wake + in-place-raise signal; long-term it slides down the
    // sphere curvature under plain -Y gravity (radial gravity is out of scope — a body only
    // rests indefinitely at the exact pole), so the enduring rest height is not asserted.
    float peakY = ySlept;
    for (int i = 0; i < 60; ++i)
    {
        pw.Step(kDt, 1);
        peakY = std::max(peakY, pw.GetBodyTransform(box).position.y);
    }
    EXPECT_GT(peakY, ySlept + 0.4f)
        << "the box never woke / rose after the sculpt raise (ActivateBodiesInAABB missing?)";
    EXPECT_NEAR(peakY, poleH + kRaise + kHalf, 0.6f)
        << "the box must be lifted onto the new raised pole surface";
}

// The planet-face provider composes with a non-unit heightScale: a synthetic face buffer at
// heightScale=2 rests a box at raw*2, and an in-place region LOWER (routed through the
// kPlanetFacePhysicsHandleBit discriminator) wakes the box, which falls to the new scaled
// height. A lower (not a raise) keeps the one-sided heightfield below the box throughout.
TEST(Physics, PlanetFaceProviderRespectsHeightScaleAndWakes)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::PhysicsCollider;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr uint32 kDim = 16;
    constexpr float32 kSize = 100.0f;
    constexpr float32 kScale = 2.0f;    // heightScale != 1 (world = raw * 2)
    constexpr float32 kFlatRaw = 10.0f;
    constexpr float32 kLoweredRaw = 4.0f;

    ScopedPhysicsWorldService svcp(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G
    {
        ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); }
    } g;
    auto& terrain = TE::TerrainService::Get();

    // A synthetic near-flat planet-face slot (one low edge row gives the encode range a real
    // span so the in-place SetHeights path has headroom).
    const auto handle = terrain.AcquirePlanetFacePhysicsHandle();
    if (auto* data = terrain.ResolvePlanetFaceColliderForWrite(handle.Index, handle.Generation, kDim))
    {
        std::fill(data->Samples.begin(), data->Samples.end(), kFlatRaw);
        for (uint32 x = 0; x < kDim; ++x)
            data->Samples[x] = kLoweredRaw; // low row 0 -> range [4,10]
        terrain.CommitPlanetFaceFull(handle.Index, handle.Generation);
    }

    ECS::World world;
    auto e = world.CreateEntity();
    world.AddComponentImmediate<Components::Transform>(e, Components::Transform{}); // identity
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    PhysicsBody body{};
    body.motionType = MotionType::Static;
    body.gravityScale = 0.0f;
    world.AddComponentImmediate<PhysicsBody>(e, body);
    PhysicsCollider collider{};
    collider.layer = Layers::Static;
    world.AddComponentImmediate<PhysicsCollider>(e, collider);
    HeightFieldColliderShape shape{};
    shape.dataHandle = handle.Index; // includes kPlanetFacePhysicsHandleBit
    shape.dataGeneration = handle.Generation;
    shape.sizeX = kSize;
    shape.sizeZ = kSize;
    shape.heightScale = kScale;
    world.AddComponentImmediate<HeightFieldColliderShape>(e, shape);

    PhysicsInitSystem init;
    init.SetRebuildQuiescenceSeconds(0.0f);
    init.Update(world, kDt);
    ASSERT_TRUE(world.GetComponent<PhysicsBody>(e)->initialized);
    pw.OptimizeBroadphase();

    // Box asleep on the flat surface at raw * scale (centre of the grid, well inside the
    // flat region so it rests at kFlatRaw*kScale).
    BoxShapeDef boxDef{};
    boxDef.halfExtents = Vector3(0.5f, 0.5f, 0.5f);
    const ShapeHandle boxShape = pw.CreateShape(boxDef);
    BodySettings bs{};
    bs.shape = boxShape;
    bs.motionType = MotionType::Dynamic;
    bs.layer = Layers::Dynamic;
    bs.mass = 1.0f;
    bs.position = Vector3(0.0f, kFlatRaw * kScale + 0.5f, 0.0f);
    bs.startAwake = false;
    const BodyHandle box = pw.CreateBody(bs);
    for (int i = 0; i < 30; ++i)
        pw.Step(kDt, 1);
    ASSERT_NEAR(pw.GetBodyTransform(box).position.y, kFlatRaw * kScale + 0.5f, 0.1f)
        << "box must rest at raw*heightScale on the flat planet-face collider";

    // Lower a central region in place (through the planet-face discriminator) and wake.
    if (auto* data = terrain.ResolvePlanetFaceColliderForWrite(handle.Index, handle.Generation, kDim))
    {
        for (uint32 z = 5; z < 11; ++z)
            for (uint32 x = 5; x < 11; ++x)
                data->Samples[z * kDim + x] = kLoweredRaw;
        terrain.CommitPlanetFaceRegion(handle.Index, handle.Generation, 5, 5, 11, 11);
    }
    init.Update(world, kDt);

    for (int i = 0; i < 240; ++i)
        pw.Step(kDt, 1);
    const float yFinal = pw.GetBodyTransform(box).position.y;
    EXPECT_LT(yFinal, kFlatRaw * kScale - 3.0f)
        << "the box never woke after the in-place lower (ActivateBodiesInAABB missing?)";
    EXPECT_NEAR(yFinal, kLoweredRaw * kScale + 0.5f, 1.0f)
        << "box must settle on the lowered region at raw*heightScale";
}

namespace
{
// Provision a planet + build its face bodies; returns the world for further edits.
void ProvisionPlanet(GameEngine::ECS::World& world, GameEngine::TerrainECS::TerrainPhysicsSystem& physics,
                     GameEngine::PhysicsECS::PhysicsInitSystem& init, float radius, float relief)
{
    using namespace GameEngine;
    constexpr float32 kDt = 1.0f / 60.0f;
    CreatePlanetEntity(world, radius, relief);
    init.SetRebuildQuiescenceSeconds(0.0f);
    physics.Update(world, kDt);
    world.ProcessCommands();
    init.Update(world, kDt);
    world.ProcessCommands();
}

// Displacement magnitude of a body from `start` after stepping.
float Displacement(GameEngine::Physics::PhysicsWorld& pw, GameEngine::Physics::BodyHandle b,
                   const GameEngine::Vector3& start)
{
    const auto p = pw.GetBodyTransform(b).position;
    const float dx = p.x - start.x, dy = p.y - start.y, dz = p.z - start.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
} // namespace

// The wake AABB must follow the collider body's rotation. A sculpt raise at an OFF-CENTRE
// spot on the yawed +Y face wakes a body sitting there in WORLD space — pre-fix the wake box
// mapped grid X/Z straight to world X/Z (a 90 deg yaw on +Y), so the box (at world +Z) never
// woke while the AABB sat at world +X. Gravity is disabled so a woken body's motion is purely
// the in-place depenetration (no sphere-curvature slide confound).
TEST(Physics, PlanetColliderOffCentreSculptWakeFollowsBodyRotation)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kRadius = 2000.0f;
    constexpr float32 kRelief = 60.0f;
    constexpr uint32 kFace = 2u; // +Y

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G { ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); } } g;
    auto& terrain = TE::TerrainService::Get();

    ECS::World world;
    TE::TerrainPhysicsSystem physics;
    PhysicsInitSystem init;
    ProvisionPlanet(world, physics, init, kRadius, kRelief);
    pw.OptimizeBroadphase();

    // An off-centre +Y grid cell (biased in U -> world +Z; the axis the yaw error swaps).
    const uint32 ci = 93u, cj = 64u;
    const Vector3 surf = FaceSurfaceWorld(kFace, ci, cj, kRadius, kRelief);
    std::array<float, 3> ta, n, tb;
    TE::ComputePlanetFaceFrame(kFace, ta, n, tb);
    constexpr float kHalf = 2.0f;
    BoxShapeDef boxDef{};
    boxDef.halfExtents = Vector3(kHalf, kHalf, kHalf);
    const ShapeHandle boxShape = pw.CreateShape(boxDef);
    BodySettings bs{};
    bs.shape = boxShape;
    bs.motionType = MotionType::Dynamic;
    bs.layer = Layers::Dynamic;
    bs.mass = 1.0f;
    bs.gravityScale = 0.0f; // isolate the wake/depenetration from -Y gravity slide
    bs.position = Vector3(surf.x + n[0] * kHalf, surf.y + n[1] * kHalf, surf.z + n[2] * kHalf);
    bs.startAwake = false;
    const BodyHandle box = pw.CreateBody(bs);
    ASSERT_TRUE(box.IsValid());
    for (int i = 0; i < 60; ++i)
        pw.Step(kDt, 1);
    const Vector3 startPos = pw.GetBodyTransform(box).position;

    // Raise the band under the off-centre box with a sculpt dab at its surface direction
    // (in-place tier-1). The wake AABB must follow the +Y face yaw to reach the off-centre cell.
    terrain.SeedPlanetSculptMirrorForTests(surf.x, surf.y, surf.z, /*angularRadius*/ 0.15f,
                                           /*strength*/ 1.5f, /*lower*/ false);

    physics.Update(world, kDt);
    world.ProcessCommands();
    init.Update(world, kDt);
    world.ProcessCommands();
    for (int i = 0; i < 60; ++i)
        pw.Step(kDt, 1);

    EXPECT_GT(Displacement(pw, box, startPos), 0.15f)
        << "the off-centre body never woke — the wake AABB did not follow the +Y face yaw";
}

// Same wake correctness on a SIDE face (+X): pre-fix the wake box also mis-mapped the height
// axis (raw local heights ~1155-2060 used as world Y while the face lives at world +X), so a
// body resting on a side face never woke on any edit.
TEST(Physics, PlanetColliderSideFaceSculptWakes)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr float32 kRadius = 2000.0f;
    constexpr float32 kRelief = 60.0f;
    constexpr uint32 kFace = 0u; // +X

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G { ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); } } g;
    auto& terrain = TE::TerrainService::Get();

    ECS::World world;
    TE::TerrainPhysicsSystem physics;
    PhysicsInitSystem init;
    ProvisionPlanet(world, physics, init, kRadius, kRelief);
    pw.OptimizeBroadphase();

    const uint32 dim = TE::kPlanetFaceColliderDim;
    const uint32 mid = (dim - 1) / 2;
    const Vector3 surf = FaceSurfaceWorld(kFace, mid, mid, kRadius, kRelief);
    std::array<float, 3> ta, n, tb;
    TE::ComputePlanetFaceFrame(kFace, ta, n, tb);
    constexpr float kHalf = 2.0f;
    BoxShapeDef boxDef{};
    boxDef.halfExtents = Vector3(kHalf, kHalf, kHalf);
    const ShapeHandle boxShape = pw.CreateShape(boxDef);
    BodySettings bs{};
    bs.shape = boxShape;
    bs.motionType = MotionType::Dynamic;
    bs.layer = Layers::Dynamic;
    bs.mass = 1.0f;
    bs.gravityScale = 0.0f;
    bs.position = Vector3(surf.x + n[0] * kHalf, surf.y + n[1] * kHalf, surf.z + n[2] * kHalf);
    bs.startAwake = false;
    const BodyHandle box = pw.CreateBody(bs);
    ASSERT_TRUE(box.IsValid());
    for (int i = 0; i < 60; ++i)
        pw.Step(kDt, 1);
    const Vector3 startPos = pw.GetBodyTransform(box).position;

    // Raise the +X face centre under the box with a sculpt dab at its surface direction.
    terrain.SeedPlanetSculptMirrorForTests(surf.x, surf.y, surf.z, /*angularRadius*/ 0.15f,
                                           /*strength*/ 1.5f, /*lower*/ false);

    physics.Update(world, kDt);
    world.ProcessCommands();
    init.Update(world, kDt);
    world.ProcessCommands();
    for (int i = 0; i < 60; ++i)
        pw.Step(kDt, 1);

    EXPECT_GT(Displacement(pw, box, startPos), 0.15f)
        << "a body on the +X face never woke — the wake AABB ignored the face's height axis";
}

// An edge-straddling dab writes BOTH adjacent faces' atlas bands; both colliders must refresh
// (the #488 crack-free contract). Pre-fix only the primary face's collider advanced.
TEST(Physics, PlanetColliderEdgeStraddleRefreshesBothFaces)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G { ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); } } g;
    auto& terrain = TE::TerrainService::Get();

    ECS::World world;
    TE::TerrainPhysicsSystem physics;
    PhysicsInitSystem init;
    ProvisionPlanet(world, physics, init, 2000.0f, 60.0f);

    uint64 builtBefore[TE::kPlanetFaceCount] = {};
    world.Query<ECS::Read<GameEngine::Components::TerrainPlanetFaceCollider>,
                ECS::Read<HeightFieldColliderShape>>()
        .Each([&](ECS::EntityHandle, const GameEngine::Components::TerrainPlanetFaceCollider& tag,
                  const HeightFieldColliderShape& s)
              { if (tag.Face < TE::kPlanetFaceCount) builtBefore[tag.Face] = s.lastBuiltVersion; });

    // A dab centred exactly on the +Y (2) / +Z (4) cube edge (direction (0,1,1)): both bands are
    // written near the shared edge, so both faces' colliders must refresh (#488 crack-free).
    terrain.SeedPlanetSculptMirrorForTests(0.0f, 1.0f, 1.0f, /*angularRadius*/ 0.12f,
                                           /*strength*/ 1.5f, /*lower*/ false);

    physics.Update(world, kDt);
    world.ProcessCommands();
    init.Update(world, kDt);
    world.ProcessCommands();

    uint64 builtAfter[TE::kPlanetFaceCount] = {};
    world.Query<ECS::Read<GameEngine::Components::TerrainPlanetFaceCollider>,
                ECS::Read<HeightFieldColliderShape>>()
        .Each([&](ECS::EntityHandle, const GameEngine::Components::TerrainPlanetFaceCollider& tag,
                  const HeightFieldColliderShape& s)
              { if (tag.Face < TE::kPlanetFaceCount) builtAfter[tag.Face] = s.lastBuiltVersion; });

    for (uint32 f = 0; f < TE::kPlanetFaceCount; ++f)
    {
        if (f == 2u || f == 4u)
            EXPECT_GT(builtAfter[f], builtBefore[f])
                << "both straddled faces must refresh, face " << f;
        else
            EXPECT_EQ(builtAfter[f], builtBefore[f]) << "untouched face " << f << " must not refresh";
    }
}

// World-space yaw pin: each face collider entity's WorldTransform columns must equal
// ComputePlanetFaceFrame (Ta, N, Tb). A transposed FillFaceMatrix packing would swap rows and
// columns yet still pass the frame/height oracles (which never look at the entity transform).
TEST(Physics, PlanetColliderFaceEntityTransformMatchesFrame)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G { ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); } } g;

    ECS::World world;
    TE::TerrainPhysicsSystem physics;
    PhysicsInitSystem init;
    CreatePlanetEntity(world, 2000.0f, 60.0f);
    physics.Update(world, kDt);
    world.ProcessCommands();

    bool sawFace[TE::kPlanetFaceCount] = {};
    world.Query<ECS::Read<GameEngine::Components::TerrainPlanetFaceCollider>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle, const GameEngine::Components::TerrainPlanetFaceCollider& tag,
                  const Components::WorldTransform& wt)
        {
            ASSERT_LT(tag.Face, TE::kPlanetFaceCount);
            sawFace[tag.Face] = true;
            std::array<float, 3> ta, n, tb;
            TE::ComputePlanetFaceFrame(tag.Face, ta, n, tb);
            // Column-major: col0 = Ta, col1 = N, col2 = Tb.
            for (int k = 0; k < 3; ++k)
            {
                EXPECT_NEAR(wt.matrix[0 + k], ta[k], 1e-5f) << "face " << tag.Face << " Ta " << k;
                EXPECT_NEAR(wt.matrix[4 + k], n[k], 1e-5f) << "face " << tag.Face << " N " << k;
                EXPECT_NEAR(wt.matrix[8 + k], tb[k], 1e-5f) << "face " << tag.Face << " Tb " << k;
            }
        });
    for (uint32 f = 0; f < TE::kPlanetFaceCount; ++f)
        EXPECT_TRUE(sawFace[f]) << "missing face collider " << f;
}

// DERIVED-WHEN-CHANGED oracle for the planet's base relief. The Inspector's "Planet Relief"
// Amplitude dial moves the rendered surface immediately; the six face colliders are derived
// data and must follow in the same tick. The pre-existing height-match oracle
// (PlanetColliderTests HeightMatchesRenderFunctionAtGridPoints) compares a FRESHLY generated
// patch against the render function, so it proves agreement-at-generation-time and cannot fail
// on staleness — this one mutates relief on a settled planet, runs one whole tick in schedule
// order (TerrainModifiers then TerrainPhysics), and requires the STORED collider to have moved.
//
// Failure mode it pins: collision keeps the OLD relief across the entire planet, so a character
// walks on invisible ground where the new relief is lower and falls through where it is higher.
TEST(Physics, PlanetReliefEditReCooksEveryFaceCollider)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::TerrainPlanetFaceCollider;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;

    constexpr float32 kRadius = 2000.0f;
    constexpr float32 kReliefBefore = 60.0f;
    constexpr float32 kReliefAfter = 300.0f;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G { ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); } } g;
    auto& terrain = TE::TerrainService::Get();

    ECS::World world;
    const ECS::EntityHandle planet = CreatePlanetEntity(world, kRadius, kReliefBefore);

    // A whole engine tick in schedule order: TerrainModifiers, then TerrainPhysics
    // (RegisterTerrainSystems declares TerrainPhysics after TerrainModifiers).
    TE::TerrainModifierSystem modifiers;
    TE::TerrainPhysicsSystem physics;
    auto tick = [&]() {
        modifiers.Update(world, kDt);
        physics.Update(world, kDt);
        world.ProcessCommands();
    };

    tick();                                            // provisions the six faces
    ASSERT_EQ(CountPlanetFaceColliders(world), 6);
    tick();                                            // settle: reach quiescence

    struct FaceRef { uint32 Face, HandleIndex, HandleGen; };
    std::vector<FaceRef> faces;
    world.Query<ECS::Read<TerrainPlanetFaceCollider>>()
        .Each([&](ECS::EntityHandle, const TerrainPlanetFaceCollider& tag) {
            faces.push_back(FaceRef{tag.Face, tag.PhysicsHandleIndex, tag.PhysicsHandleGeneration});
        });
    ASSERT_EQ(faces.size(), size_t{TE::kPlanetFaceCount});

    std::vector<uint64> versionBefore(faces.size(), 0);
    for (size_t f = 0; f < faces.size(); ++f)
    {
        const auto* data = terrain.ResolvePlanetFaceCollider(faces[f].HandleIndex, faces[f].HandleGen);
        ASSERT_NE(data, nullptr) << "face " << faces[f].Face << " has no baked collider data";
        versionBefore[f] = data->Version;
    }

    // A quiescent planet must stay quiescent: an idle tick may not churn the colliders,
    // otherwise "the version moved" below would be satisfied by noise rather than by the edit.
    tick();
    for (size_t f = 0; f < faces.size(); ++f)
    {
        const auto* data = terrain.ResolvePlanetFaceCollider(faces[f].HandleIndex, faces[f].HandleGen);
        ASSERT_NE(data, nullptr);
        ASSERT_EQ(data->Version, versionBefore[f])
            << "face " << faces[f].Face << " re-cooked on an idle tick — the oracle below is not discriminating";
    }

    // The edit: drag Amplitude in the Planet Relief inspector section.
    {
        auto* relief = world.GetComponentForWrite<Components::TerrainPlanetRelief>(planet);
        ASSERT_NE(relief, nullptr);
        relief->Amplitude = kReliefAfter;
    }

    tick(); // ONE whole engine tick

    // The reference patch the collider must now hold: the same generator, at the NEW relief.
    TE::PlanetColliderParams expected{};
    expected.Radius = kRadius;
    expected.ReliefAmplitude = kReliefAfter;
    expected.ReliefFrequency = 6.0f;
    expected.ReliefOctaves = 3u; // CreatePlanetEntity's relief octaves
    const uint32 dim = TE::kPlanetFaceColliderDim;
    const int32 last = static_cast<int32>(dim) - 1;

    float32 largestMove = 0.0f;
    for (size_t f = 0; f < faces.size(); ++f)
    {
        const uint32 face = faces[f].Face;
        const auto* data = terrain.ResolvePlanetFaceCollider(faces[f].HandleIndex, faces[f].HandleGen);
        ASSERT_NE(data, nullptr) << "face " << face;
        EXPECT_GT(data->Version, versionBefore[f])
            << "face " << face << " collider was never re-cooked after the relief edit — "
               "physics is still colliding against the old relief";

        std::vector<float32> reference(static_cast<size_t>(dim) * dim, 0.0f);
        float32 lo = 0.0f, hi = 0.0f;
        TE::GeneratePlanetFacePatch(face, expected, dim, CBTTerrain::SphereSculptSampler{},
                                    CBTTerrain::SphereAnalyticModifierSet{}, 0, 0, last, last,
                                    reference, lo, hi);

        // Probe the patch centre and four off-centre cells; the face-axis direction can sit on a
        // node of the separable noise, so an off-centre probe is what carries the discrimination.
        const int32 mid = last / 2, q = last / 4;
        const std::array<std::array<int32, 2>, 5> probes = {{
            {mid, mid}, {q, mid}, {mid, q}, {last - q, mid}, {mid, last - q}}};
        for (const auto& probe : probes)
        {
            const size_t idx = static_cast<size_t>(probe[1]) * dim + probe[0];
            ASSERT_LT(idx, data->Samples.size());
            EXPECT_NEAR(data->Samples[idx], reference[idx], 0.05f)
                << "face " << face << " cell (" << probe[0] << "," << probe[1]
                << ") does not match the new relief — the collider height field is stale";
            largestMove = std::max(largestMove, std::abs(reference[idx] - data->Samples[idx]));
        }
    }

    // Discrimination: amplitude 60 -> 300 must genuinely move the surface, otherwise the
    // equality assertions above would pass against an unchanged patch.
    TE::PlanetColliderParams before = expected;
    before.ReliefAmplitude = kReliefBefore;
    float32 reliefDelta = 0.0f;
    for (uint32 face = 0; face < TE::kPlanetFaceCount; ++face)
    {
        std::vector<float32> a(static_cast<size_t>(dim) * dim, 0.0f), b = a;
        float32 lo = 0.0f, hi = 0.0f;
        TE::GeneratePlanetFacePatch(face, before, dim, CBTTerrain::SphereSculptSampler{},
                                    CBTTerrain::SphereAnalyticModifierSet{}, 0, 0, last, last, a, lo, hi);
        TE::GeneratePlanetFacePatch(face, expected, dim, CBTTerrain::SphereSculptSampler{},
                                    CBTTerrain::SphereAnalyticModifierSet{}, 0, 0, last, last, b, lo, hi);
        for (size_t i = 0; i < a.size(); ++i)
            reliefDelta = std::max(reliefDelta, std::abs(a[i] - b[i]));
    }
    ASSERT_GT(reliefDelta, 10.0f)
        << "test not discriminating: the relief change barely moves the surface";
}

// The user-visible half of the same defect, at the Jolt seam: a body resting on the planet must
// not be left standing on ground that no longer exists. Raising Amplitude LOWERS the +Y pole (the
// base noise is negative along that axis and relief is linear in amplitude), so a box asleep on
// the pole is suddenly unsupported. With a stale collider it keeps floating at the old height —
// "walks on invisible ground". This asserts the whole chain fired: every face's Jolt shape was
// rebuilt (HeightFieldColliderShape::lastBuiltVersion) and the box fell to the NEW surface.
TEST(Physics, PlanetReliefEditDropsBodyLeftOnVanishedGround)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::TerrainPlanetFaceCollider;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;

    constexpr float32 kRadius = 2000.0f;
    constexpr float32 kReliefBefore = 60.0f;
    constexpr float32 kReliefAfter = 200.0f; // raises amplitude => LOWERS the +Y pole

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G { ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); } } g;

    ECS::World world;
    const ECS::EntityHandle planet = CreatePlanetEntity(world, kRadius, kReliefBefore);

    TE::TerrainModifierSystem modifiers;
    TE::TerrainPhysicsSystem physics;
    PhysicsInitSystem init;
    init.SetRebuildQuiescenceSeconds(0.0f);
    auto tick = [&]() {
        modifiers.Update(world, kDt);
        physics.Update(world, kDt);
        world.ProcessCommands();
        init.Update(world, kDt);
        world.ProcessCommands();
    };
    tick(); // provision the six faces and build their Jolt shapes
    ASSERT_EQ(CountPlanetFaceColliders(world), 6);

    const float poleBefore = PoleSurfaceHeight(kRadius, kReliefBefore);
    const float poleAfter = PoleSurfaceHeight(kRadius, kReliefAfter);
    // Precondition on the chosen amplitudes: the pole must genuinely drop, and by more than the
    // box, so "still floating" and "fell to the new surface" cannot be confused.
    constexpr float kHalf = 2.0f;
    ASSERT_LT(poleAfter, poleBefore - 10.0f)
        << "amplitude choice does not lower the pole (before " << poleBefore << " after "
        << poleAfter << ") — flip kReliefAfter";

    // Sleep a box on the OLD pole surface.
    BoxShapeDef boxDef{};
    boxDef.halfExtents = Vector3(kHalf, kHalf, kHalf);
    BodySettings bs{};
    bs.shape = pw.CreateShape(boxDef);
    bs.motionType = MotionType::Dynamic;
    bs.layer = Layers::Dynamic;
    bs.mass = 1.0f;
    bs.position = Vector3(0.0f, poleBefore + kHalf, 0.0f);
    bs.startAwake = false;
    const BodyHandle box = pw.CreateBody(bs);
    ASSERT_TRUE(box.IsValid());
    pw.OptimizeBroadphase();
    for (int i = 0; i < 60; ++i)
        pw.Step(kDt, 1);
    const float ySlept = pw.GetBodyTransform(box).position.y;
    ASSERT_NEAR(ySlept, poleBefore + kHalf, 0.2f) << "box must be asleep on the pole before the edit";

    uint64 builtBefore[TE::kPlanetFaceCount] = {};
    world.Query<ECS::Read<TerrainPlanetFaceCollider>, ECS::Read<HeightFieldColliderShape>>()
        .Each([&](ECS::EntityHandle, const TerrainPlanetFaceCollider& tag,
                  const HeightFieldColliderShape& s)
              { if (tag.Face < TE::kPlanetFaceCount) builtBefore[tag.Face] = s.lastBuiltVersion; });

    // The edit: drag Amplitude in the Planet Relief inspector section.
    {
        auto* relief = world.GetComponentForWrite<Components::TerrainPlanetRelief>(planet);
        ASSERT_NE(relief, nullptr);
        relief->Amplitude = kReliefAfter;
    }

    tick(); // ONE whole engine tick: re-cook the patches, then rebuild the Jolt shapes

    // Every face's Jolt shape was rebuilt — a relief change is global, so no face is exempt.
    uint64 builtAfter[TE::kPlanetFaceCount] = {};
    world.Query<ECS::Read<TerrainPlanetFaceCollider>, ECS::Read<HeightFieldColliderShape>>()
        .Each([&](ECS::EntityHandle, const TerrainPlanetFaceCollider& tag,
                  const HeightFieldColliderShape& s)
              { if (tag.Face < TE::kPlanetFaceCount) builtAfter[tag.Face] = s.lastBuiltVersion; });
    for (uint32 f = 0; f < TE::kPlanetFaceCount; ++f)
        EXPECT_GT(builtAfter[f], builtBefore[f])
            << "face " << f << " Jolt shape was never rebuilt after the relief edit";

    // The box wakes and falls: the ground it was resting on is gone. The drop is ~25 m, which
    // takes ~2.2 s, so sample just after touchdown — under plain -Y gravity a body only rests
    // indefinitely at the exact pole, and from there it slides down the curvature, so the
    // ENDURING height is not the signal. Landing is.
    constexpr int kStepsToLanding = 170; // ~2.8 s: touchdown plus a moment to settle
    float lowestY = ySlept;
    for (int i = 0; i < kStepsToLanding; ++i)
    {
        pw.Step(kDt, 1);
        lowestY = std::min(lowestY, pw.GetBodyTransform(box).position.y);
    }
    const auto landed = pw.GetBodyTransform(box).position;

    EXPECT_LT(landed.y, ySlept - 10.0f)
        << "the box never fell: it is still standing on the old relief (invisible ground)";
    EXPECT_GT(lowestY, poleAfter - 2.0f)
        << "the box fell through the new surface instead of landing on it";
    // Still essentially over the pole at this point, so the pole sample is the right reference.
    ASSERT_LT(std::abs(landed.x), 8.0f) << "slid too far to compare against the pole height";
    EXPECT_NEAR(landed.y, poleAfter + kHalf, 1.5f)
        << "the box did not land on the NEW pole surface";
}

// A planet radius edit re-cooks every face patch AT the new radius, and the radius is what
// parametrizes the grid: PhysicsInitSystem places sample (i, j) at local
// (-sizeX/2 + i*sizeX/(dim-1), height, -sizeZ/2 + j*sizeZ/(dim-1)), and the generator bakes the
// height for the direction that same radius puts that cell at. Extent and samples are therefore
// ONE derivation — moving the heights while sizeX/sizeZ still describe the old radius stretches
// the new surface over the old grid, and every off-centre cell resolves to the wrong world point.
// This asserts the declared extent tracks the radius, and that the world surface the
// shape+samples pair describes is the surface the new radius actually has.
TEST(Physics, PlanetRadiusEditUpdatesFaceColliderExtent)
{
    using namespace GameEngine;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::TerrainPlanetFaceCollider;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;

    constexpr float32 kRadiusBefore = 2000.0f;
    constexpr float32 kRadiusAfter = 2600.0f;
    constexpr float32 kRelief = 60.0f;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G { ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); } } g;
    auto& terrain = TE::TerrainService::Get();

    ECS::World world;
    const ECS::EntityHandle planet = CreatePlanetEntity(world, kRadiusBefore, kRelief);

    // A whole engine tick in schedule order: TerrainModifiers, then TerrainPhysics.
    TE::TerrainModifierSystem modifiers;
    TE::TerrainPhysicsSystem physics;
    auto tick = [&]() {
        modifiers.Update(world, kDt);
        physics.Update(world, kDt);
        world.ProcessCommands();
    };

    tick();                                            // provisions the six faces
    ASSERT_EQ(CountPlanetFaceColliders(world), 6);
    tick();                                            // settle: the shape baseline is seeded

    struct FaceRef { ECS::EntityHandle Entity; uint32 Face, HandleIndex, HandleGen; };
    std::vector<FaceRef> faces;
    world.Query<ECS::Read<TerrainPlanetFaceCollider>, ECS::Read<HeightFieldColliderShape>>()
        .Each([&](ECS::EntityHandle e, const TerrainPlanetFaceCollider& tag,
                  const HeightFieldColliderShape& shape) {
            faces.push_back(FaceRef{e, tag.Face, tag.PhysicsHandleIndex, tag.PhysicsHandleGeneration});
            EXPECT_FLOAT_EQ(shape.sizeX, kRadiusBefore * 2.0f)
                << "face " << tag.Face << " was not provisioned at the starting radius";
        });
    ASSERT_EQ(faces.size(), size_t{TE::kPlanetFaceCount});

    // The edit: drag Planet Radius in the terrain inspector.
    {
        auto* t = world.GetComponentForWrite<Components::Terrain>(planet);
        ASSERT_NE(t, nullptr);
        t->PlanetRadius = kRadiusAfter;
    }

    tick(); // ONE whole engine tick

    const uint32 dim = TE::kPlanetFaceColliderDim;

    // The local->world mapping PhysicsInitSystem hands Jolt: Offset = -size/2, Scale = size/(N-1).
    auto surfaceWorldFromShape = [&](uint32 face, const std::vector<float32>& samples,
                                     float32 sizeX, float32 sizeZ, uint32 i, uint32 j) {
        std::array<float32, 3> ta, n, tb;
        TE::ComputePlanetFaceFrame(face, ta, n, tb);
        const float32 lx = -sizeX * 0.5f +
                           static_cast<float32>(i) * (sizeX / static_cast<float32>(dim - 1));
        const float32 lz = -sizeZ * 0.5f +
                           static_cast<float32>(j) * (sizeZ / static_cast<float32>(dim - 1));
        const float32 ly = samples[static_cast<size_t>(j) * dim + i];
        return Vector3(ta[0] * lx + n[0] * ly + tb[0] * lz, ta[1] * lx + n[1] * ly + tb[1] * lz,
                       ta[2] * lx + n[2] * ly + tb[2] * lz);
    };

    // Off-centre: the extent only moves cells away from the patch centre, so a centre probe
    // cannot see this defect at all.
    const uint32 probeI = dim / 4, probeJ = dim / 2;

    for (const FaceRef& ref : faces)
    {
        const auto* shape = world.GetComponent<HeightFieldColliderShape>(ref.Entity);
        ASSERT_NE(shape, nullptr) << "face " << ref.Face;
        EXPECT_FLOAT_EQ(shape->sizeX, kRadiusAfter * 2.0f)
            << "face " << ref.Face << " still declares the OLD grid extent after a radius edit — "
               "its re-cooked heights describe a surface the shape does not span";
        EXPECT_FLOAT_EQ(shape->sizeZ, kRadiusAfter * 2.0f) << "face " << ref.Face;

        const auto* data = terrain.ResolvePlanetFaceCollider(ref.HandleIndex, ref.HandleGen);
        ASSERT_NE(data, nullptr) << "face " << ref.Face;
        const Vector3 actual = surfaceWorldFromShape(ref.Face, data->Samples, shape->sizeX,
                                                     shape->sizeZ, probeI, probeJ);
        const Vector3 expected = FaceSurfaceWorld(ref.Face, probeI, probeJ, kRadiusAfter, kRelief);
        const float32 err = std::sqrt((actual.x - expected.x) * (actual.x - expected.x) +
                                      (actual.y - expected.y) * (actual.y - expected.y) +
                                      (actual.z - expected.z) * (actual.z - expected.z));
        EXPECT_LT(err, 0.5f)
            << "face " << ref.Face << " collider surface at cell (" << probeI << "," << probeJ
            << ") is " << err << " m from the surface the new radius has";
    }

    // Discrimination: with the OLD extent the same samples resolve somewhere materially different,
    // so the comparison above could have failed.
    {
        const auto* data = terrain.ResolvePlanetFaceCollider(faces[0].HandleIndex, faces[0].HandleGen);
        ASSERT_NE(data, nullptr);
        const Vector3 stale = surfaceWorldFromShape(faces[0].Face, data->Samples,
                                                    kRadiusBefore * 2.0f, kRadiusBefore * 2.0f,
                                                    probeI, probeJ);
        const Vector3 expected = FaceSurfaceWorld(faces[0].Face, probeI, probeJ, kRadiusAfter, kRelief);
        const float32 staleErr = std::sqrt((stale.x - expected.x) * (stale.x - expected.x) +
                                           (stale.y - expected.y) * (stale.y - expected.y) +
                                           (stale.z - expected.z) * (stale.z - expected.z));
        ASSERT_GT(staleErr, 100.0f)
            << "test not discriminating: the old extent puts this cell in nearly the same place";
    }
}

// The Jolt-seam half of the radius edit: the declared extent only matters because
// PhysicsInitSystem bakes it into the built shape's Offset/Scale, so the collision surface a
// ray hits must move with the radius AWAY from the patch centre, not just at the pole. The
// probe column is chosen to land exactly on grid node (i=64, j=48) of the +Y face under the
// NEW extent; under the stale extent the same world column falls at j=43.2 of the same
// samples, ~50 m lower.
TEST(Physics, PlanetRadiusEditMovesJoltSurfaceOffCentre)
{
    using namespace GameEngine;
    using namespace GameEngine::Physics;
    using namespace GameEngine::PhysicsECS;
    using GameEngine::Components::HeightFieldColliderShape;
    using GameEngine::Components::TerrainPlanetFaceCollider;
    namespace TE = GameEngine::TerrainECS;
    constexpr float32 kDt = 1.0f / 60.0f;

    constexpr float32 kRadiusBefore = 2000.0f;
    constexpr float32 kRadiusAfter = 2600.0f;
    constexpr float32 kProbeX = 650.0f; // world +X offset; +Y face local z = -kProbeX

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();
    if (TE::TerrainService::IsInitialized())
        TE::TerrainService::Shutdown();
    TE::TerrainService::Initialize();
    struct G { ~G() { if (TE::TerrainService::IsInitialized()) TE::TerrainService::Shutdown(); } } g;

    ECS::World world;
    const ECS::EntityHandle planet = CreatePlanetEntity(world, kRadiusBefore, 0.0f);

    TE::TerrainModifierSystem modifiers;
    TE::TerrainPhysicsSystem physics;
    PhysicsInitSystem init;
    init.SetRebuildQuiescenceSeconds(0.0f);
    auto tick = [&]() {
        modifiers.Update(world, kDt);
        physics.Update(world, kDt);
        world.ProcessCommands();
        init.Update(world, kDt);
        world.ProcessCommands();
    };

    tick();
    ASSERT_EQ(CountPlanetFaceColliders(world), 6);
    tick();
    pw.OptimizeBroadphase();

    auto probe = [&]() -> float32 {
        RayCastQuery q{};
        q.ray = Ray3D(Vector3(kProbeX, 5000.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
        q.maxDistance = 4000.0f;
        RayCastResult hit{};
        if (!pw.RayCast(q, hit))
            return -1.0f;
        return hit.hitPoint.y;
    };

    const float32 yBefore = probe();
    ASSERT_GT(yBefore, 0.0f) << "probe ray never hit the planet before the edit";

    uint64 builtBefore[TE::kPlanetFaceCount] = {};
    world.Query<ECS::Read<TerrainPlanetFaceCollider>, ECS::Read<HeightFieldColliderShape>>()
        .Each([&](ECS::EntityHandle, const TerrainPlanetFaceCollider& tag,
                  const HeightFieldColliderShape& s)
              { if (tag.Face < TE::kPlanetFaceCount) builtBefore[tag.Face] = s.lastBuiltVersion; });

    {
        auto* t = world.GetComponentForWrite<Components::Terrain>(planet);
        ASSERT_NE(t, nullptr);
        t->PlanetRadius = kRadiusAfter;
    }

    tick();
    pw.OptimizeBroadphase();

    uint64 builtAfter[TE::kPlanetFaceCount] = {};
    world.Query<ECS::Read<TerrainPlanetFaceCollider>, ECS::Read<HeightFieldColliderShape>>()
        .Each([&](ECS::EntityHandle, const TerrainPlanetFaceCollider& tag,
                  const HeightFieldColliderShape& s)
              { if (tag.Face < TE::kPlanetFaceCount) builtAfter[tag.Face] = s.lastBuiltVersion; });
    for (uint32 f = 0; f < TE::kPlanetFaceCount; ++f)
        EXPECT_GT(builtAfter[f], builtBefore[f]) << "face " << f << " Jolt shape never rebuilt";

    // Both predictions come from the SAME stored samples; only the declared extent differs.
    const uint32 dim = TE::kPlanetFaceColliderDim;
    std::vector<float32> patch(static_cast<size_t>(dim) * dim, 0.0f);
    {
        TE::PlanetColliderParams p{};
        p.Radius = kRadiusAfter;
        p.ReliefAmplitude = 0.0f; // matches CreatePlanetEntity(..., 0.0f)
        p.ReliefFrequency = 6.0f;
        p.ReliefOctaves = 3u;
        float32 lo = 0.0f, hi = 0.0f;
        TE::GeneratePlanetFacePatch(2 /*+Y*/, p, dim, CBTTerrain::SphereSculptSampler{},
                                    CBTTerrain::SphereAnalyticModifierSet{}, 0, 0,
                                    static_cast<int32>(dim) - 1, static_cast<int32>(dim) - 1, patch,
                                    lo, hi);
    }
    auto sampleAtColumn = [&](float32 sizeMeters) {
        const float32 scale = sizeMeters / static_cast<float32>(dim - 1);
        const float32 fi = (0.0f + sizeMeters * 0.5f) / scale;       // local x = world z = 0
        const float32 fj = (-kProbeX + sizeMeters * 0.5f) / scale;   // local z = -world x
        const uint32 i = static_cast<uint32>(fi);
        const uint32 j0 = static_cast<uint32>(std::floor(fj));
        const uint32 j1 = std::min(j0 + 1u, dim - 1u);
        const float32 t = fj - static_cast<float32>(j0);
        const float32 a = patch[static_cast<size_t>(j0) * dim + i];
        const float32 b = patch[static_cast<size_t>(j1) * dim + i];
        return a + (b - a) * t;
    };
    const float32 yExpected = sampleAtColumn(kRadiusAfter * 2.0f);
    const float32 yStaleExtent = sampleAtColumn(kRadiusBefore * 2.0f);
    ASSERT_GT(std::abs(yExpected - yStaleExtent), 20.0f)
        << "probe column not discriminating between the two extents";

    const float32 yAfter = probe();
    ASSERT_GT(yAfter, 0.0f) << "probe ray never hit the planet after the edit";
    EXPECT_NEAR(yAfter, yExpected, 1.0f)
        << "the Jolt surface off-centre is not where the new radius puts it (stale-extent "
           "prediction is " << yStaleExtent << ", got " << yAfter << ")";
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
    settings.position = Vector3(0.0f, 5.0f, 0.0f);
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
