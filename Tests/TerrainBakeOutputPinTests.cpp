// Pins of the modifier bake output: an evaluator, base noise or splat rule change that moves a
// texel fails here, and the change bumps kTerrainBakeVersion, which seeds every surface page store
// key (TerrainBakeKeyBuilder.h), so stores baked before the change miss.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "SplineECS/SplineService.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainZonePayload.h"
#include "Types/Fnv1a.h"

#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

constexpr uint32 kDim = 129;
constexpr float32 kWorldSize = 256.0f;
constexpr float32 kHeightScale = 64.0f;

struct BakeResult
{
    std::vector<float32> Heights;
    std::vector<uint8> Splat;
};

uint64 OutputHash(const BakeResult& bake)
{
    const uint64 heights = Hashing::Fnv1a64(bake.Heights.data(), bake.Heights.size() * sizeof(float32));
    return Hashing::Fnv1a64(bake.Splat.data(), bake.Splat.size(), heights);
}

BakeResult ResultOf(const TerrainData& data)
{
    BakeResult result;
    result.Heights.assign(data.Heightfield.GetRawSamples(),
                          data.Heightfield.GetRawSamples() + data.Heightfield.GetSampleCount());
    result.Splat = data.Splatmap;
    return result;
}

// A kDim x kDim terrain of kWorldSize meters.
TerrainHandle CreatePinTerrain(TerrainService& service)
{
    Terrain::TerrainConfig config{};
    config.HeightmapWidth = kDim;
    config.HeightmapHeight = kDim;
    config.WorldSizeX = kWorldSize;
    config.WorldSizeZ = kWorldSize;
    config.HeightScale = kHeightScale;
    config.LODLevels = 4;
    return service.CreateTerrain(config);
}

// A modifier volume of `shape` centered on world (x, z).
ECS::EntityHandle AddVolume(ECS::World& world, float32 x, float32 z, Components::TerrainVolumeShape shape,
                            float32 radius)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume volume{};
    volume.Shape = shape;
    volume.Radius = radius;
    volume.Falloff = 8.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, volume);
    Components::WorldTransform xf{};
    xf.Position.X = x;
    xf.Position.Z = z;
    world.AddComponentImmediate<Components::WorldTransform>(e, xf);
    return e;
}

void AddTerrainEntity(ECS::World& world, TerrainHandle handle, Components::TerrainBaseSource base, const GUID& heightmap)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = kWorldSize;
    terrain.SizeZ = kWorldSize;
    terrain.HeightScale = kHeightScale;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    terrain.BaseSource = base;
    if (!heightmap.IsNull())
        terrain.TerrainAssetGuid.Set(heightmap);
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
}

