#pragma once

// The test world the ground authority suites share: a flat baked terrain, the
// terrain entity over it and a straight graded run, with the terrain and spline
// services scoped to one test. Included by SplineRecipesTests and by the
// editor's undo cases, which drive the same controller through the journal.

#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "SplineECS/SplineService.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

#include <vector>

namespace GameEngine::SplineGroundAuthorityTestWorld
{

using TerrainECS::TerrainHandle;
using TerrainECS::TerrainModifierSystem;
using TerrainECS::TerrainService;

constexpr uint32 kHeightmapDim = 65;
constexpr float32 kWorldSize = 128.0f;
constexpr float32 kHeightScale = 100.0f;
constexpr float32 kSampleSpacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1);
constexpr float32 kRunHalfWidth = 4.0f;
constexpr float32 kRunGrade = 12.0f;

struct ScopedTerrainService
{
    ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
    }
    ~ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }
};

struct ScopedSplineService
{
    ScopedSplineService()
    {
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
        SplineECS::SplineService::Initialize();
    }
    ~ScopedSplineService()
    {
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
    }
};

inline Components::WorldTransform IdentityAt(float32 x, float32 y, float32 z)
{
    Components::WorldTransform xf{};
    xf.matrix[0] = xf.matrix[5] = xf.matrix[10] = xf.matrix[15] = 1.0f;
    xf.matrix[12] = x; xf.matrix[13] = y; xf.matrix[14] = z;
    return xf;
}

inline TerrainHandle CreateBakedTerrain(TerrainService& svc)
{
    Terrain::TerrainConfig config{};
    config.HeightmapWidth = kHeightmapDim;
    config.HeightmapHeight = kHeightmapDim;
    config.WorldSizeX = kWorldSize;
    config.WorldSizeZ = kWorldSize;
    config.HeightScale = kHeightScale;
    config.LODLevels = 4;
    const TerrainHandle handle = svc.CreateTerrain(config);
    auto* data = svc.GetTerrainData(handle);
    // FLAT base, so any difference between the two arms below is the graded run
    // and nothing else. Procedural noise would still compare equal, but a
    // failure would be far harder to read.
    for (uint32 z = 0; z < kHeightmapDim; ++z)
        for (uint32 x = 0; x < kHeightmapDim; ++x)
            data->Heightfield.SetSample(x, z, 0.25f);
    data->MarkFullDirty();
    svc.RebuildQuadtree(handle);
    data->ResetSplatmapAndCommitRange();
    return handle;
}

inline void CreateTerrainEntity(ECS::World& world, TerrainHandle handle)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = kWorldSize;
    terrain.SizeZ = kWorldSize;
    terrain.HeightScale = kHeightScale;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    terrain.BaseSource = Components::TerrainBaseSource::Flat;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
}

// A straight run along X at constant height, carrying whatever recipe the caller
// hands it. The spline's width channel is the run's half-width, which is also
// what the volume's station polyline reads — there is no second width.
inline ECS::EntityHandle CreateRun(ECS::World& world, SplineECS::SplineService& svc, float32 z)
{
    const SplineECS::SplineHandle handle =
        svc.CreateSpline(Spline::SplineType::CatmullRom, false);
    auto* data = svc.GetSplineData(handle);
    for (const float32 x : {-40.0f, 0.0f, 40.0f})
        data->AddPoint({x, kRunGrade, z}, kRunHalfWidth);
    svc.RebuildCache(handle);

    auto e = world.CreateEntity();
    Components::SplineComponent comp{};
    comp.SplineDataIndex = handle.Index();
    comp.SplineDataGeneration = handle.Generation();
    world.AddComponentImmediate<Components::SplineComponent>(e, comp);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
    return e;
}

inline std::vector<float32> BakeWorld(ECS::World& world, TerrainService& svc, TerrainHandle handle)
{
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    auto* data = svc.GetTerrainData(handle);
    const float32* baked = data->Heightfield.GetRawSamples();
    std::vector<float32> out(baked, baked + data->Heightfield.GetSampleCount());
    return out;
}

} // namespace GameEngine::SplineGroundAuthorityTestWorld
