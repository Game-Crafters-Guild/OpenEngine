// The GPU height bake's refusal report: which modifier keeps a tiled terrain's height bake on the CPU,
// and why. Each test bakes a resident tiled terrain through TerrainModifierSystem::Update and reads
// the report it records. A modifier with several refusals reports the first in the packer's order.

#include <gtest/gtest.h>

#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "SplineECS/SplineService.h"
#include "TerrainECS/TerrainGpuBake.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainTestHelpers.h"

using namespace GameEngine;
using namespace GameEngine::TerrainECS;

namespace
{

using Reason = GpuHeightBakeRefusalReason;
using Components::TerrainModifierBlend;

constexpr float32 kTileWorld = 1024.0f;
constexpr float32 kHeightScale = 64.0f;

// One resident tiled terrain of one tile and its entity. The GPU bake's own checks on the terrain
// pass, so the modifiers decide the path.
class TerrainGpuBakeRefusal : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
        SplineECS::SplineService::Initialize();
        SetGpuHeightBakeEnabledForTests(1);

        TiledTerrainConfig config{};
        config.Base = TerrainTests::GeneratedNoiseBase();
        config.WorldSizeX = kTileWorld;
        config.WorldSizeZ = kTileWorld;
        config.HeightScale = kHeightScale;
        config.SamplesPerMeter = 1.0f;
        m_Handle = TerrainService::Get().CreateTiledTerrain(config);
        TerrainService::Get().LoadTile(m_Handle, TileCoord{0, 0})->LodState = TileLodState::Full;
        Tiled()->AtlasGpuBakeEligible = true;

        EnableTerrainModifierLifecycleEvents(m_World);
        const ECS::EntityHandle terrainEntity = m_World.CreateEntity();
        Components::Terrain terrain{};
        terrain.SizeX = kTileWorld;
        terrain.SizeZ = kTileWorld;
        terrain.HeightScale = kHeightScale;
        terrain.TiledTerrainHandle = m_Handle.Index;
        terrain.TiledTerrainGeneration = m_Handle.Generation;
        m_World.AddComponentImmediate<Components::Terrain>(terrainEntity, terrain);
        m_World.AddComponentImmediate<Components::WorldTransform>(terrainEntity, Components::WorldTransform{});
    }

    void TearDown() override
    {
        SetGpuHeightBakeEnabledForTests(-1);
        SplineECS::SplineService::Shutdown();
        TerrainService::Shutdown();
    }

    TiledTerrainData* Tiled() { return TerrainService::Get().GetTiledTerrainData(m_Handle); }

    // A circle volume at `priority` with no effects.
    ECS::EntityHandle Circle(float32 priority)
    {
        const ECS::EntityHandle e = m_World.CreateEntity();
        Components::TerrainModifierVolume volume{};
        volume.Shape = Components::TerrainVolumeShape::Circle;
        volume.Radius = 40.0f;
        volume.Falloff = 8.0f;
        volume.Priority = priority;
        m_World.AddComponentImmediate<Components::TerrainModifierVolume>(e, volume);
        Components::WorldTransform transform{};
        transform.Position.X = 500.0;
        transform.Position.Z = 500.0;
        m_World.AddComponentImmediate<Components::WorldTransform>(e, transform);
        return e;
    }

    // A circle volume at `priority` with one flatten.
    ECS::EntityHandle Flatten(float32 priority, TerrainModifierBlend blend, bool fromVolume = false)
    {
        const ECS::EntityHandle e = Circle(priority);
        Components::TerrainFlattenEffect flatten{};
        flatten.Blend = blend;
        flatten.UseVolumeHeight = fromVolume;
        flatten.TargetHeight = 12.0f;
        m_World.AddComponentImmediate<Components::TerrainFlattenEffect>(e, flatten);
        return e;
    }

    // A spline path volume at `priority` with an Average flatten from the volume and a ground claim:
    // the pieces a recipe road provisions.
    ECS::EntityHandle Road(float32 priority)
    {
        auto& splines = SplineECS::SplineService::Get();
        const SplineECS::SplineHandle spline = splines.CreateSpline(Spline::SplineType::CatmullRom, false);
        for (int point = 0; point < 4; ++point)
            splines.GetSplineData(spline)->AddPoint(
                Mathematics::Vector3(300.0f + 100.0f * static_cast<float32>(point), 0.0f, 400.0f), 12.0f);
        splines.RebuildCache(spline);
        const ECS::EntityHandle e = m_World.CreateEntity();
        Components::SplineComponent splineComponent{};
        splineComponent.SplineDataIndex = spline.Index();
        splineComponent.SplineDataGeneration = spline.Generation();
        m_World.AddComponentImmediate<Components::SplineComponent>(e, splineComponent);
        Components::TerrainModifierVolume volume{};
        volume.Shape = Components::TerrainVolumeShape::SplinePath;
        volume.Falloff = 6.0f;
        volume.Priority = priority;
        m_World.AddComponentImmediate<Components::TerrainModifierVolume>(e, volume);
        Components::TerrainFlattenEffect flatten{};
        flatten.Blend = TerrainModifierBlend::Average;
        flatten.UseVolumeHeight = true;
        m_World.AddComponentImmediate<Components::TerrainFlattenEffect>(e, flatten);
        m_World.AddComponentImmediate<Components::TerrainGroundClaimEffect>(e, Components::TerrainGroundClaimEffect{});
        m_World.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
        return e;
    }

    // One engine tick: the lifecycle swap, then the modifier bake.
    const TerrainHeightBakeReport& Bake()
    {
        m_World.SwapLifecycleEvents();
        m_System.Update(m_World, 1.0f / 60.0f);
        return Tiled()->HeightBakeReport;
    }

    // Bakes, expects `volume` to hold the refusal for `reason`, then destroys it.
    void ExpectRefusalThenRemove(ECS::EntityHandle volume, Reason reason)
    {
        const TerrainHeightBakeReport& report = Bake();
        EXPECT_EQ(report.ModifierRefusal, (GpuHeightBakeRefusal{reason, volume})) << GpuHeightBakeRefusalName(reason);
        m_World.DestroyEntityImmediate(volume);
    }

    ECS::World m_World;
    TerrainModifierSystem m_System;