// A heightmap base with a flatten, a noise, a masked stamp, a paint layer, surface rules, a
// spline-path flatten and a sculpt zone, through the modifier system's first (full) bake.
BakeResult BakeAuthoredScene()
{
    if (TerrainService::IsInitialized())
        TerrainService::Shutdown();
    TerrainService::Initialize();
    if (SplineECS::SplineService::IsInitialized())
        SplineECS::SplineService::Shutdown();
    SplineECS::SplineService::Initialize();
    auto& svc = TerrainService::Get();

    const GUID heightmapGuid = GUID::Generate();
    const GUID maskGuid = GUID::Generate();
    const GUID payloadGuid = GUID::Generate();
    Terrain::HeightfieldData heightmap(33, 33);
    heightmap.FillWithNoise(3.0f, 1.0f, 4, 99);
    svc.SeedDecodedHeightmapForTests(heightmapGuid, heightmap);
    TerrainZonePayload payload;
    payload.Allocate(ZonePayloadFormat::SculptOffsetR32F, 17, 17);
    for (uint32 i = 0; i < 17u * 17u; ++i)
        payload.Offsets[i] = 30.0f * static_cast<float32>(i % 17u) / 16.0f;
    svc.SeedZonePayloadForTests(payloadGuid, payload);

    const TerrainHandle handle = CreatePinTerrain(svc);
    ECS::World world;
    EnableTerrainModifierLifecycleEvents(world);
    AddTerrainEntity(world, handle, Components::TerrainBaseSource::HeightmapAsset, heightmapGuid);
    {
        Components::TerrainFlattenEffect flatten{};
        flatten.TargetHeight = 20.0f;
        world.AddComponentImmediate(AddVolume(world, 30.0f, 40.0f, Components::TerrainVolumeShape::Circle, 25.0f),
                                    flatten);
        Components::TerrainNoiseEffect noise{};
        noise.Amplitude = 5.0f;
        noise.Frequency = 8.0f;
        noise.Octaves = 3u;
        world.AddComponentImmediate(AddVolume(world, -50.0f, 10.0f, Components::TerrainVolumeShape::Circle, 25.0f),
                                    noise);
        Components::TerrainStampEffect stamp{};
        stamp.HeightScale = 20.0f;
        stamp.StampAssetGuid.Set(maskGuid);
        world.AddComponentImmediate(AddVolume(world, 60.0f, -60.0f, Components::TerrainVolumeShape::Circle, 25.0f),
                                    stamp);
        Components::TerrainPaintLayerEffect paint{};
        paint.LayerIndex = 1;
        paint.Strength = 0.8f;
        world.AddComponentImmediate(AddVolume(world, -30.0f, -60.0f, Components::TerrainVolumeShape::Circle, 20.0f),
                                    paint);
        Components::TerrainSurfaceRulesEffect rules{};
        rules.RuleCount = 1;
        rules.Rules[0].MaterialSlot = 2;
        rules.Rules[0].Strength = 1.0f;
        rules.Rules[0].ConditionCount = 1;
        rules.Rules[0].Conditions[0].Kind = Components::TerrainRuleConditionKind::HeightMeters;
        rules.Rules[0].Conditions[0].Min = 0.3f * kHeightScale;
        rules.Rules[0].Conditions[0].Max = 0.9f * kHeightScale;
        world.AddComponentImmediate(AddVolume(world, 0.0f, 0.0f, Components::TerrainVolumeShape::Global, 0.0f), rules);
    }
    {
        auto& splines = SplineECS::SplineService::Get();
        const SplineECS::SplineHandle spline = splines.CreateSpline(Spline::SplineType::CatmullRom, false);
        auto* points = splines.GetSplineData(spline);
        for (int i = 0; i < 4; ++i)
            points->AddPoint(Mathematics::Vector3(-100.0f + 60.0f * static_cast<float32>(i), 0.0f,
                                                  80.0f - 20.0f * static_cast<float32>(i)),
                             10.0f);
        splines.RebuildCache(spline);
        auto e = AddVolume(world, 0.0f, 0.0f, Components::TerrainVolumeShape::SplinePath, 0.0f);
        Components::SplineComponent component{};
        component.SplineDataIndex = spline.Index();
        component.SplineDataGeneration = spline.Generation();
        world.AddComponentImmediate<Components::SplineComponent>(e, component);
        Components::TerrainFlattenEffect pathFlatten{};
        pathFlatten.TargetHeight = 12.0f;
        world.AddComponentImmediate(e, pathFlatten);
    }
    {
        auto e = world.CreateEntity();
        Components::TerrainSculptZone zone{};
        zone.ExtentX = 40.0f;
        zone.ExtentZ = 40.0f;
        zone.Priority = 1000.0f;
        zone.PayloadRef.Set(payloadGuid);
        world.AddComponentImmediate<Components::TerrainSculptZone>(e, zone);
        Components::WorldTransform xf{};
        xf.Position.X = 80.0f;
        xf.Position.Z = 60.0f;
        world.AddComponentImmediate<Components::WorldTransform>(e, xf);
    }

    std::vector<float32> mask(16u * 16u);
    for (uint32 i = 0; i < mask.size(); ++i)
        mask[i] = 0.15f + 0.7f * static_cast<float32>(i % 16u) / 15.0f;
    TerrainModifierSystem system;
    system.SeedDecodedStampMaskForTests(maskGuid, mask, 16, 16);
    system.Update(world, 1.0f / 60.0f);

    const BakeResult result = ResultOf(*svc.GetTerrainData(handle));
    SplineECS::SplineService::Shutdown();
    TerrainService::Shutdown();
    return result;
}

