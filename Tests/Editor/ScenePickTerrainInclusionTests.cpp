// Scene-view selection casts through RaycastSceneAll (it needs the whole front-to-back stack
// for click-through), while only RaycastScene carried a terrain branch. Terrain was therefore
// absent from every selection and hover pick regardless of PickOptions::IncludeTerrain, so
// clicking terrain in the scene view selected nothing. These lock both entry points on the
// same rule: IncludeTerrain decides, and the two agree about what the ray hit.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "Picking/MeshPickingService.h"
#include "Picking/TerrainPicking.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scene/SceneTlas.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainService.h"
#include "PagedTiledTerrain.h"

using GameEngine::EngineCore;
using GameEngine::ApplicationConfig;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Ray3D;
using GameEngine::Mathematics::Vector3;
using GameEngine::TerrainECS::TerrainService;
using GameEngine::TerrainECS::Test::CreatePagedTiledTerrain;
using GameEngine::TerrainECS::Test::PagedTiledTerrain;
using GameEngine::TerrainECS::Test::StreamPagesUntilTheCameraPageLands;
using GameEngine::Editor::Picking::PickableKind;
using GameEngine::Editor::Picking::PickHit;
using GameEngine::Editor::Picking::PickOptions;
using GameEngine::Editor::Picking::RaycastScene;
using GameEngine::Editor::Picking::RaycastSceneAll;

namespace Components = GameEngine::Components;

namespace
{
constexpr float kTerrainSize = 64.0f;
constexpr float kHeightScale = 16.0f;

// A world holding one planar terrain centred at the origin, flat at y = 0 (CreateTerrain
// zero-fills the heightfield). The engine is initialized because the picking entry points
// reach AssetManager to attach their BVH-invalidation hook.
class ScenePickTerrainInclusionTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void SetUp() override
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();

        const auto config = GameEngine::Terrain::TerrainConfig::FromSamplesPerMeter(
            kTerrainSize, kTerrainSize, kHeightScale, /*samplesPerMeter*/ 1.0f);
        const GameEngine::TerrainECS::TerrainHandle handle =
            TerrainService::Get().CreateTerrain(config);

        Components::Terrain terrain{};
        terrain.SizeX = kTerrainSize;
        terrain.SizeZ = kTerrainSize;
        terrain.HeightScale = kHeightScale;
        terrain.Domain = Components::TerrainDomain::Planar;
        terrain.TerrainDataHandle = handle.Index;
        terrain.TerrainDataGeneration = handle.Generation;

        m_Terrain = m_World.CreateEntity();
        m_World.AddComponentImmediate(m_Terrain, terrain);
        m_World.AddComponentImmediate(m_Terrain, Components::WorldTransform{});
    }

    void TearDown() override
    {
        GameEngine::Scene::ReleaseSceneTlas(m_World);
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }

    static Ray3D DownAtCentre()
    {
        Ray3D ray;
        ray.origin = Vector3(0.0f, 100.0f, 0.0f);
        ray.direction = Vector3(0.0f, -1.0f, 0.0f);
        return ray;
    }

    World m_World;
    GameEngine::ECS::EntityHandle m_Terrain{};
};
} // namespace

// Harness sanity twin: the fixture's terrain IS hittable through the entry point that always
// had a terrain branch. Without this, the RaycastSceneAll expectation below could fail (or
// pass) for reasons that have nothing to do with the missing branch.
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneHitsTheTerrain)
{
    const auto result = RaycastScene(DownAtCentre(), m_World, PickOptions{});
    ASSERT_TRUE(result.Hit);
    EXPECT_EQ(result.Best.Kind, PickableKind::Terrain);
    EXPECT_EQ(result.Best.Entity.id, m_Terrain.id);
    EXPECT_NEAR(result.Best.WorldPosition.y, 0.0f, 0.1f);
}

// The defect: the selection path's entry point returned no hits at all over a terrain.
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneAllHitsTheTerrain)
{
    std::vector<PickHit> hits;
    RaycastSceneAll(DownAtCentre(), m_World, hits, PickOptions{});

    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].Kind, PickableKind::Terrain);
    EXPECT_EQ(hits[0].Entity.id, m_Terrain.id);
    EXPECT_NEAR(hits[0].WorldPosition.y, 0.0f, 0.1f);
    EXPECT_NEAR(hits[0].Distance, 100.0f, 0.2f);
}

// IncludeTerrain is what decides, so a caller that wants meshes only still gets meshes only.
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneAllHonoursIncludeTerrainFalse)
{
    PickOptions options;
    options.IncludeTerrain = false;

    std::vector<PickHit> hits;
    RaycastSceneAll(DownAtCentre(), m_World, hits, options);
    EXPECT_TRUE(hits.empty());
}

