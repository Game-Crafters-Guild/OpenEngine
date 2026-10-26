#include <gtest/gtest.h>

#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/Interpolation.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"
#include "TerrainECS/Scene/TerrainSceneSchemas.h"
#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainGrassField.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainRowBands.h" // TerrainECS/Source: kParallelBakeMinTexels

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

class TerrainGrassRegions : public testing::Test
{
protected:
    // Declared first so it outlives the system and the world that use it; null runs every compose
    // in place.
    std::unique_ptr<JobSystem::WorkStealingThreadPool> Pool;
    ECS::World World;
    TerrainModifierSystem System;
    TerrainHandle Handle{};
    ECS::EntityHandle TerrainEntity{};
    TerrainData* Data = nullptr;

    void SetUp() override
    {
        if (TerrainService::IsInitialized()) TerrainService::Shutdown();
        if (SplineECS::SplineService::IsInitialized()) SplineECS::SplineService::Shutdown();
        TerrainService::Initialize();
        SplineECS::SplineService::Initialize();
        EnableTerrainModifierLifecycleEvents(World);
        Terrain::TerrainConfig config{};
        config.HeightmapWidth = config.HeightmapHeight = 65;
        config.WorldSizeX = config.WorldSizeZ = 128;
        config.HeightScale = 32;
        config.LODLevels = 3;
        Handle = TerrainService::Get().CreateTerrain(config);
        Data = TerrainService::Get().GetTerrainData(Handle);
        ASSERT_NE(Data, nullptr);
        const auto entity = World.CreateEntity();
        TerrainEntity = entity;
        Components::Terrain terrain{};
        terrain.SizeX = terrain.SizeZ = 128;
        terrain.HeightScale = 32;
        terrain.BaseSource = Components::TerrainBaseSource::Flat;
        terrain.TerrainDataHandle = Handle.Index;
        terrain.TerrainDataGeneration = Handle.Generation;
        World.AddComponentImmediate<Components::Terrain>(entity, terrain);
        World.AddComponentImmediate<Components::WorldTransform>(entity, {});
        Tick(); // establish the ordinary ground bake before measuring cosmetic edits
    }

    void TearDown() override
    {
        World.Clear();
        SplineECS::SplineService::Shutdown();
        TerrainService::Shutdown();
    }

    void Tick()
    {
        World.SwapLifecycleEvents();
        System.Update(World, 1.0f / 60.0f);
    }

    ECS::EntityHandle Region(Components::TerrainVolumeShape shape, float32 height,
                             float32 density, float32 x = 0, float32 priority = 0)
    {
        const auto entity = World.CreateEntity();
        Components::TerrainModifierVolume volume{};
        volume.Shape = shape;
        volume.Radius = volume.RectHalfX = volume.RectHalfZ = 12;
        volume.Falloff = 4;
        volume.Priority = priority;
        World.AddComponentImmediate<Components::TerrainModifierVolume>(entity, volume);
        Components::TerrainGrassEffect effect{};
        effect.HeightScale = height; effect.DensityScale = density;
        World.AddComponentImmediate<Components::TerrainGrassEffect>(entity, effect);
        Components::WorldTransform transform{};
        transform.matrix[12] = x;
        World.AddComponentImmediate<Components::WorldTransform>(entity, transform);
        return entity;
    }

    ECS::EntityHandle GroundRegion(float32 height, float32 priority)
    {
        const auto entity = World.CreateEntity();
        Components::TerrainModifierVolume volume{};
        volume.Shape = Components::TerrainVolumeShape::Global;
        volume.Priority = priority;
        World.AddComponentImmediate<Components::TerrainModifierVolume>(entity, volume);
        Components::TerrainFlattenEffect effect{};
        effect.UseVolumeHeight = false;
        effect.TargetHeight = height;
        World.AddComponentImmediate<Components::TerrainFlattenEffect>(entity, effect);
        World.AddComponentImmediate<Components::WorldTransform>(entity, {});
        return entity;
    }

    uint8 At(float32 x, float32 z, uint32 channel = 0) const
    {
        if (!Data->GrassField.IsActive()) return 255;
        const auto ix = static_cast<uint32>((x + 64) / 2);
        const auto iz = static_cast<uint32>((z + 64) / 2);
        return Data->GrassField.Pixels[(iz * 65 + ix) * 2 + channel];
    }

    SplineECS::SplineHandle AddClosedSpline(ECS::EntityHandle entity)
    {
        auto& service = SplineECS::SplineService::Get();
        const auto spline = service.CreateSpline(Spline::SplineType::CatmullRom, true);
        for (const auto& p : std::array<Mathematics::Vector3, 4>{{{-24,0,-24},{24,0,-24},{24,0,24},{-24,0,24}}})
            service.GetSplineData(spline)->AddPoint(p, 3);
        service.RebuildCache(spline);
        Components::SplineComponent component{};
        component.SplineDataIndex = spline.Index();
        component.SplineDataGeneration = spline.Generation();
        World.AddComponentImmediate<Components::SplineComponent>(entity, component);
        return spline;
    }
};

// The compose runs as a job: a pool whose one worker the test holds keeps it queued for as long as
// a test needs it in flight.
class TerrainGrassAsyncCompose : public TerrainGrassRegions
{
protected:
    std::optional<std::promise<void>> Gate;
    JobSystem::TaskHandle Blocker;
    // A neutral region under every other: an empty stack composes in place, so the fixture's first
    // update needs a region to request its first field as a job. Being neutral, it changes no pixel.
    ECS::EntityHandle StackRegion{};

    void SetUp() override
    {
        Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(1);
        World.SetJobSystem(Pool.get());
        StackRegion = Region(Components::TerrainVolumeShape::Global, 1, 1, 0, -1);
        HoldPool(); // the fixture's first update requests the first field
        TerrainGrassRegions::SetUp();
    }

    void TearDown() override
    {
        ReleasePool();
        TerrainGrassRegions::TearDown();
    }

    void HoldPool()
    {
        Gate.emplace();
        auto started = std::make_shared<std::promise<void>>();
        auto running = started->get_future();
        Blocker = Pool->Submit([started, gate = Gate->get_future().share()] { started->set_value(); gate.wait(); });
        running.wait();
    }

    void ReleasePool()
    {
        if (!Gate) return;
        Gate->set_value();
        Gate.reset();
        Blocker.Wait();
    }

    // Lets every requested compose finish and swap in, including one an edit left owed.
    void Settle()
    {
        ReleasePool();
        for (int i = 0; i < 3; ++i)
        {
            System.WaitForGrassComposeForTests();
            Tick();
        }
    }
};

