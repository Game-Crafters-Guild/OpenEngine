// The terrain in both scene raycast entry points. Scene View selection casts through
// RaycastSceneAll (it needs the whole front-to-back stack for click-through), and a single hit
// through RaycastScene. On both, IncludeTerrain decides whether the terrain is tested, the two
// agree about what the ray hit, and a mesh flush with the terrain answers before it.

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "PagedTiledTerrain.h"
#include "Scene/SceneRaycast.h"
#include "Scene/SceneTlas.h"
#include "Scene/TerrainPicking.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainService.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using GameEngine::EngineCore;
using GameEngine::ApplicationConfig;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Ray3D;
using GameEngine::Mathematics::Vector3;
using GameEngine::TerrainECS::TerrainService;
using GameEngine::TerrainECS::Test::CreatePagedTiledTerrain;
using GameEngine::TerrainECS::Test::PagedTiledTerrain;
using GameEngine::TerrainECS::Test::StreamPagesUntilTheCameraPageLands;
using GameEngine::Scene::PickableKind;
using GameEngine::Scene::PickHit;
using GameEngine::Scene::PickOptions;
using GameEngine::Scene::RaycastScene;
using GameEngine::Scene::RaycastSceneAll;

namespace Components = GameEngine::Components;

namespace
{
constexpr float kTerrainSize = 64.0f;
constexpr float kHeightScale = 16.0f;

// A world holding one planar terrain centred at the origin, flat at y = 0 (CreateTerrain
// zero-fills the heightfield). The engine is initialized because the picking entry points
// reach AssetManager to attach their BVH-invalidation hook.
class SceneRaycastTerrainInclusionTests : public ::testing::Test
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