// A terrain-only query is a legitimate query: turning the mesh categories off must not
// short-circuit the cast before terrain is considered.
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneAllServesATerrainOnlyQuery)
{
    PickOptions options;
    options.IncludeMeshes = false;
    options.IncludePrimitives = false;

    std::vector<PickHit> hits;
    RaycastSceneAll(DownAtCentre(), m_World, hits, options);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].Kind, PickableKind::Terrain);
}

// The two entry points answer the same question the same way — the nearest hit of the full
// stack is the single best hit.
TEST_F(ScenePickTerrainInclusionTests, BothEntryPointsAgreeOnTheNearestHit)
{
    const auto best = RaycastScene(DownAtCentre(), m_World, PickOptions{});
    std::vector<PickHit> hits;
    RaycastSceneAll(DownAtCentre(), m_World, hits, PickOptions{});

    ASSERT_TRUE(best.Hit);
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits.front().Entity.id, best.Best.Entity.id);
    EXPECT_EQ(hits.front().Kind, best.Best.Kind);
    EXPECT_NEAR(hits.front().Distance, best.Best.Distance, 1e-3f);
}

// A ray that leaves the footprint reports nothing, so the terrain branch is not a blanket
// "always one hit".
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneAllMissesOutsideTheFootprint)
{
    Ray3D ray;
    ray.origin = Vector3(kTerrainSize * 4.0f, 100.0f, 0.0f);
    ray.direction = Vector3(0.0f, -1.0f, 0.0f);

    std::vector<PickHit> hits;
    RaycastSceneAll(ray, m_World, hits, PickOptions{});
    EXPECT_TRUE(hits.empty());
}

// A tiled terrain whose heights have no page source yet (a height store still cooking) is not
// picked: it answers unsampled, never with the height of a tile that happens to be resident.
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneMissesATiledTerrainWhoseHeightsHaveNoSourceYet)
{
    GameEngine::TerrainECS::TiledTerrainConfig config{};
    config.WorldSizeX = 2048.0f;
    config.WorldSizeZ = 1024.0f;
    config.HeightScale = kHeightScale;
    config.SamplesPerMeter = 1.0f;
    const auto handle = TerrainService::Get().CreateTiledTerrain(config);
    const auto* tiled = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    ASSERT_EQ(tiled->Config.TilesPerAxisX, 2u);
    auto* tile = TerrainService::Get().LoadTile(handle, {0, 0});
    ASSERT_NE(tile, nullptr);
    tile->LodState = GameEngine::TerrainECS::TileLodState::Full;
    // The tile's cached range, as the streaming integration sets it with the heights.
    tile->Heightfield.GetMinMax(0, 0, static_cast<GameEngine::int32>(tile->Heightfield.GetWidth()),
                                static_cast<GameEngine::int32>(tile->Heightfield.GetHeight()), tile->CachedMinH,
                                tile->CachedMaxH);

    // The tiled terrain covers [0, 2048] x [0, 1024]: its corner is the tiles' world origin.
    Components::Terrain terrain{};
    terrain.SizeX = config.WorldSizeX;
    terrain.SizeZ = config.WorldSizeZ;
    terrain.HeightScale = kHeightScale;
    terrain.Domain = Components::TerrainDomain::Planar;
    terrain.TiledTerrainHandle = handle.Index;
    terrain.TiledTerrainGeneration = handle.Generation;
    Components::WorldTransform transform{};
    transform.matrix[12] = config.WorldSizeX * 0.5f;
    transform.matrix[14] = config.WorldSizeZ * 0.5f;
    const auto entity = m_World.CreateEntity();
    m_World.AddComponentImmediate(entity, terrain);
    m_World.AddComponentImmediate(entity, transform);

    constexpr float kProbeX = 700.0f;
    constexpr float kProbeZ = 300.0f;
    ASSERT_TRUE(TerrainService::Get().SurfaceQuery(terrain, transform).ContainsXZ(kProbeX, kProbeZ))
        << "the probe is on the footprint, over the resident tile";

    Ray3D ray;
    ray.origin = Vector3(kProbeX, 1000.0f, kProbeZ);
    ray.direction = Vector3(0.0f, -1.0f, 0.0f);
    const auto result = RaycastScene(ray, m_World, PickOptions{});
    EXPECT_FALSE(result.Hit) << "the pick read the resident tile of a terrain with no page source";
}

