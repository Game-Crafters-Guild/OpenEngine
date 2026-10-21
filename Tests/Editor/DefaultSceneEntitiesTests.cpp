#include <gtest/gtest.h>

#include "Scene/DefaultSceneEntities.h"

#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunPath.h"
#include "Components/Transform.h"
#include "Engine/Rendering/RenderServices.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "StagedTestPaths.h"

using namespace GameEngine;

// A new scene takes its ambient from the sky. A reflection probe the camera stands in replaces the sky as the
// view's diffuse and specular environment (ReflectionProbeSystem), so a capture probe seeded into every new scene
// would define its global lighting from one capture point, and anything placed at that point would darken it all.
TEST(DefaultSceneEntities, ANewSceneTakesItsAmbientFromTheSky)
{
    ECS::World world;
    Editor::SeedDefaultSceneEntities(world, nullptr);

    int skies = 0;
    world.Query<ECS::Read<Components::SkyEnvironment>>().Each(
        [&](ECS::EntityHandle, const Components::SkyEnvironment&) { ++skies; });
    int probes = 0;
    world.Query<ECS::Read<Components::ReflectionProbe>>().Each(
        [&](ECS::EntityHandle, const Components::ReflectionProbe&) { ++probes; });

    EXPECT_EQ(skies, 1) << "positive control: the default scene seeds its sky";
    EXPECT_EQ(probes, 0) << "a new scene seeds no reflection probe; its ambient is the sky's";
}

// A new scene's sun stands mid-morning in the south-east, not overhead: about 42 degrees up at
// 10:00 at 40 degrees north on the equinox, so vertical faces are sunlit and shadows fall away
// from the default camera (0, 3, -10) looking +Z. Noon on the equator puts it at the zenith.
TEST(DefaultSceneEntities, ANewScenesSunStandsOffTheZenithInTheSouthEast)
{
    ECS::World world;
    Editor::SeedDefaultSceneEntities(world, nullptr);

    int skies = 0;
    world.Query<ECS::Read<Components::SkyEnvironment>>().Each(
        [&](ECS::EntityHandle, const Components::SkyEnvironment& sky)
        {
            ++skies;
            const Rendering::SolarFrame frame =
                Rendering::MakeSolarFrame(Components::SkySunPath::PathAngles(sky));
            const Rendering::SolarPosition sun = Rendering::SolarPositionAtHour(frame, sky.TimeOfDayHours);
            EXPECT_GT(sun.ElevationDegrees, 30.0f);
            EXPECT_LT(sun.ElevationDegrees, 55.0f);
            EXPECT_GT(sun.AzimuthDegrees, 90.0f) << "east of south";
            EXPECT_LT(sun.AzimuthDegrees, 180.0f) << "east of south";
        });
    EXPECT_EQ(skies, 1);
}

// A new scene's sky is the component default, and its global volume carries the default height
// fog, so a new scene shows the same horizon haze as the project templates.
TEST(DefaultSceneEntities, ANewSceneHasTheDefaultSkyAndHorizonFog)
{
    ECS::World world;
    Editor::SeedDefaultSceneEntities(world, nullptr);

    const Components::SkyEnvironment defaultSky{};
    int skies = 0;
    world.Query<ECS::Read<Components::SkyEnvironment>>().Each(
        [&](ECS::EntityHandle, const Components::SkyEnvironment& sky)
        {
            ++skies;
            EXPECT_FLOAT_EQ(sky.TimeOfDayHours, defaultSky.TimeOfDayHours);
            EXPECT_FLOAT_EQ(sky.Latitude, defaultSky.Latitude);
            EXPECT_EQ(sky.DayOfYear, defaultSky.DayOfYear);
        });
    EXPECT_EQ(skies, 1);

    const Components::HeightFogEffect defaultFog{};
    int fogs = 0;
    world.Query<ECS::Read<Components::PostProcessVolume>, ECS::Read<Components::HeightFogEffect>>().Each(
        [&](ECS::EntityHandle, const Components::PostProcessVolume& volume, const Components::HeightFogEffect& fog)
        {
            ++fogs;
            EXPECT_TRUE(volume.IsGlobal);
            EXPECT_TRUE(fog.Enabled);
            EXPECT_EQ(fog.ColorMode, defaultFog.ColorMode);
            EXPECT_FLOAT_EQ(fog.MinDistance, defaultFog.MinDistance);
            EXPECT_FLOAT_EQ(fog.MaxDistance, defaultFog.MaxDistance);
            EXPECT_FLOAT_EQ(fog.SkyFillEnd, defaultFog.SkyFillEnd);
        });
    EXPECT_EQ(fogs, 1) << "the global post-process volume carries the height fog";
}