TEST_F(TerrainGrassAsyncCompose, TheUpdateReturnsBeforeTheComposeRuns)
{
    EXPECT_FALSE(Data->GrassField.Initialized) << "the update composed the first field itself";
    Settle();
    ASSERT_TRUE(Data->GrassField.Initialized);
    HoldPool();
    Region(Components::TerrainVolumeShape::Circle, 0.2f, 0.4f, -30);
    Tick();
    EXPECT_EQ(At(-30, 0), 255) << "the update waited for the compose";
    Settle();
    EXPECT_EQ(At(-30, 0), 51);
}

TEST_F(TerrainGrassAsyncCompose, AnEditDuringTheComposeDropsItsResultAndOnlyTheNewestLands)
{
    Settle();
    const auto version = Data->GrassField.Version;
    HoldPool();
    const auto region = Region(Components::TerrainVolumeShape::Circle, 0.2f, 0.4f, -30);
    Tick(); // requests the 0.4 density, held
    World.GetComponentForWrite<Components::TerrainGrassEffect>(region)->DensityScale = 0.8f;
    Tick();
    World.GetComponentForWrite<Components::TerrainGrassEffect>(region)->DensityScale = 0.6f;
    Tick();
    ReleasePool();
    System.WaitForGrassComposeForTests();
    Tick(); // the 0.4 result lands stale: dropped, and the newest stack is requested
    EXPECT_EQ(Data->GrassField.Version, version) << "a stale result was applied";
    System.WaitForGrassComposeForTests();
    Tick();
    EXPECT_EQ(Data->GrassField.Version, version + 1) << "more than one compose landed";
    EXPECT_EQ(At(-30, 0, 1), 153);
    EXPECT_EQ(At(-30, 0), 51);
}

TEST_F(TerrainGrassAsyncCompose, GrassWaitsForTheFirstFieldAndAnEditKeepsThePreviousOneDrawn)
{
    EXPECT_FALSE(Data->GrassField.Initialized);
    EXPECT_TRUE(Data->GrassField.GatesPlacement()) << "an uncomposed field would draw grass as if neutral";
    Settle();
    EXPECT_FALSE(Data->GrassField.GatesPlacement()) << "a composed neutral field holds grass back";
    const auto region = Region(Components::TerrainVolumeShape::Circle, 0.2f, 0.4f, -30);
    Settle();
    const auto pixels = Data->GrassField.Pixels;
    ASSERT_EQ(At(-30, 0), 51);
    HoldPool();
    World.GetComponentForWrite<Components::WorldTransform>(region)->matrix[12] = 30;
    Tick();
    EXPECT_EQ(Data->GrassField.Pixels, pixels) << "the previous field left before the new one was ready";
    EXPECT_TRUE(Data->GrassField.GatesPlacement());
    Settle();
    EXPECT_EQ(At(-30, 0), 255);
    EXPECT_EQ(At(30, 0), 51);
}

TEST_F(TerrainGrassAsyncCompose, ATerrainRemovedDuringTheComposeIsSkipped)
{
    ASSERT_FALSE(Data->GrassField.Initialized) << "no compose in flight to outlive the terrain";
    World.DestroyEntity(TerrainEntity);
    TerrainService::Get().DestroyTerrain(Handle);
    Data = nullptr;
    Settle(); // the result names a terrain that no longer exists
    EXPECT_EQ(TerrainService::Get().GetTerrainData(Handle), nullptr);
}

TEST_F(TerrainGrassAsyncCompose, AnAtlasTerrainsFlagAndATileStreamedInMeanwhileFollowTheSwap)
{
    auto& service = TerrainService::Get();
    TiledTerrainConfig config{};
    config.WorldSizeX = 9216; config.WorldSizeZ = 1024;
    config.HeightScale = 32; config.SamplesPerMeter = 1;
    const auto handle = service.CreateTiledTerrain(config);
    auto* tiled = service.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    const auto entity = World.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = 9216; terrain.SizeZ = 1024; terrain.HeightScale = 32;
    terrain.TiledTerrainHandle = handle.Index; terrain.TiledTerrainGeneration = handle.Generation;
    World.AddComponentImmediate<Components::Terrain>(entity, terrain);
    World.AddComponentImmediate<Components::WorldTransform>(entity, {});
    Settle();
    ASSERT_TRUE(tiled->GrassCoarseField.Initialized);
    ASSERT_FALSE(tiled->GrassRegionsActive);
    HoldPool();
    Region(Components::TerrainVolumeShape::Global, .2f, .4f);
    Tick();
    EXPECT_FALSE(tiled->GrassRegionsActive) << "the flag changed before the fields it describes";
    auto* tile = service.LoadTile(handle, {3,0});
    ASSERT_NE(tile, nullptr);
    tile->LodState = TileLodState::Full;
    Tick(); // the tile arrives while the compose is held
    Settle();
    EXPECT_TRUE(tiled->GrassRegionsActive);
    ASSERT_TRUE(tile->GrassField.IsActive()) << "a tile that streamed in during the compose was never composed";
    EXPECT_EQ(tile->GrassField.Pixels.front(), 51);
    World.DestroyEntityImmediate(entity);
}

TEST_F(TerrainGrassAsyncCompose, ARegionlessTerrainHasItsFieldAfterItsFirstUpdate)
{
    World.DestroyEntityImmediate(StackRegion);
    ASSERT_FALSE(Data->GrassField.Initialized);
    TerrainModifierSystem system;
    system.Update(World, 1.0f / 60.0f); // the pool's one worker is still held
    EXPECT_TRUE(Data->GrassField.Initialized) << "a terrain with no grass regions waited for a job";
    EXPECT_FALSE(Data->GrassField.GatesPlacement());
}

TEST_F(TerrainGrassAsyncCompose, ASystemDestroyedDuringTheComposeDoesNotWaitForIt)
{
    auto system = std::make_unique<TerrainModifierSystem>();
    system->Update(World, 1.0f / 60.0f); // requests a field while the worker is held
    system.reset();                      // cancels the queued compose instead of waiting on the held worker
    ReleasePool();
    EXPECT_FALSE(Data->GrassField.Initialized) << "the destroyed system's result was applied";
}