namespace
{

// The terrain's transform: centered on its footprint.
Components::WorldTransform TerrainTransform(const Components::Terrain& terrain)
{
    Components::WorldTransform transform{};
    transform.matrix[12] = terrain.SizeX * 0.5f;
    transform.matrix[14] = terrain.SizeZ * 0.5f;
    return transform;
}

// The terrain's entity, centered on its footprint.
GameEngine::ECS::EntityHandle AddTerrainEntity(World& world, const Components::Terrain& terrain)
{
    const auto entity = world.CreateEntity();
    world.AddComponentImmediate(entity, terrain);
    world.AddComponentImmediate(entity, TerrainTransform(terrain));
    return entity;
}

// The height tile `tileX` of `paged` holds at world (x, z): the tile is loaded at full detail.
float SampleLoadedTile(TerrainService& service, const PagedTiledTerrain& paged, GameEngine::int32 tileX, float x, float z)
{
    using namespace GameEngine::TerrainECS;
    TerrainTileData* tile = service.LoadTile(paged.Tiled, {tileX, 0});
    EXPECT_NE(tile, nullptr);
    tile->LodState = TileLodState::Full;
    const TiledTerrainData& tiled = *service.GetTiledTerrainData(paged.Tiled);
    const float tileSize = tiled.Config.TileWorldSize;
    const float tileOriginX = tiled.WorldOriginX + static_cast<float>(tileX) * tileSize;
    return tile->Heightfield.SampleBilinear((x - tileOriginX) / tileSize, (z - tiled.WorldOriginZ) / tileSize) *
           kHeightScale;
}

Ray3D DownAt(float x, float z)
{
    Ray3D ray;
    ray.origin = Vector3(x, 1000.0f, z);
    ray.direction = Vector3(0.0f, -1.0f, 0.0f);
    return ray;
}

} // namespace

// A paged tiled terrain is picked on its CPU height pages, also where its tile is not resident:
// the pick lands on the height the tile would hold (the generated pages' level 0 is the tile fill).
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneHitsAPagedTiledTerrainWhereItsTileIsNotResident)
{
    using namespace GameEngine::TerrainECS;
    TerrainService& service = TerrainService::Get();
    const PagedTiledTerrain paged = CreatePagedTiledTerrain(service, 2048.0f, 1024.0f, kHeightScale);
    constexpr float kProbeX = 1500.0f;
    constexpr float kProbeZ = 300.0f;
    const float expected = SampleLoadedTile(service, paged, 1, kProbeX, kProbeZ);
    service.UnloadTile(paged.Tiled, {1, 0});
    ASSERT_TRUE(StreamPagesUntilTheCameraPageLands(service, paged, Vector3(kProbeX, 50.0f, kProbeZ)));

    const auto entity = AddTerrainEntity(m_World, paged.Component);
    const auto result = RaycastScene(DownAt(kProbeX, kProbeZ), m_World, PickOptions{});
    ASSERT_TRUE(result.Hit) << "the pick misses a paged terrain whose tile is not resident";
    EXPECT_EQ(result.Best.Entity.id, entity.id);
    EXPECT_NEAR(result.Best.WorldPosition.y, expected, 0.1f);
}

// Past the CPU window, a paged terrain is picked at its finest height, not on the coarser pinned
// page: a hit on a coarse page fetches the finest page under it and marches again. 400 m from the
// camera the pinned level is off by more than the tile's own height error.
TEST_F(ScenePickTerrainInclusionTests, RaycastSceneHitsAPagedTiledTerrainAtItsFinestHeightOutsideTheCpuWindow)
{
    using namespace GameEngine::TerrainECS;
    TerrainService& service = TerrainService::Get();
    PagedTiledTerrain paged = CreatePagedTiledTerrain(service, 2048.0f, 1024.0f, kHeightScale);
    paged.Request.GpuPages = false; // only the CPU windows want level 0: the GPU fetches none there
    // Between the level-0 samples: on a coarse lattice point a coarser level holds the same sample.
    constexpr float kProbeX = 1501.5f;
    constexpr float kProbeZ = 301.5f;
    const float expected = SampleLoadedTile(service, paged, 1, kProbeX, kProbeZ);
    const Vector3 camera(kProbeX - 400.0f, 50.0f, kProbeZ);
    ASSERT_TRUE(StreamPagesUntilTheCameraPageLands(service, paged, camera));
    SurfaceHeightSample coarse;
    ASSERT_TRUE(service.SurfaceQuery(paged.Component, TerrainTransform(paged.Component))
                    .SampleHeight(Vector3(kProbeX, 0.0f, kProbeZ), SurfaceHeightPolicy::AcceptCoarse, coarse));
    ASSERT_EQ(coarse.Detail, SurfaceHeightDetail::Coarse) << "the probe is outside every CPU window";
    ASSERT_GT(std::abs(coarse.Height - expected), 0.1f) << "the coarse page must miss by more than the tolerance";

    const auto entity = AddTerrainEntity(m_World, paged.Component);
    const auto result = RaycastScene(DownAt(kProbeX, kProbeZ), m_World, PickOptions{});
    ASSERT_TRUE(result.Hit);
    EXPECT_EQ(result.Best.Entity.id, entity.id);
    EXPECT_NEAR(result.Best.WorldPosition.y, expected, 0.1f) << "the coarse page answered: " << coarse.Height;
    SurfaceHeightSample finest;
    EXPECT_TRUE(service.SurfaceQuery(paged.Component, TerrainTransform(paged.Component))
                    .SampleHeight(Vector3(kProbeX, 0.0f, kProbeZ), SurfaceHeightPolicy::RequireFinest, finest))
        << "the pick fetched the finest page under its hit";
}