// The Main Camera sees, and the ground reaches, as far as the default height fog takes to cover the
// ground, so a new scene's ground ends inside the horizon haze.
TEST(DefaultSceneEntities, TheGroundAndTheFarClipReachTheFogsFullStrength)
{
    Engine::Renderer::RenderServices renderServices;
    ECS::World world;
    Editor::SeedDefaultSceneEntities(world, &renderServices);
    const float fogDistance = Components::HeightFogEffect{}.MaxDistance;

    int cameras = 0;
    world.Query<ECS::Read<Components::Camera>>().Each(
        [&](ECS::EntityHandle, const Components::Camera& camera)
        {
            ++cameras;
            EXPECT_GE(camera.FarZ, fogDistance);
        });
    EXPECT_EQ(cameras, 1);

    int planes = 0;
    world.Query<ECS::Read<Components::PlaneColliderShape>, ECS::Read<Components::LocalBounds>,
                ECS::Read<Components::Transform>>().Each(
        [&](ECS::EntityHandle, const Components::PlaneColliderShape&, const Components::LocalBounds& bounds,
            const Components::Transform& transform)
        {
            ++planes;
            EXPECT_GE(bounds.Box.halfExtents.x * transform.GetScale().x, fogDistance);
            EXPECT_GE(bounds.Box.halfExtents.z * transform.GetScale().z, fogDistance);
        });
    EXPECT_EQ(planes, 1);
}

// A new scene's ground is the engine's grid ground, the material the Basic 3D template's ground uses,
// so a new scene and the template read as one look. The material samples the engine's grid texture and
// repeats its 1 m grid cell once per meter of the plane.
TEST(DefaultSceneEntities, TheGroundIsTheEnginesGridGround)
{
    Engine::Renderer::RenderServices renderServices;
    ECS::World world;
    Editor::SeedDefaultSceneEntities(world, &renderServices);
    const GUID gridGround =
        AssetRegistry::DeriveGuidForSourcePath(kAssetSourceAliasEditor, "Materials/GroundGrid.material");
    MaterialAsset material(gridGround, TestPaths::StagedEngineAssetsDir() / "Materials/GroundGrid.material");
    ASSERT_TRUE(material.Load()) << material.GetPath();
    const auto tiling = material.GetDocument().textureTransforms.find("albedoMap");
    ASSERT_NE(tiling, material.GetDocument().textureTransforms.end());
    const auto albedo = material.GetDocument().textures.find("albedoMap");
    ASSERT_NE(albedo, material.GetDocument().textures.end());
    EXPECT_EQ(GUID(albedo->second.c_str()),
              AssetRegistry::DeriveGuidForSourcePath(kAssetSourceAliasEditor, "Textures/GroundGrid.png"));

    int planes = 0;
    world.Query<ECS::Read<Components::PlaneColliderShape>, ECS::Read<Components::MeshRenderer>,
                ECS::Read<Components::Transform>>().Each(
        [&](ECS::EntityHandle, const Components::PlaneColliderShape&, const Components::MeshRenderer& renderer,
            const Components::Transform& transform)
        {
            ++planes;
            EXPECT_EQ(renderer.materialAssetGuid.ToGuid(), gridGround);
            EXPECT_FLOAT_EQ(tiling->second[0], transform.GetScale().x) << "one grid cell per meter";
            EXPECT_FLOAT_EQ(tiling->second[5], transform.GetScale().z) << "one grid cell per meter";
        });
    EXPECT_EQ(planes, 1);
}

TEST(DefaultSceneEntities, SeededPrimitiveBottomsRestOnThePlane)
{
    Engine::Renderer::RenderServices renderServices;
    ECS::World world;
    Editor::SeedDefaultSceneEntities(world, &renderServices);

    int planes = 0;
    float planeHeight = 0.0f;
    world.Query<ECS::Read<Components::PlaneColliderShape>, ECS::Read<Components::Transform>>().Each(
        [&](ECS::EntityHandle, const Components::PlaneColliderShape&, const Components::Transform& transform)
        {
            ++planes;
            planeHeight = transform.GetPosition().y;
        });
    ASSERT_EQ(planes, 1);

    int primitives = 0;
    world.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::LocalBounds>,
                ECS::Read<Components::Transform>, ECS::Read<Components::Name>>().Each(
        [&](ECS::EntityHandle, const Components::MeshRenderer&, const Components::LocalBounds& bounds,
            const Components::Transform& transform, const Components::Name& name)
        {
            ++primitives;
            const float bottom = transform.GetPosition().y +
                (bounds.Box.center.y - bounds.Box.halfExtents.y) * transform.GetScale().y;
            EXPECT_FLOAT_EQ(bottom, planeHeight) << name.value;
        });
    EXPECT_EQ(primitives, 3) << "the plane and both spheres must be seeded";
}