// The swap moves the job's bytes into the field unchanged: with a spline area, a spline path and a
// circle edited while the compose is held, the settled field equals a serial compose of the same
// stack on a pool-less system.
TEST_F(TerrainGrassAsyncCompose, TheSettledFieldIsTheSerialComposeByteForByte)
{
    Settle();
    HoldPool();
    const auto area = Region(Components::TerrainVolumeShape::SplineArea, 0.5f, 0.3f);
    AddClosedSpline(area);
    const auto path = Region(Components::TerrainVolumeShape::SplinePath, 0.1f, 0.7f, 0, 5);
    AddClosedSpline(path);
    const auto circle = Region(Components::TerrainVolumeShape::Circle, 0.0f, 0.2f, 30, 10);
    Tick();
    World.GetComponentForWrite<Components::WorldTransform>(circle)->matrix[12] = -20;
    Tick();
    World.GetComponentForWrite<Components::TerrainGrassEffect>(area)->HeightScale = 0.25f;
    Tick();
    Settle();
    const auto settled = Data->GrassField.Pixels;
    ASSERT_FALSE(settled.empty());

    World.SetJobSystem(nullptr);
    TerrainModifierSystem serial;
    Data->GrassField = {};
    World.SwapLifecycleEvents();
    serial.Update(World, 1.0f / 60.0f);
    ASSERT_TRUE(Data->GrassField.Initialized);
    EXPECT_EQ(Data->GrassField.Pixels, settled);
}

// The running job reads only its own copy of the stack. A system and a terrain with eight winding
// spline routes are destroyed while their compose runs; the gather's spline arena goes with the
// system. The red needs a use-after-free detector (the -asan preset, or MallocScribble on macOS):
// freed spline memory the job still reads is otherwise left intact and reads as valid splines.
TEST_F(TerrainGrassAsyncCompose, ASystemAndTerrainDestroyedWhileTheComposeRunsLeaveTheJobItsOwnStack)
{
    Settle();
    constexpr uint32 kDim = 257;
    constexpr float32 kSize = 256.0f, kX = 1000.0f;
    constexpr int kRoutes = 8;
    constexpr auto kSleep = std::chrono::milliseconds(30);
    Terrain::TerrainConfig config{};
    config.HeightmapWidth = config.HeightmapHeight = kDim;
    config.WorldSizeX = config.WorldSizeZ = kSize;
    config.HeightScale = 32;
    config.LODLevels = 3;
    const auto handle = TerrainService::Get().CreateTerrain(config);
    const auto entity = World.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = terrain.SizeZ = kSize;
    terrain.HeightScale = 32;
    terrain.BaseSource = Components::TerrainBaseSource::Flat;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    World.AddComponentImmediate<Components::Terrain>(entity, terrain);
    Components::WorldTransform transform{};
    transform.matrix[12] = kX;
    World.AddComponentImmediate<Components::WorldTransform>(entity, transform);
    auto& splines = SplineECS::SplineService::Get();
    for (int route = 0; route < kRoutes; ++route)
    {
        const auto region = Region(Components::TerrainVolumeShape::SplinePath, 0.3f, 0.2f, 0);
        World.GetComponentForWrite<Components::TerrainModifierVolume>(region)->Falloff = 40;
        const auto spline = splines.CreateSpline(Spline::SplineType::CatmullRom, false);
        for (int i = 0; i < 24; ++i)
        {
            const float32 t = static_cast<float32>(i) / 23.0f;
            const float32 z = kSize * (-0.45f + 0.9f * (route + 0.5f) / kRoutes) + 20 * std::sin(t * 9 + route);
            splines.GetSplineData(spline)->AddPoint({kX - kSize * 0.5f + kSize * t, 0, z}, 3);
        }
        splines.RebuildCache(spline);
        Components::SplineComponent component{};
        component.SplineDataIndex = spline.Index();
        component.SplineDataGeneration = spline.Generation();
        World.AddComponentImmediate<Components::SplineComponent>(region, component);
    }

    // The red needs the job still running when the system goes: time one compose of this scene.
    {
        TerrainModifierSystem timing;
        timing.Update(World, 1.0f / 60.0f);
        const auto start = std::chrono::steady_clock::now();
        timing.WaitForGrassComposeForTests();
        const auto composeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        RecordProperty("composeMs", std::to_string(composeMs));
        if (composeMs < 10.0 * kSleep.count())
            GTEST_SKIP() << "the compose took " << composeMs << " ms, too short to destroy the system while it runs";
    }

    auto system = std::make_unique<TerrainModifierSystem>();
    system->Update(World, 1.0f / 60.0f);
    std::this_thread::sleep_for(kSleep);
    system.reset();
    World.DestroyEntity(entity);
    TerrainService::Get().DestroyTerrain(handle);
    Settle();
    EXPECT_EQ(TerrainService::Get().GetTerrainData(handle), nullptr);
    EXPECT_TRUE(Data->GrassField.Initialized);
}

TEST_F(TerrainGrassRegions, DefaultAndRepeatedNeutralEffectsAllocateNothing)
{
    Region(Components::TerrainVolumeShape::Global, 1, 1);
    Tick();
    EXPECT_TRUE(Data->GrassField.Initialized);
    EXPECT_TRUE(Data->GrassField.Pixels.empty());
    EXPECT_EQ(Data->GrassField.Pixels.capacity(), 0u);
    const auto version = Data->GrassField.Version;
    for (int frame = 0; frame < 4; ++frame) Tick();
    EXPECT_EQ(Data->GrassField.Version, version);
    EXPECT_FALSE(Data->GrassField.Dirty);
}

TEST_F(TerrainGrassRegions, OrderedTargetLerpLetsLaterNeutralRestoreBaseline)
{
    Region(Components::TerrainVolumeShape::Global, 0, 0.25f);
    const auto restoring = Region(Components::TerrainVolumeShape::Global, 1, 1, 0, 10);
    World.GetComponentForWrite<Components::TerrainModifierVolume>(restoring)->Weight = 0.5f;
    Tick();
    EXPECT_EQ(At(0, 0), 128);
    EXPECT_EQ(At(0, 0, 1), 159);
    World.GetComponentForWrite<Components::TerrainModifierVolume>(restoring)->Weight = 1;
    Tick();
    EXPECT_FALSE(Data->GrassField.IsActive());
    World.GetComponentForWrite<Components::TerrainModifierVolume>(restoring)->Priority = -10;
    Tick();
    EXPECT_EQ(At(0, 0), 0);
    EXPECT_EQ(At(0, 0, 1), 64);
}