private:
    TiledTerrainHandle m_Handle{};
};

} // namespace

// A circle with a Set flatten packs: the bake takes the GPU path and records no refusal.
TEST_F(TerrainGpuBakeRefusal, AnExpressibleCircleBakesOnTheGpu)
{
    Flatten(0.0f, TerrainModifierBlend::Set);
    const TerrainHeightBakeReport& report = Bake();
    EXPECT_EQ(report.Path, TerrainHeightBakePath::Gpu);
    EXPECT_EQ(report.FirstRefusal(), GpuHeightBakeRefusal{});
    EXPECT_EQ(report.OtherModifierRefusals, 0u);
}

// The pieces a recipe road provisions: the shape is checked first, so the road reports it.
TEST_F(TerrainGpuBakeRefusal, ARecipeRoadReportsItsSplineShape)
{
    const ECS::EntityHandle road = Road(0.0f);
    const TerrainHeightBakeReport& report = Bake();
    EXPECT_EQ(report.Path, TerrainHeightBakePath::Cpu);
    EXPECT_EQ(report.ModifierRefusal, (GpuHeightBakeRefusal{Reason::SplineShape, road}));
}

// One refusal per check, each volume holding every later check armed too, so a reordered check fails.
TEST_F(TerrainGpuBakeRefusal, EachCheckReportsItsReasonInOrder)
{
    const ECS::EntityHandle global = Flatten(0.0f, TerrainModifierBlend::Average, true);
    m_World.GetComponentForWrite<Components::TerrainModifierVolume>(global)->Shape =
        Components::TerrainVolumeShape::Global;
    m_World.GetComponentForWrite<Components::TerrainModifierVolume>(global)->FalloffInward = 2.0f;
    ExpectRefusalThenRemove(global, Reason::GlobalVolume);

    const ECS::EntityHandle feathered = Flatten(0.0f, TerrainModifierBlend::Average, true);
    m_World.GetComponentForWrite<Components::TerrainModifierVolume>(feathered)->FalloffInward = 2.0f;
    ExpectRefusalThenRemove(feathered, Reason::InwardFeatherOrWeight);

    const ECS::EntityHandle weighted = Flatten(0.0f, TerrainModifierBlend::Average, true);
    m_World.GetComponentForWrite<Components::TerrainModifierVolume>(weighted)->Weight = 0.5f;
    ExpectRefusalThenRemove(weighted, Reason::InwardFeatherOrWeight);

    ExpectRefusalThenRemove(Flatten(0.0f, TerrainModifierBlend::Average, true), Reason::PooledBlend);

    const ECS::EntityHandle fromVolume = Flatten(0.0f, TerrainModifierBlend::Set, true);
    m_World.AddComponentImmediate<Components::TerrainGroundClaimEffect>(fromVolume,
                                                                       Components::TerrainGroundClaimEffect{});
    ExpectRefusalThenRemove(fromVolume, Reason::HeightFromVolume);

    const ECS::EntityHandle eroded = Circle(0.0f);
    Components::TerrainNoiseEffect noise{};
    noise.Blend = TerrainModifierBlend::Add;
    noise.ErosionStrength = 0.5f;
    m_World.AddComponentImmediate<Components::TerrainNoiseEffect>(eroded, noise);
    ExpectRefusalThenRemove(eroded, Reason::Erosion);

    const ECS::EntityHandle claimed = Flatten(0.0f, TerrainModifierBlend::Set);
    m_World.AddComponentImmediate<Components::TerrainGroundClaimEffect>(claimed, Components::TerrainGroundClaimEffect{});
    ExpectRefusalThenRemove(claimed, Reason::GroundClaim);
}

