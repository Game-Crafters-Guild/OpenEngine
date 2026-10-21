// A physics scene for the managed binding tests (Managed/BindingTests), which call the generated
// Physics binding through the real GameEngine.Native in a dotnet test host. That host has no frame
// loop and no scene file, so these exports build what play mode would: box bodies whose user data is
// their entity, entities with a PhysicsBody that names them, and the steps. Close does what leaving
// play mode does to physics: it shuts the physics world down.
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "Physics/PhysicsWorld.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/PhysicsEntityEventCallbacks.h"
#include "PhysicsECS/PhysicsWorldService.h"

#include <cstdint>

#if defined(_WIN32)
#define GE_TEST_SCENE_EXPORT extern "C" __declspec(dllexport)
#else
#define GE_TEST_SCENE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

using namespace GameEngine;

namespace
{

// The scene world: the engine's primary world, which the generated Physics thunks read.
ECS::World* SceneWorld()
{
    EngineCore& engine = EngineCore::GetInstance();
    return engine.IsInitialized() ? engine.EnsurePrimaryWorld() : nullptr;
}

} // namespace

/// Starts the physics world, as entering play mode does. 0 on success, -1 before the engine runs (a
/// generated call starts it).
GE_TEST_SCENE_EXPORT int GE_TestScene_Open()
{
    if (SceneWorld() == nullptr)
        return -1;
    Physics::PhysicsWorldSettings settings{};
    settings.numThreads = 1;
    PhysicsECS::PhysicsWorldService::Initialize(settings);
    PhysicsECS::PhysicsEntityEventCallbacks::NotifyWorldStateChanged();
    return 0;
}

/// Adds a box of the given half extents at a position, with its entity in the scene world. `kind` is 0
/// for a static box, 1 for a dynamic one, 2 for a static trigger. 0 on success.
GE_TEST_SCENE_EXPORT int GE_TestScene_AddBox(float x, float y, float z, float halfX, float halfY, float halfZ, int kind,
                                             std::uint32_t* outEntity)
{
    ECS::World* world = SceneWorld();
    Physics::PhysicsWorld* physics = PhysicsECS::PhysicsWorldService::TryGet();
    if (world == nullptr || physics == nullptr || outEntity == nullptr)
        return -1;

    const ECS::EntityHandle entity = world->CreateEntity();
    Physics::BoxShapeDef box{};
    box.halfExtents = Physics::Vector3(halfX, halfY, halfZ);
    Physics::BodySettings settings{};
    settings.shape = physics->CreateShape(box);
    settings.position = Physics::Vector3(x, y, z);
    const bool dynamic = kind == 1;
    settings.motionType = dynamic ? Physics::MotionType::Dynamic : Physics::MotionType::Static;
    settings.layer = dynamic ? Physics::Layers::Dynamic : Physics::Layers::Static;
    settings.isSensor = kind == 2;
    settings.restitution = 0.0f;
    settings.userData = entity.id;
    const Physics::BodyHandle body = physics->CreateBody(settings);
    if (!body.IsValid())
        return -1;

    Components::PhysicsBody component{};
    component.motionType = settings.motionType;
    component.shape = settings.shape;
    component.body = body;
    component.initialized = true;
    world->AddComponentImmediate(entity, component);
    *outEntity = entity.id;
    return 0;
}

/// Runs `steps` physics steps of 1/60 s, which deliver the events. 0 on success.
GE_TEST_SCENE_EXPORT int GE_TestScene_Step(int steps)
{
    Physics::PhysicsWorld* physics = PhysicsECS::PhysicsWorldService::TryGet();
    if (physics == nullptr)
        return -1;
    for (int i = 0; i < steps; ++i)
        physics->Step(1.0f / 60.0f, 1);
    return 0;
}

/// Shuts the physics world down, as leaving play mode does, which ends every event subscription.
GE_TEST_SCENE_EXPORT int GE_TestScene_Close()
{
    if (PhysicsECS::PhysicsWorldService::IsInitialized())
        PhysicsECS::PhysicsWorldService::Shutdown();
    return 0;
}