TEST_F(TerrainGrassRegions, MovedRegionRestoresOldBoundsAndIdleRetainsVersion)
{
    const auto region = Region(Components::TerrainVolumeShape::Circle, 0.2f, 0.4f, -30);
    Tick();
    EXPECT_EQ(At(-30, 0), 51);
    EXPECT_EQ(At(30, 0), 255);
    Data->GrassField.Dirty = false; // extraction consumed this version
    World.GetComponentForWrite<Components::WorldTransform>(region)->matrix[12] = 30;
    Tick();
    EXPECT_EQ(At(-30, 0), 255);
    EXPECT_EQ(At(30, 0), 51);
    EXPECT_GT(Data->GrassField.DirtyMinX, 0u);
    EXPECT_LT(Data->GrassField.DirtyMaxX, 65u);
    EXPECT_TRUE(Data->GrassField.Dirty);
    Data->GrassField.Dirty = false;
    const auto version = Data->GrassField.Version;
    Tick(); Tick();
    EXPECT_EQ(Data->GrassField.Version, version);
    EXPECT_FALSE(Data->GrassField.Dirty);
}

TEST_F(TerrainGrassRegions, DisableAndRemovalRestoreNeutralWithoutGroundDirtying)
{
    const auto region = Region(Components::TerrainVolumeShape::Circle, 0, 0);
    Tick();
    const auto groundVersion = Data->HeightfieldVersion;
    Data->SplatmapDirty = false;
    World.GetComponentForWrite<Components::TerrainGrassEffect>(region)->Enabled = false;
    Tick();
    EXPECT_FALSE(Data->GrassField.IsActive());
    EXPECT_EQ(Data->HeightfieldVersion, groundVersion);
    EXPECT_FALSE(Data->SplatmapDirty);
    World.GetComponentForWrite<Components::TerrainGrassEffect>(region)->Enabled = true;
    Tick();
    ASSERT_TRUE(Data->GrassField.IsActive());
    ECS::Entity(&World, region).SetEnabled<Components::TerrainModifierVolume>(false);
    Tick();
    EXPECT_FALSE(Data->GrassField.IsActive());
    ECS::Entity(&World, region).SetEnabled<Components::TerrainModifierVolume>(true);
    Tick();
    ASSERT_TRUE(Data->GrassField.IsActive());
    World.RemoveComponentImmediate<Components::TerrainGrassEffect>(region);
    Tick();
    EXPECT_FALSE(Data->GrassField.IsActive());
    EXPECT_EQ(Data->HeightfieldVersion, groundVersion);
    EXPECT_FALSE(Data->SplatmapDirty);
}

TEST_F(TerrainGrassRegions, GrassEditOnMixedVolumePreservesHeightSplatAndNormals)
{
    const auto region = Region(Components::TerrainVolumeShape::Circle, 0.5f, 0.75f);
    Components::TerrainHeightOffsetEffect offset{};
    offset.Offset = 3;
    World.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(region, offset);
    Tick();
    const auto groundVersion = Data->HeightfieldVersion;
    const std::vector<float32> heights(Data->Heightfield.GetRawSamples(),
        Data->Heightfield.GetRawSamples() + Data->Heightfield.GetSampleCount());
    const auto splat = Data->Splatmap;
    const auto normals = Data->Normalmap;
    Data->SplatmapDirty = false;
    auto* effect = World.GetComponentForWrite<Components::TerrainGrassEffect>(region);
    effect->HeightScale = 0.1f;
    effect->DensityScale = 0.2f;
    effect->StackOrder = 12;
    Tick();
    EXPECT_EQ(At(0,0), 26);
    EXPECT_EQ(Data->HeightfieldVersion, groundVersion);
    EXPECT_TRUE(std::equal(heights.begin(), heights.end(), Data->Heightfield.GetRawSamples()));
    EXPECT_EQ(Data->Splatmap, splat);
    EXPECT_EQ(Data->Normalmap, normals);
    EXPECT_FALSE(Data->SplatmapDirty);
}

TEST_F(TerrainGrassRegions, StructuralGrassEditsPreserveExplicitGroundPriorityOrder)
{
    const auto a = GroundRegion(2, 0);
    const auto b = GroundRegion(6, 10);
    Tick();
    const auto groundVersion = Data->HeightfieldVersion;
    const std::vector<float32> heights(Data->Heightfield.GetRawSamples(),
        Data->Heightfield.GetRawSamples() + Data->Heightfield.GetSampleCount());
    const auto splat = Data->Splatmap;
    const auto normals = Data->Normalmap;
    Components::TerrainGrassEffect grass{};
    grass.HeightScale = .2f;
    for (const auto entity : {a, b})
    {
        World.AddComponentImmediate<Components::TerrainGrassEffect>(entity, grass);
        Tick();
        EXPECT_EQ(Data->HeightfieldVersion, groundVersion);
    }
    for (const auto entity : {b, a})
    {
        World.RemoveComponentImmediate<Components::TerrainGrassEffect>(entity);
        Tick();
        EXPECT_EQ(Data->HeightfieldVersion, groundVersion);
    }
    EXPECT_TRUE(std::equal(heights.begin(), heights.end(), Data->Heightfield.GetRawSamples()));
    EXPECT_EQ(Data->Splatmap, splat);
    EXPECT_EQ(Data->Normalmap, normals);
}

TEST_F(TerrainGrassRegions, StructuralGrassPresenceRetainsExistingEqualPriorityGroundTieSemantics)
{
    // Moving the first of three entities also swap-removes the last one from
    // its old archetype, making the tie-order change observable.
    const auto a = GroundRegion(2, 0);
    GroundRegion(4, 0);
    GroundRegion(6, 0);
    Tick();
    const auto before = Data->Heightfield.GetRawSamples()[0];
    World.AddComponentImmediate<Components::TerrainGrassEffect>(a, {});
    Tick();
    const auto afterAdd = Data->Heightfield.GetRawSamples()[0];
    ASSERT_NE(before, afterAdd) << "fixture must actually change the equal-priority archetype gather order";
    World.RemoveComponentImmediate<Components::TerrainGrassEffect>(a);
    Tick();
    EXPECT_NE(Data->Heightfield.GetRawSamples()[0], afterAdd)
        << "removal must also exercise the existing structural tie order";
    // Presence is a structural input to existing ground tie ordering. Consumers
    // may ignore grass scalar edits, but cannot omit this presence dependency.
}

