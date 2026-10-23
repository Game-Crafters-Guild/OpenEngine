// A physics scene for the managed binding tests (Managed/BindingTests), which call the generated
// Physics binding through the real GameEngine.Native in a dotnet test host. That host has no frame
// loop and no scene file, so these exports build what play mode would: box bodies whose user data is
// their entity, entities with a PhysicsBody that names them, and the steps. Close does what leaving
// play mode does to physics: it shuts the physics world down. LoadRenderLayerNames reads a project's
// render layer names, as the editor's project open does. The navigation exports make a surface entity
// and read back what the generated Navigation calls asked of it, bake a grid surface over level ground,
// place an agent on it and run the frames that solve its path.
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/RenderLayerNames.h"
#include "Physics/PhysicsWorld.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/PhysicsEntityEventCallbacks.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "Components/Transform.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

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

/// Makes the render layer names of the project at `projectRoot` (UTF-8; its .Editor/ProjectSettings.json) the names
/// a script finds, as the editor's project open does. An empty root clears them. 0 on success.
GE_TEST_SCENE_EXPORT int GE_TestScene_LoadRenderLayerNames(const char* projectRoot)
{
    if (projectRoot == nullptr)
        return -1;
    const std::u8string root(reinterpret_cast<const char8_t*>(projectRoot));
    Rendering::SetRenderLayerNames(Rendering::RenderLayerNames::Load(std::filesystem::path(root)));
    return 0;
}

/// Adds an entity to the scene world with a NavigationSurface in Mesh mode (`kind` 0), in Grid mode
/// (1), or with none (2), and starts the navigation service a surface needs. 0 on success.
GE_TEST_SCENE_EXPORT int GE_TestScene_AddNavigationSurface(int kind, std::uint32_t* outEntity)
{
    ECS::World* world = SceneWorld();
    if (world == nullptr || outEntity == nullptr)
        return -1;
    if (!PathfindingECS::NavigationService::IsInitialized())
        PathfindingECS::NavigationService::Initialize();
    const ECS::EntityHandle entity = world->CreateEntity();
    if (kind != 2)
    {
        Components::NavigationSurface surface{};
        surface.Mode = kind == 1 ? Components::NavigationSurfaceMode::Grid : Components::NavigationSurfaceMode::Mesh;
        world->AddComponentImmediate(entity, surface);
    }
    *outEntity = entity.id;
    return 0;
}

/// Whether a bake was asked of the surface on `entity`: 1 or 0, -1 when it has none.
GE_TEST_SCENE_EXPORT int GE_TestScene_NavigationBakeRequested(std::uint32_t entity)
{
    ECS::World* world = SceneWorld();
    const auto* surface =
        world ? world->GetComponent<Components::NavigationSurface>(ECS::EntityHandle(entity)) : nullptr;
    return surface ? (surface->BakeRequested ? 1 : 0) : -1;
}

/// Takes the boxes MarkDirty queued for the scene world and writes the last one queued for `entity` as
/// min X, min Z, max X, max Z. The number of boxes queued for it.
GE_TEST_SCENE_EXPORT int GE_TestScene_TakeNavigationDirtyBoxes(std::uint32_t entity, float* outBox)
{
    ECS::World* world = SceneWorld();
    if (world == nullptr || outBox == nullptr)
        return -1;
    std::vector<std::pair<ECS::EntityHandle, Mathematics::Rect>> boxes;
    PathfindingECS::NavigationService::TakeDirtyBoxes(world->GetWorldId(), boxes);
    int count = 0;
    for (const auto& [surface, box] : boxes)
    {
        if (surface.id != entity)
            continue;
        ++count;
        outBox[0] = box.X;
        outBox[1] = box.Y;
        outBox[2] = box.Right();
        outBox[3] = box.Bottom();
    }
    return count;
}

/// Adds level ground at y = 0 and a grid NavigationSurface of 16 x 16 cells of 1 m from the origin over
/// it, and bakes the surface. 0 on success, -1 when the bake did not complete.
GE_TEST_SCENE_EXPORT int GE_TestScene_AddBakedNavigationGrid(std::uint32_t* outSurface)
{
    ECS::World* world = SceneWorld();
    if (world == nullptr || outSurface == nullptr)
        return -1;
    if (!PathfindingECS::NavigationService::IsInitialized())
        PathfindingECS::NavigationService::Initialize();

    const ECS::EntityHandle ground = world->CreateEntity();
    Components::PhysicsBody body{};
    body.motionType = Physics::MotionType::Static;
    world->AddComponentImmediate(ground, body);
    world->AddComponentImmediate(ground, Components::PhysicsCollider{});
    world->AddComponentImmediate(ground, Components::PlaneColliderShape{});
    world->AddComponentImmediate(ground, Components::Transform{});
    world->AddComponentImmediate(ground, Components::WorldTransform{});

    const ECS::EntityHandle entity = world->CreateEntity();
    Components::NavigationSurface surface{};
    surface.Mode = Components::NavigationSurfaceMode::Grid;
    surface.CellSize = 1.0f;
    surface.FitBounds = false;
    surface.Center = {8.0f, 0.0f, 8.0f};
    surface.Extent = {8.0f, 1.0f, 8.0f};
    world->AddComponentImmediate(entity, surface);
    PathfindingECS::NavigationSurfaceSystem{}.Update(*world, 1.0f / 60.0f);
    const auto* baked = world->GetComponent<Components::NavigationSurface>(entity);
    if (baked == nullptr || baked->BakePhase != Components::NavigationBakePhase::Baked)
        return -1;
    *outSurface = entity.id;
    return 0;
}

/// Adds a NavigationAgent standing at (x, 0, z) that walks the surface on `surface`. 0 on success.
GE_TEST_SCENE_EXPORT int GE_TestScene_AddNavigationAgent(std::uint32_t surface, float x, float z,
                                                         std::uint32_t* outAgent)
{
    ECS::World* world = SceneWorld();
    if (world == nullptr || outAgent == nullptr)
        return -1;
    const ECS::EntityHandle entity = world->CreateEntity();
    Components::NavigationAgent agent{};
    agent.Surface = ECS::EntityHandle(surface);
    world->AddComponentImmediate(entity, agent);
    Components::Transform transform{};
    transform.matrix[12] = x;
    transform.matrix[14] = z;
    world->AddComponentImmediate(entity, transform);
    Components::WorldTransform worldTransform{};
    worldTransform.matrix[12] = x;
    worldTransform.matrix[14] = z;
    world->AddComponentImmediate(entity, worldTransform);
    *outAgent = entity.id;
    return 0;
}

/// Runs `frames` frames of the navigation path solves, which answer the requests of the agents. 0 on
/// success.
GE_TEST_SCENE_EXPORT int GE_TestScene_StepNavigation(int frames)
{
    ECS::World* world = SceneWorld();
    if (world == nullptr || !PathfindingECS::NavigationService::IsInitialized())
        return -1;
    static PathfindingECS::NavigationPathfindingSystem paths;
    for (int i = 0; i < frames; ++i)
        paths.Update(*world, 1.0f / 60.0f);
    return 0;
}