// A Flat base with the effect kinds BakeAuthoredScene lacks: a height offset, an eroded noise, a
// ground claim, grass and a paint zone.
BakeResult BakeFlatBaseScene()
{
    if (TerrainService::IsInitialized())
        TerrainService::Shutdown();
    TerrainService::Initialize();
    auto& svc = TerrainService::Get();
    const GUID maskGuid = GUID::Generate();
    TerrainZonePayload mask;
    mask.Allocate(ZonePayloadFormat::PaintMaskR8, 17, 17);
    for (uint32 i = 0; i < 17u * 17u; ++i)
        mask.Mask[i] = static_cast<uint8>((i * 13u) % 256u);
    svc.SeedZonePayloadForTests(maskGuid, mask);

    const TerrainHandle handle = CreatePinTerrain(svc);
    ECS::World world;
    EnableTerrainModifierLifecycleEvents(world);
    AddTerrainEntity(world, handle, Components::TerrainBaseSource::Flat, GUID{});
    {
        Components::TerrainHeightOffsetEffect offset{};
        offset.Offset = 6.0f;
        world.AddComponentImmediate(AddVolume(world, 40.0f, 30.0f, Components::TerrainVolumeShape::Circle, 30.0f),
                                    offset);
        Components::TerrainNoiseEffect eroded{};
        eroded.Amplitude = 8.0f;
        eroded.ErosionStrength = 0.6f;
        world.AddComponentImmediate(AddVolume(world, -40.0f, -20.0f, Components::TerrainVolumeShape::Circle, 35.0f),
                                    eroded);
        Components::TerrainGroundClaimEffect claim{};
        world.AddComponentImmediate(AddVolume(world, -40.0f, -20.0f, Components::TerrainVolumeShape::Circle, 15.0f),
                                    claim);
        Components::TerrainGrassEffect grass{};
        grass.HeightScale = 0.5f;
        grass.DensityScale = 0.5f;
        world.AddComponentImmediate(AddVolume(world, 0.0f, 60.0f, Components::TerrainVolumeShape::Circle, 20.0f),
                                    grass);

        auto zoneEntity = world.CreateEntity();
        Components::TerrainPaintZone zone{};
        zone.ExtentX = 30.0f;
        zone.ExtentZ = 30.0f;
        zone.LayerIndex = 2;
        zone.PayloadRef.Set(maskGuid);
        world.AddComponentImmediate<Components::TerrainPaintZone>(zoneEntity, zone);
        Components::WorldTransform xf{};
        xf.Position.X = 60.0f;
        xf.Position.Z = -50.0f;
        world.AddComponentImmediate<Components::WorldTransform>(zoneEntity, xf);
    }
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const BakeResult result = ResultOf(*svc.GetTerrainData(handle));
    TerrainService::Shutdown();
    return result;
}

} // namespace

// When a pin fails on purpose, bump kTerrainBakeVersion and take the new value from the failure
// message.
TEST(TerrainBakeOutputPin, TheAuthoredBakeOutputIsPinnedToTheBakeVersion)
{
    constexpr uint64 kPinnedOutput = 0x9e99e58ee4e15cccull;
    const uint64 output = OutputHash(BakeAuthoredScene());
    EXPECT_EQ(output, kPinnedOutput) << std::hex << "the bake output changed (now 0x" << output
                                     << "): bump kTerrainBakeVersion, then pin the new value";
}

TEST(TerrainBakeOutputPin, TheFlatBaseBakeOutputIsPinnedToTheBakeVersion)
{
    // One value on every platform: the eroded noise takes its sin, cos and pow from Math (Transcendental.h).
    constexpr uint64 kPinnedOutput = 0x10971e86c0c37a18ull;
    const uint64 output = OutputHash(BakeFlatBaseScene());
    EXPECT_EQ(output, kPinnedOutput) << std::hex << "the flat-base bake output changed (now 0x" << output
                                     << "): bump kTerrainBakeVersion, then pin the new value";
}