TEST_F(TerrainGrassRegions, CircleRectangleAndBothSplineShapesUseExistingFootprints)
{
    const auto region = Region(Components::TerrainVolumeShape::Circle, 0, 0);
    Tick();
    EXPECT_EQ(At(0,0), 0);
    EXPECT_EQ(At(12,12), 255); // outside radius + feather
    World.GetComponentForWrite<Components::TerrainModifierVolume>(region)->Shape = Components::TerrainVolumeShape::Rectangle;
    Tick();
    EXPECT_LT(At(10,10), 255);
    AddClosedSpline(region);
    World.GetComponentForWrite<Components::TerrainModifierVolume>(region)->Shape = Components::TerrainVolumeShape::SplineArea;
    Tick();
    EXPECT_EQ(At(0,0), 0);
    EXPECT_EQ(At(60,60), 255);
    World.GetComponentForWrite<Components::TerrainModifierVolume>(region)->Shape = Components::TerrainVolumeShape::SplinePath;
    Tick();
    EXPECT_EQ(At(0,0), 255);
    EXPECT_GT(Data->GrassField.NonNeutralTexels, 0u);
}

TEST_F(TerrainGrassRegions, SplinePointEditRestoresOldAreaWithoutChangingGround)
{
    const auto region = Region(Components::TerrainVolumeShape::SplineArea, 0, 0);
    const auto handle = AddClosedSpline(region);
    Tick();
    ASSERT_EQ(At(0, 0), 0);
    const auto groundVersion = Data->HeightfieldVersion;
    const auto grassVersion = Data->GrassField.Version;
    auto& service = SplineECS::SplineService::Get();
    auto* spline = service.GetSplineData(handle);
    ASSERT_NE(spline, nullptr);
    for (uint32 i = 0; i < spline->Points.size(); ++i)
    {
        auto point = spline->Points[i].Position;
        point.x += 100;
        service.GetSplineData(handle)->SetPointPosition(i, point);
    }
    service.RebuildCache(handle);
    Tick();
    EXPECT_EQ(At(0, 0), 255);
    EXPECT_GT(Data->GrassField.Version, grassVersion);
    EXPECT_EQ(Data->HeightfieldVersion, groundVersion);
}

TEST_F(TerrainGrassRegions, DisabledTerrainReentryClearsRegionRemovedWhileSuspended)
{
    const auto region = Region(Components::TerrainVolumeShape::Circle, 0, 0);
    Tick();
    ASSERT_TRUE(Data->GrassField.IsActive());
    World.AddComponentImmediate<ECS::Disabled>(TerrainEntity, {});
    Tick();
    World.DestroyEntityImmediate(region);
    Tick();
    ASSERT_TRUE(Data->GrassField.IsActive()); // excluded owner retains its cache
    World.RemoveComponentImmediate<ECS::Disabled>(TerrainEntity);
    Tick();
    EXPECT_FALSE(Data->GrassField.IsActive());
}

TEST_F(TerrainGrassRegions, UnrelatedRemovalReconcilesEqualPriorityStorageReorder)
{
    const auto far = Region(Components::TerrainVolumeShape::Circle, .7f, .7f, 500);
    Region(Components::TerrainVolumeShape::Circle, .2f, .2f);
    Region(Components::TerrainVolumeShape::Circle, .8f, .8f);
    Tick();
    const auto original = Data->GrassField.Pixels;
    World.DestroyEntityImmediate(far); // swap-remove can reorder the two survivors
    Tick();
    const auto incremental = Data->GrassField.Pixels;
    ASSERT_NE(original, incremental) << "fixture must actually reorder the overlapping survivors";
    TerrainModifierSystem fresh;
    fresh.Update(World, 1.0f/60.0f);
    EXPECT_EQ(Data->GrassField.Pixels, incremental);
}

TEST_F(TerrainGrassRegions, PlanetDoesNotAllocateRegionalGrass)
{
    World.GetComponentForWrite<Components::Terrain>(TerrainEntity)->Domain = Components::TerrainDomain::Spherical;
    Region(Components::TerrainVolumeShape::Global, 0, 0);
    Tick();
    EXPECT_FALSE(Data->GrassField.IsActive());
}

TEST_F(TerrainGrassRegions, TiledMoveUsesCurrentAuthoredOriginBeforeExtraction)
{
    auto& service = TerrainService::Get();
    TiledTerrainConfig config{};
    config.WorldSizeX = config.WorldSizeZ = 128;
    config.SamplesPerMeter = 1;
    const auto handle = service.CreateTiledTerrain(config);
    auto* tiled = service.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    const auto entity = World.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = terrain.SizeZ = 128;
    terrain.TiledTerrainHandle = handle.Index; terrain.TiledTerrainGeneration = handle.Generation;
    World.AddComponentImmediate<Components::Terrain>(entity, terrain);
    World.AddComponentImmediate<Components::WorldTransform>(entity, {});
    Region(Components::TerrainVolumeShape::Circle, 0, 0);
    Tick();
    ASSERT_TRUE(tiled->GrassUnifiedField.IsActive());
    World.GetComponentForWrite<Components::WorldTransform>(entity)->matrix[12] = 256;
    // Do not pre-update service.WorldOrigin: actual extraction runs after modifiers.
    Tick();
    EXPECT_FALSE(tiled->GrassUnifiedField.IsActive());
}

TEST_F(TerrainGrassRegions, NonMultipleTiledSizeUsesTheRenderCoverageLattice)
{
    auto& service = TerrainService::Get();
    TiledTerrainConfig config{};
    config.WorldSizeX = 1500;
    config.WorldSizeZ = 1200;
    config.SamplesPerMeter = 1;
    const auto handle = service.CreateTiledTerrain(config);
    auto* tiled = service.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    const auto entity = World.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = 1500; terrain.SizeZ = 1200;
    terrain.TiledTerrainHandle = handle.Index; terrain.TiledTerrainGeneration = handle.Generation;
    World.AddComponentImmediate<Components::Terrain>(entity, terrain);
    World.AddComponentImmediate<Components::WorldTransform>(entity, {});
    Region(Components::TerrainVolumeShape::Circle, 0, .4f, 250);
    Tick();
    const auto extent = ComputeTiledRenderExtent(tiled->Config);
    ASSERT_EQ(extent.WorldSizeX, 2048);
    ASSERT_EQ(extent.WorldSizeZ, 2048);
    const auto& field = tiled->GrassUnifiedField;
    ASSERT_TRUE(field.IsActive());
    ASSERT_EQ(field.Width, 2049u);
    ASSERT_EQ(field.Height, 2049u);
    // Renderer origin is the requested centre minus half requested size;
    // subsequent UV spacing covers the complete tile lattice at one metre.
    const size_t centre = (600u * field.Width + 1000u) * 2;
    EXPECT_EQ(field.Pixels[centre], 0);
    EXPECT_EQ(field.Pixels[centre + 1], 102);
    EXPECT_EQ(field.Pixels[(600u * field.Width + 750u) * 2], 255);
}