    // A built-in cube at `center`, scaled per axis by `scale`.
    GameEngine::ECS::EntityHandle CreateCube(const Vector3& center, const Vector3& scale)
    {
        const GameEngine::ECS::EntityHandle cube = m_World.CreateEntity();
        Components::MeshRenderer renderer{};
        renderer.modelAssetGuid.Set(GameEngine::Engine::Renderer::PrimitiveGenerator::CubeGuid());
        m_World.AddComponentImmediate(cube, renderer);
        Components::WorldTransform transform{};
        transform.matrix[0] = scale.x;
        transform.matrix[5] = scale.y;
        transform.matrix[10] = scale.z;
        transform.matrix[12] = center.x;
        transform.matrix[13] = center.y;
        transform.matrix[14] = center.z;
        m_World.AddComponentImmediate(cube, transform);
        Components::LocalBounds bounds{};
        bounds.Box.halfExtents = Vector3(0.5f, 0.5f, 0.5f);
        m_World.AddComponentImmediate(cube, bounds);
        return cube;
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

// The fixture's terrain is hit through RaycastScene, the control for the RaycastSceneAll tests.
TEST_F(SceneRaycastTerrainInclusionTests, RaycastSceneHitsTheTerrain)
{
    const auto result = RaycastScene(DownAtCentre(), m_World, PickOptions{});
    ASSERT_TRUE(result.Hit);
    EXPECT_EQ(result.Best.Kind, PickableKind::Terrain);
    EXPECT_EQ(result.Best.Entity.id, m_Terrain.id);
    EXPECT_NEAR(result.Best.WorldPosition.y, 0.0f, 0.1f);
}

// The selection path's entry point hits the terrain.
TEST_F(SceneRaycastTerrainInclusionTests, RaycastSceneAllHitsTheTerrain)
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
TEST_F(SceneRaycastTerrainInclusionTests, RaycastSceneAllHonoursIncludeTerrainFalse)
{
    PickOptions options;
    options.IncludeTerrain = false;

    std::vector<PickHit> hits;
    RaycastSceneAll(DownAtCentre(), m_World, hits, options);
    EXPECT_TRUE(hits.empty());
}

// A terrain-only query is a legitimate query: turning the mesh categories off must not
// short-circuit the cast before terrain is considered.
TEST_F(SceneRaycastTerrainInclusionTests, RaycastSceneAllServesATerrainOnlyQuery)
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
TEST_F(SceneRaycastTerrainInclusionTests, BothEntryPointsAgreeOnTheNearestHit)
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
TEST_F(SceneRaycastTerrainInclusionTests, RaycastSceneAllMissesOutsideTheFootprint)
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
TEST_F(SceneRaycastTerrainInclusionTests, RaycastSceneMissesATiledTerrainWhoseHeightsHaveNoSourceYet)
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
TEST_F(SceneRaycastTerrainInclusionTests, RaycastSceneHitsAPagedTiledTerrainWhereItsTileIsNotResident)
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
TEST_F(SceneRaycastTerrainInclusionTests, RaycastSceneHitsAPagedTiledTerrainAtItsFinestHeightOutsideTheCpuWindow)
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

// A cube whose top lies flush with the terrain at y = 0: both entry points answer with the cube,
// RaycastSceneAll with the terrain right behind it.
TEST_F(SceneRaycastTerrainInclusionTests, AMeshFlushWithTheTerrainAnswersBeforeItInBothEntryPoints)
{
    const GameEngine::ECS::EntityHandle cube = CreateCube(Vector3(0.0f, -1.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f));

    const auto best = RaycastScene(DownAtCentre(), m_World, PickOptions{});
    ASSERT_TRUE(best.Hit);
    EXPECT_EQ(best.Best.Kind, PickableKind::Primitive);
    EXPECT_EQ(best.Best.Entity, cube);

    std::vector<PickHit> hits;
    RaycastSceneAll(DownAtCentre(), m_World, hits, PickOptions{});
    ASSERT_GE(hits.size(), 2u);
    EXPECT_EQ(hits[0].Kind, PickableKind::Primitive);
    EXPECT_EQ(hits[0].Entity, cube);
    EXPECT_EQ(hits[1].Kind, PickableKind::Terrain);
}

// The normal of a face under non-uniform scale stays perpendicular to the face. The cube is turned
// 45 degrees about +y and then stretched four times along world x, which shears it into a rhombus
// in the xz plane: the stretched image of a face normal is not perpendicular to the face.
TEST_F(SceneRaycastTerrainInclusionTests, AFaceNormalUnderNonUniformScaleIsPerpendicularToTheFace)
{
    // World = S * R, column-major: the columns are the world images of the local axes.
    const float c = std::sqrt(0.5f);
    const Vector3 axisX(4.0f * c, 0.0f, -c);
    const Vector3 axisY(0.0f, 1.0f, 0.0f);
    const Vector3 axisZ(4.0f * c, 0.0f, c);
    const GameEngine::ECS::EntityHandle cube = m_World.CreateEntity();
    Components::MeshRenderer renderer{};
    renderer.modelAssetGuid.Set(GameEngine::Engine::Renderer::PrimitiveGenerator::CubeGuid());
    m_World.AddComponentImmediate(cube, renderer);
    Components::WorldTransform transform{};
    transform.matrix[0] = axisX.x;
    transform.matrix[2] = axisX.z;
    transform.matrix[5] = axisY.y;
    transform.matrix[8] = axisZ.x;
    transform.matrix[10] = axisZ.z;
    transform.matrix[13] = 10.0f; // above the terrain
    m_World.AddComponentImmediate(cube, transform);
    Components::LocalBounds bounds{};
    bounds.Box.halfExtents = Vector3(0.5f, 0.5f, 0.5f);
    m_World.AddComponentImmediate(cube, bounds);

    // Along -x at z = 0.2: onto the face of local +z, which the local x and y axes span.
    PickOptions options;
    options.IncludeTerrain = false;
    Ray3D ray;
    ray.origin = Vector3(20.0f, 10.0f, 0.2f);
    ray.direction = Vector3(-1.0f, 0.0f, 0.0f);
    const auto result = RaycastScene(ray, m_World, options);
    ASSERT_TRUE(result.Hit);
    ASSERT_EQ(result.Best.Entity, cube);

    const Vector3 n = result.Best.WorldNormal;
    EXPECT_NEAR(n.Length(), 1.0f, 1e-4f);
    EXPECT_NEAR(Vector3::Dot(n, axisX * (1.0f / axisX.Length())), 0.0f, 1e-4f)
        << "the normal (" << n.x << ", " << n.y << ", " << n.z << ") leans along the face";
    EXPECT_NEAR(Vector3::Dot(n, axisY), 0.0f, 1e-4f);
    EXPECT_GT(n.z, 0.0f) << "the normal points into the cube";
}
