#pragma once

// A tiled noise terrain whose height pages stream as a test requests them: the generated provider
// over its tile lattice, the state of a tiled terrain the extraction pages. Shared by the picking
// tests (ScenePickTerrainInclusionTests), the navigation bake tests
// (NavigationSurfacePagedTerrainTests) and the ocean depth bake tests (OceanDepthCacheBakerTests).

#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainService.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

namespace GameEngine::TerrainECS::Test
{

// The terrain's tiled data, its component (not yet on an entity) and the request that pages it.
struct PagedTiledTerrain
{
    TiledTerrainHandle Tiled;
    Components::Terrain Component;
    PagedTerrainRequest Request;
};

// A worldSizeX x worldSizeZ meter terrain at 1 m, on tiles of the shorter side, its pages from the
// tile noise fill (level 0 equals what its tiles hold), heights of +/- 2 x heightScale at most.
inline PagedTiledTerrain CreatePagedTiledTerrain(TerrainService& service, float32 worldSizeX, float32 worldSizeZ,
                                                 float32 heightScale)
{
    PagedTiledTerrain paged;
    TiledTerrainConfig config{};
    config.WorldSizeX = worldSizeX;
    config.WorldSizeZ = worldSizeZ;
    config.HeightScale = heightScale;
    config.SamplesPerMeter = 1.0f;
    paged.Tiled = service.CreateTiledTerrain(config);
    TiledTerrainData* tiled = service.GetTiledTerrainData(paged.Tiled);

    Terrain::TerrainConfig renderConfig{};
    renderConfig.HeightmapWidth = 3;
    renderConfig.HeightmapHeight = 3;
    renderConfig.WorldSizeX = config.WorldSizeX;
    renderConfig.WorldSizeZ = config.WorldSizeZ;
    const TerrainHandle render = service.CreateTerrain(renderConfig);
    tiled->GlobalGpuHandleIndex = render.Index;
    tiled->GlobalGpuHandleGeneration = render.Generation;

    PageStreaming::GeneratedHeightLattice lattice;
    lattice.Source = PageStreaming::GeneratedHeightSource::Noise;
    lattice.TerrainOriginX = tiled->WorldOriginX;
    lattice.TerrainOriginZ = tiled->WorldOriginZ;
    lattice.TileWorldSize = tiled->Config.TileWorldSize;
    lattice.TileSamples = tiled->Config.TileConfig.HeightmapWidth;
    lattice.TilesX = tiled->Config.TilesPerAxisX;
    lattice.TilesZ = tiled->Config.TilesPerAxisZ;
    lattice.Noise = {kTileNoiseFrequency, kTileNoiseAmplitude, kTileNoiseOctaves, kTileNoiseSeed};
    PagedTerrainRequest& request = paged.Request;
    request.Terrain = render.Index;
    request.TerrainGeneration = render.Generation;
    request.SourceIdentity = 1;
    request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
    request.Shape.OriginX = tiled->WorldOriginX;
    request.Shape.OriginZ = tiled->WorldOriginZ;
    request.Shape.Level0TexelX = 1.0f;
    request.Shape.Level0TexelZ = 1.0f;
    request.Shape.SamplesX = static_cast<uint32>(std::lround(worldSizeX)) + 1u;
    request.Shape.SamplesZ = static_cast<uint32>(std::lround(worldSizeZ)) + 1u;
    request.Shape.MinHeight = -2.0f * heightScale;
    request.Shape.MaxHeight = 2.0f * heightScale;
    request.OriginY = 0.0f;
    request.HeightScale = heightScale;

    Components::Terrain& terrain = paged.Component;
    terrain.SizeX = config.WorldSizeX;
    terrain.SizeZ = config.WorldSizeZ;
    terrain.HeightScale = heightScale;
    terrain.Domain = Components::TerrainDomain::Planar;
    terrain.TiledTerrainHandle = paged.Tiled.Index;
    terrain.TiledTerrainGeneration = paged.Tiled.Generation;
    return paged;
}

// Frames of the terrain's height pages with one camera at `camera`, until the level-0 page under the
// camera is on the CPU (it may land on a job a few frames later). False when it never does.
inline bool StreamPagesUntilTheCameraPageLands(TerrainService& service, const PagedTiledTerrain& paged,
                                               const Mathematics::Vector3& camera)
{
    constexpr uint64 kFramesToSettle = 2000;
    TerrainHeightPages& pages = service.GetHeightPages();
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    for (uint64 frame = 2; frame < kFramesToSettle; ++frame)
    {
        pages.Request(paged.Request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        SurfaceHeightSample sample;
        if (service.SurfaceQuery(paged.Component, Components::WorldTransform{})
                .SampleHeight(camera, SurfaceHeightPolicy::RequireFinest, sample))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace GameEngine::TerrainECS::Test