TEST_F(TerrainGrassRegions, SchemaRoundtripKeepsBoundedTargetsAndRefusesNonfinite)
{
    Scene::EnsureTerrainSceneSchemasRegistered();
    const auto* schema = Scene::SceneSchemaRegistry::Find("TerrainGrassEffect");
    ASSERT_NE(schema, nullptr);
    const auto entity = World.CreateEntity();
    Scene::SceneLoadContext context{};
    std::string error;
    ASSERT_TRUE(schema->ApplyProperty(World, entity, context, "heightscale", "0.25", &error)) << error;
    ASSERT_TRUE(schema->ApplyProperty(World, entity, context, "densityscale", "2", &error)) << error;
    ASSERT_TRUE(schema->ApplyProperty(World, entity, context, "grounddarkeningscale", "0.4", &error)) << error;
    EXPECT_FLOAT_EQ(World.GetComponent<Components::TerrainGrassEffect>(entity)->HeightScale, .25f);
    EXPECT_FLOAT_EQ(World.GetComponent<Components::TerrainGrassEffect>(entity)->DensityScale, 1);
    EXPECT_FALSE(schema->ApplyProperty(World, entity, context, "heightscale", "nan", &error));
    EXPECT_FALSE(schema->ApplyProperty(World, entity, context, "densityscale", "inf", &error));
    EXPECT_FALSE(schema->ApplyProperty(World, entity, context, "grounddarkeningscale", "nan", &error));
    EXPECT_FALSE(schema->ApplyProperty(World, entity, context, "unknown", "1", &error));
    EXPECT_FLOAT_EQ(Components::ClampTerrainGrassEffectScale(std::numeric_limits<float32>::quiet_NaN()), 1);
    EXPECT_FLOAT_EQ(Components::ClampTerrainGrassEffectScale(-1), 0);
    ECS::World authored;
    const auto source = authored.CreateEntity();
    authored.AddComponentImmediate<Components::TerrainGrassEffect>(source,
        *World.GetComponent<Components::TerrainGrassEffect>(entity));
    authored.AddComponentImmediate<Components::TerrainModifierVolume>(source, {});
    const auto file = std::filesystem::temp_directory_path() / ("GrassRegionScene-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(authored, file)) << Scene::GetLastSceneIOError().message;
    ECS::World loaded;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions options; options.outDegradation = &degradation;
    const bool success = Scene::LoadSceneFromFile(loaded, file, options);
    std::error_code ignored; std::filesystem::remove(file, ignored);
    ASSERT_TRUE(success) << Scene::GetLastSceneIOError().message;
    EXPECT_FALSE(degradation.IsDegraded());
    uint32 found = 0;
    loaded.Query<ECS::Read<Components::TerrainGrassEffect>>()
        .Each([&](const Components::TerrainGrassEffect& effect) {
            ++found;
            EXPECT_FLOAT_EQ(effect.HeightScale, .25f);
            EXPECT_FLOAT_EQ(effect.DensityScale, 1);
            EXPECT_FLOAT_EQ(effect.GroundDarkeningScale, .4f);
        });
    EXPECT_EQ(found, 1u);
}

TEST_F(TerrainGrassRegions, ColorEffectSchemaRoundtripClampsAndRefusesNonfinite)
{
    Scene::EnsureTerrainSceneSchemasRegistered();
    const auto* schema = Scene::SceneSchemaRegistry::Find("TerrainColorEffect");
    ASSERT_NE(schema, nullptr);
    const auto entity = World.CreateEntity();
    Scene::SceneLoadContext context{};
    std::string error;
    ASSERT_TRUE(schema->ApplyProperty(World, entity, context, "colorr", "0.3", &error)) << error;
    ASSERT_TRUE(schema->ApplyProperty(World, entity, context, "colorg", "2", &error)) << error;
    ASSERT_TRUE(schema->ApplyProperty(World, entity, context, "colorb", "-1", &error)) << error;
    ASSERT_TRUE(schema->ApplyProperty(World, entity, context, "strength", "0.6", &error)) << error;
    EXPECT_FALSE(schema->ApplyProperty(World, entity, context, "strength", "nan", &error));
    EXPECT_FALSE(schema->ApplyProperty(World, entity, context, "colorr", "inf", &error));
    EXPECT_FALSE(schema->ApplyProperty(World, entity, context, "unknown", "1", &error));
    ECS::World authored;
    const auto source = authored.CreateEntity();
    authored.AddComponentImmediate<Components::TerrainColorEffect>(source,
        *World.GetComponent<Components::TerrainColorEffect>(entity));
    authored.AddComponentImmediate<Components::TerrainModifierVolume>(source, {});
    const auto file = std::filesystem::temp_directory_path() / ("ColorRegionScene-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(authored, file)) << Scene::GetLastSceneIOError().message;
    ECS::World loaded;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions options; options.outDegradation = &degradation;
    const bool success = Scene::LoadSceneFromFile(loaded, file, options);
    std::error_code ignored; std::filesystem::remove(file, ignored);
    ASSERT_TRUE(success) << Scene::GetLastSceneIOError().message;
    EXPECT_FALSE(degradation.IsDegraded());
    uint32 found = 0;
    loaded.Query<ECS::Read<Components::TerrainColorEffect>>()
        .Each([&](const Components::TerrainColorEffect& effect) {
            ++found;
            EXPECT_FLOAT_EQ(effect.Color.r, .3f);
            EXPECT_FLOAT_EQ(effect.Color.g, 1);
            EXPECT_FLOAT_EQ(effect.Color.b, 0);
            EXPECT_FLOAT_EQ(effect.Strength, .6f);
        });
    EXPECT_EQ(found, 1u);
}

TEST_F(TerrainGrassRegions, TiledUnifiedComposesNeverStreamedGroundAndAtlasCoarseIsAnalytic)
{
    auto& service = TerrainService::Get();
    const auto region = Region(Components::TerrainVolumeShape::Global, .2f, .4f);
    for (const float32 sizeX : {2048.0f, 9216.0f})
    {
        TiledTerrainConfig config{};
        config.WorldSizeX = sizeX; config.WorldSizeZ = 1024;
        config.HeightScale = 32; config.SamplesPerMeter = 1;
        const auto handle = service.CreateTiledTerrain(config);
        auto* tiled = service.GetTiledTerrainData(handle);
        ASSERT_NE(tiled, nullptr);
        ASSERT_TRUE(tiled->Tiles.empty());
        const auto entity = World.CreateEntity();
        Components::Terrain terrain{};
        terrain.SizeX = sizeX; terrain.SizeZ = 1024; terrain.HeightScale = 32;
        terrain.TiledTerrainHandle = handle.Index; terrain.TiledTerrainGeneration = handle.Generation;
        World.AddComponentImmediate<Components::Terrain>(entity, terrain);
        World.AddComponentImmediate<Components::WorldTransform>(entity, {});
        Tick();
        const auto& field = sizeX < 8193 ? tiled->GrassUnifiedField : tiled->GrassCoarseField;
        ASSERT_TRUE(field.IsActive());
        EXPECT_EQ(field.Pixels.front(), 51);
        EXPECT_EQ(field.Pixels.back(), 102);
        for (size_t i = 0; i < field.Pixels.size(); i += 2)
        {
            ASSERT_EQ(field.Pixels[i], 51);
            ASSERT_EQ(field.Pixels[i+1], 102);
        }
        EXPECT_TRUE(tiled->Tiles.empty());
        if (sizeX > 8193)
        {
            auto* tile = service.LoadTile(handle, {3,0});
            ASSERT_NE(tile, nullptr);
            tile->LodState = TileLodState::Full;
            Tick();
            ASSERT_TRUE(tile->GrassField.IsActive());
            EXPECT_EQ(tile->GrassField.Pixels.front(), 51);
            const auto version = tile->HeightfieldVersion;
            World.GetComponentForWrite<Components::TerrainGrassEffect>(region)->HeightScale = .6f;
            Tick();
            EXPECT_EQ(tile->GrassField.Pixels.front(), 153);
            EXPECT_EQ(tile->HeightfieldVersion, version);
            auto* late = service.LoadTile(handle, {6,0});
            ASSERT_NE(late, nullptr);
            late->LodState = TileLodState::Full;
            Tick();
            ASSERT_TRUE(late->GrassField.IsActive());
            EXPECT_EQ(late->GrassField.Pixels.front(), 153)
                << "a previously nonresident tile must receive edits made before it arrived";
            EXPECT_EQ(tile->GrassField.Pixels.front(), 153);
        }
        World.DestroyEntityImmediate(entity);
    }
}

TEST(TerrainGrassField, TwoChannelAtlasPackingPreservesSharedEdgesAndApron)
{
    const auto geometry = MakeAtlasGeometry(5, 4, 2, 2);
    std::vector<uint8> samples(5 * 5 * 2);
    for (uint32 z = 0; z < 5; ++z)
        for (uint32 x = 0; x < 5; ++x)
        {
            samples[(z*5+x)*2] = static_cast<uint8>(x*30);
            samples[(z*5+x)*2+1] = static_cast<uint8>(z*40);
        }
    std::vector<uint8> full(geometry.AtlasDim * geometry.AtlasDim * 2), patch;
    PackTileBytesIntoSlot(full.data(), geometry, 0, samples.data(), 2);
    samples[0] = 99;
    const auto rect = PackTileEditRegionIntoSlot(patch, samples.data(), 2, geometry, 0, 0, 0, 0, 0);
    EXPECT_EQ(rect.DstTexelX, 0u);
    EXPECT_EQ(rect.DstTexelY, 0u);
    EXPECT_EQ(rect.Width, 2u);
    EXPECT_EQ(rect.Height, 2u);
    for (size_t i = 0; i < patch.size(); i += 2) EXPECT_EQ(patch[i], 99);
    const auto plainSlots = DeriveAtlasSlotCount(32, 32, 1025, kAtlasVramBudgetBytes);
    const auto grassSlots = DeriveAtlasSlotCount(32, 32, 1025, kAtlasVramBudgetBytes, 14);
    EXPECT_LE(grassSlots, plainSlots);
    const auto active = MakeAtlasGeometry(1025, grassSlots, 32, 32);
    EXPECT_LE(static_cast<uint64>(active.AtlasDim) * active.AtlasDim * 14, kAtlasVramBudgetBytes);
}

// A grass region volume as the gather resolves it, with the gather's world bounds.
ResolvedModifier GrassVolume(Components::TerrainModifierShape shape, float32 height, float32 density,
                             float32 falloff, const Spline::SplineData* spline = nullptr, bool fillInterior = false)
{
    ResolvedModifier mod{};
    mod.ModType = ResolvedModifier::Type::Volume;
    mod.Shape = shape;
    mod.Enabled = true;
    mod.Falloff = falloff;
    mod.FalloffInward = 0.4f;
    mod.ResolvedSpline = spline;
    mod.SplineFillInterior = fillInterior;
    ResolvedEffect effect;
    effect.EffectKind = ResolvedEffect::Kind::Grass;
    effect.Grass = {height, density};
    mod.Effects.push_back(effect);
    if (spline)
    {
        mod.BoundsMinX = mod.BoundsMinZ = std::numeric_limits<float32>::max();
        mod.BoundsMaxX = mod.BoundsMaxZ = -std::numeric_limits<float32>::max();
        for (const auto& box : spline->SegmentBounds)
        {
            mod.BoundsMinX = std::min(mod.BoundsMinX, box.min.x - falloff);
            mod.BoundsMinZ = std::min(mod.BoundsMinZ, box.min.z - falloff);
            mod.BoundsMaxX = std::max(mod.BoundsMaxX, box.max.x + falloff);
            mod.BoundsMaxZ = std::max(mod.BoundsMaxZ, box.max.z + falloff);
        }
    }
    return mod;
}

// A spline region's weight from its exact signed distance, never cut off.
float32 ExactSplineWeight(const ResolvedModifier& mod, float32 worldX, float32 worldZ)
{
    constexpr float32 kExact = std::numeric_limits<float32>::infinity();
    const float32 sd = mod.SplineFillInterior
        ? Spline::SignedDistanceToSplineXZ(*mod.ResolvedSpline, worldX, worldZ, kExact)
        : Spline::SignedDistanceToSplineBandXZ(*mod.ResolvedSpline, worldX, worldZ, kExact);
    return VolumeShapeWeight(mod, -sd);
}

// The field a texel-by-texel walk of the whole ordered stack gives, every spline measured exactly
// (no cutoff): what ComposeTerrainGrassField must equal byte for byte.
std::vector<uint8> ReferenceGrassField(uint32 dim, float32 size, float32 origin,
                                       std::span<const ResolvedModifier> modifiers)
{
    std::vector<uint8> pixels(static_cast<size_t>(dim) * dim * kTerrainGrassFieldChannels, 255);
    for (uint32 z = 0; z < dim; ++z)
        for (uint32 x = 0; x < dim; ++x)
        {
            const float32 worldX = origin + size * (static_cast<float32>(x) / (dim - 1));
            const float32 worldZ = origin + size * (static_cast<float32>(z) / (dim - 1));
            float32 heightScale = 1.0f, densityScale = 1.0f;
            for (const auto& mod : modifiers)
            {
                if (!mod.Enabled || worldX < mod.BoundsMinX || worldX > mod.BoundsMaxX
                    || worldZ < mod.BoundsMinZ || worldZ > mod.BoundsMaxZ) continue;
                const float32 rawWeight = mod.ResolvedSpline ? ExactSplineWeight(mod, worldX, worldZ)
                                                             : ComputeWeight(mod, worldX, worldZ);
                const float32 linear = std::isfinite(rawWeight) ? Math::Clamp01(rawWeight) : 0.0f;
                if (linear <= 0) continue;
                const float32 weight = linear * linear * (3.0f - 2.0f * linear);
                for (const auto& fx : mod.Effects)
                {
                    heightScale += (Components::ClampTerrainGrassEffectScale(fx.Grass.HeightScale) - heightScale) * weight;
                    densityScale += (Components::ClampTerrainGrassEffectScale(fx.Grass.DensityScale) - densityScale) * weight;
                }
            }
            const size_t offset = (static_cast<size_t>(z) * dim + x) * kTerrainGrassFieldChannels;
            pixels[offset] = static_cast<uint8>(std::lround(Math::Clamp01(heightScale) * 255.0f));
            pixels[offset + 1] = static_cast<uint8>(std::lround(Math::Clamp01(densityScale) * 255.0f));
        }
    return pixels;
}

// The compose skips work three ways: rows a region's Z extent misses, texels its X extent misses,
// and spline samples whose segment bounds are all beyond the falloff. None may change a byte:
// tolerance 0 against the exhaustive walk, over a stack that mixes a winding route at two widths
// (a road and its verge), a closed loop as both a ring and an area, a circle, a rotated rectangle
// and a disabled region, overlapping in stack order. Composing in row bands across a pool gives the
// same bytes.
TEST(TerrainGrassField, ComposeEqualsTheExhaustiveWalkOfEveryRegion)
{
    Spline::SplineData route;
    route.Type = Spline::SplineType::CatmullRom;
    for (uint32 i = 0; i < 24; ++i)
    {
        const float32 t = static_cast<float32>(i) / 23.0f;
        route.AddPoint({-110.0f + 220.0f * t, 0.0f, 70.0f * std::sin(t * 9.0f)}, 2.45f + 0.4f * std::cos(t * 13.0f));
    }
    Spline::RebuildSplineCache(route);
    Spline::SplineData verge = route;
    for (auto& point : verge.Points) point.Radius += 0.2f;
    Spline::RebuildSplineCache(verge);
    Spline::SplineData loop;
    loop.Type = Spline::SplineType::CatmullRom;
    loop.Closed = true;
    for (const auto& p : std::array<Mathematics::Vector3, 5>{{{-60,0,-60},{20,0,-80},{70,0,-10},{30,0,60},{-50,0,40}}})
        loop.AddPoint(p, 3.0f);
    Spline::RebuildSplineCache(loop);

    std::vector<ResolvedModifier> modifiers;
    modifiers.push_back(GrassVolume(Components::TerrainModifierShape::Spline, 0.2f, 0.6f, 2.2f, &verge));
    modifiers.push_back(GrassVolume(Components::TerrainModifierShape::Spline, 0.0f, 0.1f, 0.9f, &route));
    modifiers.push_back(GrassVolume(Components::TerrainModifierShape::Spline, 0.5f, 0.5f, 4.0f, &loop, true));
    modifiers.push_back(GrassVolume(Components::TerrainModifierShape::Spline, 0.9f, 0.3f, 3.0f, &loop));
    auto circle = GrassVolume(Components::TerrainModifierShape::Circle, 1.0f, 1.0f, 3.0f);
    circle.Position = {10, 0, 5}; circle.Radius = 9;
    circle.BoundsMinX = 10 - 12; circle.BoundsMaxX = 10 + 12;
    circle.BoundsMinZ = 5 - 12; circle.BoundsMaxZ = 5 + 12;
    modifiers.push_back(circle);
    auto rectangle = GrassVolume(Components::TerrainModifierShape::Rectangle, 0.3f, 0.0f, 2.0f);
    rectangle.Position = {-40, 0, 30}; rectangle.RectHalfX = 14; rectangle.RectHalfZ = 5;
    rectangle.YawRadians = 0.6f;
    rectangle.BoundsMinX = -40 - 20; rectangle.BoundsMaxX = -40 + 20;
    rectangle.BoundsMinZ = 30 - 20; rectangle.BoundsMaxZ = 30 + 20;
    modifiers.push_back(rectangle);
    auto disabled = GrassVolume(Components::TerrainModifierShape::Circle, 0.0f, 0.0f, 1.0f);
    disabled.Enabled = false;
    disabled.Position = {60, 0, 60}; disabled.Radius = 20;
    disabled.BoundsMinX = disabled.BoundsMinZ = 39; disabled.BoundsMaxX = disabled.BoundsMaxZ = 81;
    modifiers.push_back(disabled);

    // Past kParallelBakeMinTexels, so the pooled compose below really runs in row bands.
    constexpr uint32 kDim = 321;
    constexpr float32 kSize = 240.0f, kOrigin = -120.0f;
    static_assert(static_cast<uint64>(kDim) * kDim >= kParallelBakeMinTexels);
    TerrainGrassField field;
    ComposeTerrainGrassField(field, kDim, kDim, kSize, kSize, kOrigin, kOrigin, modifiers);
    ASSERT_TRUE(field.IsActive());
    EXPECT_EQ(field.Pixels, ReferenceGrassField(kDim, kSize, kOrigin, modifiers));

    JobSystem::WorkStealingThreadPool pool(4);
    TerrainGrassField banded;
    ComposeTerrainGrassField(banded, kDim, kDim, kSize, kSize, kOrigin, kOrigin, modifiers, 0, 0, -1, -1, &pool);
    EXPECT_EQ(banded.Pixels, field.Pixels) << "row bands across the pool changed a byte";
    EXPECT_EQ(banded.NonNeutralTexels, field.NonNeutralTexels);
}
} // namespace