// The first modifier in priority order holds the refusal; an expressible modifier before it refuses
// nothing, and each refusing effect after it counts once.
TEST_F(TerrainGpuBakeRefusal, TheFirstRefusingModifierIsReportedAndTheOthersCounted)
{
    Flatten(0.0f, TerrainModifierBlend::Set);
    const ECS::EntityHandle pooled = Flatten(1.0f, TerrainModifierBlend::Average);
    Flatten(2.0f, TerrainModifierBlend::Average);
    Road(3.0f);
    const TerrainHeightBakeReport& report = Bake();
    EXPECT_EQ(report.Path, TerrainHeightBakePath::Cpu);
    EXPECT_EQ(report.ModifierRefusal, (GpuHeightBakeRefusal{Reason::PooledBlend, pooled}));
    EXPECT_EQ(report.OtherModifierRefusals, 2u) << "the second Average circle and the road";
}

// A new decision increases the revision once; a bake that decides the same keeps it.
TEST_F(TerrainGpuBakeRefusal, TheRevisionMovesOnlyWhenTheDecisionChanges)
{
    const ECS::EntityHandle circle = Flatten(0.0f, TerrainModifierBlend::Set);
    const uint32 first = Bake().Revision;
    m_World.GetComponentForWrite<Components::TerrainModifierVolume>(circle)->Radius = 60.0f;
    EXPECT_EQ(Bake().Revision, first) << "an edit that keeps the GPU path";
    m_World.GetComponentForWrite<Components::TerrainFlattenEffect>(circle)->Blend = TerrainModifierBlend::Average;
    const TerrainHeightBakeReport& report = Bake();
    EXPECT_EQ(report.Revision, first + 1u);
    EXPECT_EQ(report.ModifierRefusal.Reason, Reason::PooledBlend);
}

// A reason on the terrain itself is the refusal a reader acts on; the modifier refusal stays in the
// report.
TEST_F(TerrainGpuBakeRefusal, ATerrainGateIsTheFirstRefusal)
{
    const ECS::EntityHandle road = Road(0.0f);
    Tiled()->AtlasGpuBakeEligible = false;
    const TerrainHeightBakeReport& report = Bake();
    EXPECT_EQ(report.Path, TerrainHeightBakePath::Cpu);
    EXPECT_EQ(report.FirstRefusal(), (GpuHeightBakeRefusal{Reason::TextureLayout, ECS::EntityHandle{}}));
    EXPECT_EQ(report.ModifierRefusal, (GpuHeightBakeRefusal{Reason::SplineShape, road}));
}
