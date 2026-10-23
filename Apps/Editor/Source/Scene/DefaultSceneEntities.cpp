#include "Scene/DefaultSceneEntities.h"

#include "Inspectors/ComponentPresets.h"

#include "Assets/AssetRegistry.h"
#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Transform.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"

#include <cstring>

namespace GameEngine::Editor
{

using namespace Components;

namespace
{

void SetName(ECS::Entity& entity, const char* text)
{
    Name name{};
    std::memset(name.value, 0, sizeof(name.value));
    const size_t maxCopy = sizeof(name.value) - 1;
    for (size_t i = 0; i < maxCopy && text[i] != '\0'; ++i)
        name.value[i] = text[i];
    entity.Set(name);
}

MeshRenderer MakeMeshRenderer(Engine::Renderer::RenderServices* rs, const GUID& meshGuid, const GUID& matGuid)
{
    return Engine::Renderer::PrimitiveGenerator::MakePrimitiveMeshRenderer(rs, meshGuid, matGuid);
}

void SpawnDynamicSphere(ECS::World& world,
                        Engine::Renderer::RenderServices* rs,
                        const GUID& sphereGuid,
                        const GUID& materialGuid,
                        const Mathematics::Vector3& position,
                        const char* name)
{
    ECS::Entity e = world.Create();
    e.Set(Transform::FromTRS(position, Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
    e.Set(MakeMeshRenderer(rs, sphereGuid, materialGuid));
    e.Set(Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(rs, sphereGuid));

    PhysicsBody sphereBody{};
    sphereBody.motionType = Physics::MotionType::Dynamic;
    world.AddComponentImmediate(e.GetHandle(), sphereBody);

    PhysicsCollider sphereCollider{};
    sphereCollider.layer = Physics::Layers::Dynamic;
    world.AddComponentImmediate(e.GetHandle(), sphereCollider);
    world.AddComponentImmediate(e.GetHandle(), SphereColliderShape{});

    SetName(e, name);
}

// The engine's grid ground, shared with the Basic 3D template's ground. The material file is engine
// content (Engine/Assets/Materials), staged into the editor asset source, whose GUIDs derive from their
// paths.
GUID GroundGridMaterialGuid()
{
    return AssetRegistry::DeriveGuidForSourcePath(kAssetSourceAliasEditor, "Materials/GroundGrid.material");
}

} // namespace

void SeedDefaultSceneEntities(ECS::World& world, Engine::Renderer::RenderServices* rs)
{
    const GUID matGuid = Engine::Renderer::PrimitiveGenerator::DefaultMaterialGuid();
    // The camera sees, and the ground reaches, as far as the default height fog takes to cover the
    // ground, so the ground ends inside the horizon haze rather than as a line below the horizon.
    const float viewDistance = HeightFogEffect{}.MaxDistance;

    // --- Directional Light (inspector Euler 50, -30, 0; shines along +Z) ---
    ECS::EntityHandle directionalLightHandle{};
    {
        ECS::Entity e = world.Create();
        auto rot = QuaternionFromEulerXYZDegrees(kDefaultDirectionalLightEulerXDeg,
                                                kDefaultDirectionalLightEulerYDeg,
                                                kDefaultDirectionalLightEulerZDeg);
        e.Set(Transform::FromTRS(Mathematics::Vector3{0.0f, 3.0f, 0.0f}, rot, Mathematics::Vector3{1.0f, 1.0f, 1.0f}));

        e.Set(MakeDefaultDirectionalLight());

        SetName(e, "Directional Light");
        directionalLightHandle = e.GetHandle();
    }

    // No flat Ambient Light: ambient comes from the sky-derived IBL irradiance,
    // which tracks the sun physically. A fixed-lux ambient double-counts it and
    // fights the exposure model when the sun dims (ambient-lit ground vs a
    // physically dark sky skews the auto-exposure metering).

    // --- Sky Environment ---
    {
        ECS::Entity e = world.Create();
        e.Set(Transform::FromTRS(Mathematics::Vector3{}, Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
        // The component's default sun stands mid-morning in the south-east, over the default
        // camera's shoulder, so a new scene shows sunlit vertical faces and shadows that fall
        // away from the camera.
        SkyEnvironment sky{};
        sky.SunLight = directionalLightHandle;
        e.Set(sky);
        SetName(e, "Sky Environment");
    }

    // --- Main Camera (Game View default; matches editor inspector defaults: pos 0,3,-10, identity rot, unit scale) ---
    {
        ECS::Entity e = world.Create();
        e.Set(Transform::FromTRS(
            Mathematics::Vector3{0.0f, 3.0f, -10.0f},
            Mathematics::Quaternion{},
            Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
        Camera camera{};
        camera.FarZ = viewDistance;
        e.Set(camera);
        SetName(e, "Main Camera");
    }

    // --- Post Process Volume ---
    {
        ECS::Entity e = world.Create();
        e.Set(Transform::FromTRS(Mathematics::Vector3{}, Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f}));

        // Tonemap is left at the component default so the default scene and a
        // volume-less world always show the same operator.
        PostProcessVolume volume{};
        volume.IsGlobal = true;
        e.Set(volume);
        // The default height fog is the horizon haze in the sky's own horizon color.
        e.Set(HeightFogEffect{});
        // Exposure is on the Main Camera (default Auto); camera-less views (the editor Scene View)
        // auto-meter from the world default.

        SetName(e, "Post Process Volume");
    }

    // --- Ground Plane ---
    if (rs)
    {
        const GUID groundGuid = Engine::Renderer::PrimitiveGenerator::GroundGuid();
        ECS::Entity e = world.Create();
        // The ground primitive is 1 m wide: this scale puts its edge viewDistance from the origin.
        // The grid ground tiles its 1 m cell 6000 times across the plane, which is 2 x viewDistance wide.
        e.Set(Transform::FromTRS(Mathematics::Vector3{0.0f, 0.0f, 0.0f}, Mathematics::Quaternion{}, Mathematics::Vector3{2.0f * viewDistance, 1.0f, 2.0f * viewDistance}));
        e.Set(MakeMeshRenderer(rs, groundGuid, GroundGridMaterialGuid()));
        e.Set(Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(rs, groundGuid));

        PhysicsBody planeBody{};
        planeBody.motionType = Physics::MotionType::Static;
        planeBody.gravityScale = 0.0f;
        world.AddComponentImmediate(e.GetHandle(), planeBody);

        PhysicsCollider planeCollider{};
        planeCollider.layer = Physics::Layers::Static;
        world.AddComponentImmediate(e.GetHandle(), planeCollider);
        world.AddComponentImmediate(e.GetHandle(), PlaneColliderShape{});

        SetName(e, "Ground");
    }

    // --- Sphere ---
    if (rs)
    {
        const GUID sphereGuid = Engine::Renderer::PrimitiveGenerator::SphereGuid();
        const GUID reflectionMatGuid = Engine::Renderer::PrimitiveGenerator::ReflectionProbeTestMaterialGuid();
        SpawnDynamicSphere(world, rs, sphereGuid, matGuid, Mathematics::Vector3{-1.25f, SphereColliderShape{}.radius, 0.0f}, "Sphere");
        SpawnDynamicSphere(world, rs, sphereGuid, reflectionMatGuid, Mathematics::Vector3{1.25f, SphereColliderShape{}.radius, 0.0f}, "Mirror Sphere");
    }

    world.ProcessCommands();
}

} // namespace GameEngine::Editor
