// What the recipe controllers keep for each recipe: the build state, the scene's settled summary and
// the bounds of a fence's parts.

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplineFence.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Scripting/ScriptsConfig.h"
#include "SharedHeadlessDevice.h"
#include "SplineECS/SplineService.h"
#include "SplineRecipes/RecipeBuildStates.h"
#include "SplineRecipes/FenceRecipeLayout.h"
#include "SplineRecipes/RecipeControllers.h"
#include "SplineRecipes/SplineRecipesScriptApi.h"
#include "SplineRecipes/SplineSurfaceConform.h"
#include "TerrainECS/TerrainService.h"

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <thread>

namespace GameEngine::SplineRecipes
{
namespace
{

using Mathematics::Vector3;

constexpr float32 kFrameSeconds = 0.3f;

class SplineRecipeBuildStateTests : public ::testing::Test
{
protected:
    // The builds load their materials through the engine's asset manager.
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (engine.IsInitialized())
            return;
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAsyncHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scriptsConfig);
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        ASSERT_TRUE(engine.Initialize(config));
    }

    void SetUp() override
    {
        Rendering::IDevice* device = SharedHeadlessDevice();
        if (!device)
            GTEST_SKIP() << "no Vulkan device available";
        ASSERT_TRUE(m_RenderServices.Initialize(device));
        m_OwnsSplines = !SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            SplineECS::SplineService::Initialize();
        m_OwnsTerrain = !TerrainECS::TerrainService::IsInitialized();
        if (m_OwnsTerrain)
            TerrainECS::TerrainService::Initialize();
    }

    void TearDown() override
    {
        m_Controllers.ReleaseSceneScoped(m_World, &m_RenderServices);
        if (m_OwnsTerrain)
            TerrainECS::TerrainService::Shutdown();
        if (m_OwnsSplines)
            SplineECS::SplineService::Shutdown();
    }

    // A straight spline entity along `pointCount` points 12 m apart, with no recipe yet.
    ECS::EntityHandle CreateSplineEntity(uint32 pointCount)
    {
        auto& splines = SplineECS::SplineService::Get();
        const auto handle = splines.CreateSpline(Spline::SplineType::Linear, false);
        auto* data = splines.GetSplineData(handle);
        for (uint32 i = 0; i < pointCount; ++i)
            data->AddPoint(Vector3(-6.0f + 12.0f * static_cast<float32>(i), 0.0f, 0.5f), 1.0f);
        splines.RebuildCache(handle);

        const ECS::EntityHandle entity = m_World.CreateEntity();
        Components::SplineComponent spline{};
        spline.SplineDataIndex = handle.Index();
        spline.SplineDataGeneration = handle.Generation();
        m_World.AddComponentImmediate(entity, spline);
        m_World.AddComponentImmediate(entity, Components::WorldTransform{});
        return entity;
    }

    // A straight extrude recipe with the recipe's defaults, along `pointCount` points 12 m apart.
    ECS::EntityHandle CreateExtrude(uint32 pointCount)
    {
        const ECS::EntityHandle entity = CreateSplineEntity(pointCount);
        m_World.AddComponentImmediate(entity, Components::SplineExtrude{});
        return entity;
    }

    // A fence of one Span part with `mesh`, on two points, that does not conform: it needs no ground.
    ECS::EntityHandle CreateFence(const GUID& mesh)
    {
        const ECS::EntityHandle entity = CreateSplineEntity(2);
        Components::SplineFence fence{};
        fence.Parts[0].Mesh.Set(mesh);
        fence.ConformMode = Components::SplinePlacementConform::None;
        m_World.AddComponentImmediate(entity, fence);
        return entity;
    }

    // A resident model of one cube mesh with `halfExtent`, under a new GUID. The fixture's teardown
    // leaves it registered: each test makes its own GUIDs.
    static GUID RegisterCubeModel(float32 halfExtent)
    {
        const GUID guid = GUID::Generate();
        auto model = std::make_shared<ModelAsset>(guid, "cube.fbx");
        Vector<Mesh> meshes;
        meshes.push_back(Engine::Renderer::PrimitiveGenerator::GenerateCube(halfExtent));
        model->SetMeshesForTest(std::move(meshes));
        EngineCore::GetInstance().GetAssetManager().RegisterLoadedAsset(guid, model);
        return guid;
    }

    // Runs the controllers until `recipe` is Built, at most `maxUpdates` times.
    void UpdateUntilBuilt(ECS::EntityHandle recipe, uint32 maxUpdates)
    {
        for (uint32 i = 0; i < maxUpdates && StateOf(recipe) != BuildState::Built; ++i)
            Update();
    }

    void Update()
    {
        m_Controllers.Update(m_World, &m_RenderServices, nullptr, kFrameSeconds);
        m_World.ProcessCommands();
    }

    // A terrain whose data is not loaded, centered at `center` with the default size.
    ECS::EntityHandle CreateUnloadedTerrain(const Vector3& center)
    {
        const ECS::EntityHandle entity = m_World.CreateEntity();
        m_World.AddComponentImmediate(entity, Components::Terrain{});
        Components::WorldTransform transform{};
        transform.matrix[12] = center.x;
        transform.matrix[13] = center.y;
        transform.matrix[14] = center.z;
        m_World.AddComponentImmediate(entity, transform);
        return entity;
    }

    // A mesh entity at `position` whose model is not loaded.
    ECS::EntityHandle CreateUnloadedModel(const Vector3& position, const GUID& model)
    {
        const ECS::EntityHandle entity = m_World.CreateEntity();
        Components::MeshRenderer renderer{};
        renderer.modelAssetGuid.Set(model);
        m_World.AddComponentImmediate(entity, renderer);
        Components::WorldTransform transform{};
        transform.matrix[12] = position.x;
        transform.matrix[13] = position.y;
        transform.matrix[14] = position.z;
        m_World.AddComponentImmediate(entity, transform);
        return entity;
    }

    uint32 ChunkCount()
    {
        uint32 count = 0;
        m_World.Query<ECS::Read<Components::RuntimeOnlyEntity>>().Each(
            [&](ECS::EntityHandle, const Components::RuntimeOnlyEntity&) { ++count; });
        return count;
    }

    float32 ChunkTop(ECS::EntityHandle chunk)
    {
        const Mathematics::BoundingBox& box = m_World.GetComponent<Components::LocalBounds>(chunk)->Box;
        return box.center.y + box.halfExtents.y;
    }

    BuildState StateOf(ECS::EntityHandle recipe)
    {
        return m_Controllers.BuildStates().StateOf(m_World, recipe).State;
    }

    ECS::World m_World;
    Engine::Renderer::RenderServices m_RenderServices;
    RecipeControllers m_Controllers;
    bool m_OwnsSplines = false;
    bool m_OwnsTerrain = false;
};

TEST_F(SplineRecipeBuildStateTests, ARecipeIsPendingUntilItsFirstBuildAndBuiltAfter)
{
    const ECS::EntityHandle road = CreateExtrude(2);
    EXPECT_EQ(StateOf(road), BuildState::Pending);
    EXPECT_FALSE(m_Controllers.BuildStates().IsSceneSettled(m_World))
        << "a recipe that no update has seen settled the scene";
    EXPECT_FALSE(m_Controllers.BuildStates().IsSceneSettledAtLastUpdate(m_World));

    Update();

    EXPECT_EQ(StateOf(road), BuildState::Built);
    EXPECT_TRUE(m_Controllers.BuildStates().IsSceneSettled(m_World));
    EXPECT_TRUE(m_Controllers.BuildStates().IsSceneSettledAtLastUpdate(m_World));
}

// A process with no render device builds no recipe: each recipe fails at once with the reason,
// so the scene settles on the first update.
TEST_F(SplineRecipeBuildStateTests, WithNoRenderDeviceEveryRecipeFailsAndTheSceneSettlesAtOnce)
{
    const ECS::EntityHandle road = CreateExtrude(2);
    const ECS::EntityHandle fence = m_World.CreateEntity();
    m_World.AddComponentImmediate(fence, Components::SplineComponent{});
    m_World.AddComponentImmediate(fence, Components::SplineFence{});
    m_World.AddComponentImmediate(fence, Components::WorldTransform{});

    m_Controllers.Update(m_World, nullptr, nullptr, kFrameSeconds);
    m_World.ProcessCommands();

    const RecipeBuildStates& states = m_Controllers.BuildStates();
    EXPECT_EQ(StateOf(road), BuildState::Failed);
    EXPECT_EQ(StateOf(fence), BuildState::Failed);
    EXPECT_EQ(states.ReasonOf(m_World, road), kNoRenderDeviceReason);
    EXPECT_EQ(states.Failures(m_World).size(), 2u);
    EXPECT_TRUE(states.IsSceneSettled(m_World)) << "a run with no render device kept the scene unsettled";
    EXPECT_TRUE(states.IsSceneSettledAtLastUpdate(m_World));
    EXPECT_EQ(ChunkCount(), 0u);
}

// The script API answers on the main thread only: a call from another thread is refused and
// returns the default, not the state.
TEST_F(SplineRecipeBuildStateTests, AQueryFromAnotherThreadIsRefused)
{
    const ECS::EntityHandle prop = m_World.CreateEntity();
    m_World.AddComponentImmediate(prop, Components::WorldTransform{});
    ASSERT_TRUE(SplineRecipes::IsSceneSettled(m_World)) << "a world with no recipe is not settled on the main thread";
    ASSERT_EQ(SplineRecipes::StateOf(m_World, prop).State, BuildState::Built);

    bool settled = true;
    BuildState state = BuildState::Built;
    std::thread caller([&] {
        settled = SplineRecipes::IsSceneSettled(m_World);
        state = SplineRecipes::StateOf(m_World, prop).State;
    });
    caller.join();

    EXPECT_FALSE(settled) << "IsSceneSettled answered off the main thread";
    EXPECT_EQ(state, BuildState::Pending) << "StateOf answered off the main thread";
}

TEST_F(SplineRecipeBuildStateTests, ARecipeOverATerrainThatIsNotLoadedIsHeldAndBuildsWhenItLands)
{
    const ECS::EntityHandle terrainEntity = CreateUnloadedTerrain(Vector3(0.0f, 0.0f, 0.0f));
    CreateUnloadedTerrain(Vector3(5000.0f, 0.0f, 5000.0f));
    const ECS::EntityHandle road = CreateExtrude(2);

    Update();
    EXPECT_EQ(StateOf(road), BuildState::Held);
    EXPECT_EQ(m_Controllers.BuildStates().ReasonOf(m_World, road), kHeldForTerrainReason);
    EXPECT_FALSE(m_Controllers.BuildStates().IsSceneSettled(m_World));

    Terrain::TerrainConfig config{};
    config.HeightmapWidth = config.HeightmapHeight = 17;
    config.WorldSizeX = config.WorldSizeZ = 32.0f;
    config.LODLevels = 2;
    const auto handle = TerrainECS::TerrainService::Get().CreateTerrain(config);
    auto* terrain = m_World.GetComponentForWrite<Components::Terrain>(terrainEntity);
    ASSERT_NE(terrain, nullptr);
    terrain->TerrainDataHandle = handle.Index;
    terrain->TerrainDataGeneration = handle.Generation;

    for (int frame = 0; frame < 4 && StateOf(road) != BuildState::Built; ++frame)
        Update();
    EXPECT_EQ(StateOf(road), BuildState::Built)
        << "the held recipe never built once the terrain under it landed, with a far terrain still not loaded";
    EXPECT_TRUE(m_Controllers.BuildStates().ReasonOf(m_World, road).empty());
    EXPECT_TRUE(m_Controllers.BuildStates().IsSceneSettled(m_World));
}

TEST_F(SplineRecipeBuildStateTests, AFarTerrainThatIsNotLoadedDoesNotHoldTheRecipe)
{
    CreateUnloadedTerrain(Vector3(5000.0f, 0.0f, 5000.0f));
    const ECS::EntityHandle road = CreateExtrude(2);

    Update();

    EXPECT_EQ(StateOf(road), BuildState::Built) << "a terrain far from the run held its first build";
    EXPECT_TRUE(m_Controllers.BuildStates().IsSceneSettled(m_World));
}

TEST_F(SplineRecipeBuildStateTests, ARecipeOnASplineOfOnePointFailsWithTheReason)
{
    const ECS::EntityHandle road = CreateExtrude(1);

    Update();

    EXPECT_EQ(StateOf(road), BuildState::Failed);
    EXPECT_EQ(m_Controllers.BuildStates().ReasonOf(m_World, road), kSplineTooShortReason);
    const auto failures = m_Controllers.BuildStates().Failures(m_World);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_EQ(failures[0].Recipe.id, road.id);
    EXPECT_TRUE(m_Controllers.BuildStates().IsSceneSettled(m_World)) << "a Failed recipe kept the scene unsettled";
}

TEST_F(SplineRecipeBuildStateTests, ARecipeThatMakesNoChunkIsBuiltWithTheReason)
{
    const ECS::EntityHandle river = CreateExtrude(2);
    m_World.GetComponentForWrite<Components::SplineExtrude>(river)->WidthMode =
        Components::SplineExtrudeWidthMode::FitToBanks;

    Update();
    EXPECT_EQ(StateOf(river), BuildState::Built);
    EXPECT_EQ(m_Controllers.BuildStates().ReasonOf(m_World, river), kNoTerrainToFillReason);

    Update();
    EXPECT_EQ(m_Controllers.BuildStates().ReasonOf(m_World, river), kNoTerrainToFillReason)
        << "the reason went away on an update that did not build again";
    EXPECT_TRUE(m_Controllers.BuildStates().IsSceneSettled(m_World));
}

// A 120 m road makes 3 chunks; a model 44 m before its start is within the margin of chunk 0 only.
constexpr uint32 kLongRoadPoints = 11;
const Vector3 kModelNearFirstChunk(-50.0f, 0.0f, 0.5f);

TEST_F(SplineRecipeBuildStateTests, AChunkNearAModelThatIsNotLoadedWaitsAndTheOthersBuild)
{
    const ECS::EntityHandle model = CreateUnloadedModel(kModelNearFirstChunk, GUID::Generate());
    const ECS::EntityHandle road = CreateExtrude(kLongRoadPoints);

    Update();
    EXPECT_EQ(StateOf(road), BuildState::Held);
    EXPECT_EQ(m_Controllers.BuildStates().ReasonOf(m_World, road), "1 model near it is still loading. It builds when the model loads.");
    EXPECT_EQ(ChunkCount(), 2u) << "the chunks far from the model did not build, or the near one did";
    EXPECT_FALSE(m_Controllers.BuildStates().IsSceneSettled(m_World));

    m_World.GetComponentForWrite<Components::MeshRenderer>(model)->modelAssetGuid.Set(
        Engine::Renderer::PrimitiveGenerator::CubeGuid());
    Update();

    EXPECT_EQ(StateOf(road), BuildState::Built) << "the waiting chunk did not build once its model was resident";
    EXPECT_EQ(ChunkCount(), 3u);
    EXPECT_TRUE(m_Controllers.BuildStates().IsSceneSettled(m_World));
}

TEST_F(SplineRecipeBuildStateTests, AModelWhoseLoadFailedEndsTheWaitWithTheReason)
{
    const GUID missing = GUID::Generate();
    CreateUnloadedModel(kModelNearFirstChunk, missing);
    const ECS::EntityHandle road = CreateExtrude(kLongRoadPoints);
    Update();
    ASSERT_EQ(StateOf(road), BuildState::Held);

    EngineCore::GetInstance().GetAssetManager().SuppressLoadRetries(missing, AssetMetadata{}, "test failure");
    Update();

    EXPECT_EQ(StateOf(road), BuildState::Failed);
    EXPECT_NE(m_Controllers.BuildStates().ReasonOf(m_World, road).find(missing.ToString()), std::string_view::npos)
        << "the reason does not name the model";
    EXPECT_EQ(ChunkCount(), 3u) << "the chunk did not build without the model";
    EngineCore::GetInstance().GetAssetManager().ClearLoadSuppressed(missing);
}

TEST_F(SplineRecipeBuildStateTests, AWaitPastTheBudgetBuildsWithoutTheModelAndFails)
{
    const GUID slow = GUID::Generate();
    CreateUnloadedModel(kModelNearFirstChunk, slow);
    const ECS::EntityHandle road = CreateExtrude(kLongRoadPoints);
    Update();
    ASSERT_EQ(StateOf(road), BuildState::Held);

    // A frame of a third of the budget: the wait ends on the third frame.
    const float32 longFrame = kConformHoldBudgetSeconds / 3.0f;
    for (int frame = 0; frame < 2; ++frame)
    {
        m_Controllers.Update(m_World, &m_RenderServices, nullptr, longFrame);
        m_World.ProcessCommands();
        ASSERT_EQ(StateOf(road), BuildState::Held) << "the wait ended before the budget, frame " << frame;
    }
    m_Controllers.Update(m_World, &m_RenderServices, nullptr, longFrame);
    m_World.ProcessCommands();

    EXPECT_EQ(StateOf(road), BuildState::Failed);
    EXPECT_NE(m_Controllers.BuildStates().ReasonOf(m_World, road).find(slow.ToString()), std::string_view::npos);
    EXPECT_EQ(ChunkCount(), 3u);
    EXPECT_TRUE(m_Controllers.BuildStates().IsSceneSettled(m_World));
}

// The part bounds follow a part's mesh: the rebuild after a mesh change lists the new mesh's box
// and does not list the old one.
TEST_F(SplineRecipeBuildStateTests, AFencesPartBoundsFollowAPartsMeshChange)
{
    constexpr uint32 kMaxUpdates = 8;
    constexpr float32 kSmallHalfExtent = 0.5f;
    constexpr float32 kLargeHalfExtent = 1.25f;
    const GUID small = RegisterCubeModel(kSmallHalfExtent);
    const GUID large = RegisterCubeModel(kLargeHalfExtent);
    const ECS::EntityHandle fence = CreateFence(small);
    EXPECT_TRUE(m_Controllers.FencePartBounds(fence).empty()) << "a fence that has not built lists no bounds";

    UpdateUntilBuilt(fence, kMaxUpdates);
    ASSERT_EQ(StateOf(fence), BuildState::Built);
    std::span<const FenceMeshBounds> bounds = m_Controllers.FencePartBounds(fence);
    ASSERT_EQ(bounds.size(), 1u);
    EXPECT_EQ(bounds[0].Mesh.ToGuid(), small);
    EXPECT_NEAR(bounds[0].Bounds.HalfExtents.y, kSmallHalfExtent, 1e-4f);

    m_World.GetComponentForWrite<Components::SplineFence>(fence)->Parts[0].Mesh.Set(large);
    Update();
    UpdateUntilBuilt(fence, kMaxUpdates);
    ASSERT_EQ(StateOf(fence), BuildState::Built);
    bounds = m_Controllers.FencePartBounds(fence);
    ASSERT_EQ(bounds.size(), 1u) << "the old mesh is still listed after the part changed its mesh";
    EXPECT_EQ(bounds[0].Mesh.ToGuid(), large);
    EXPECT_NEAR(bounds[0].Bounds.HalfExtents.y, kLargeHalfExtent, 1e-4f);
}

// The spline's points move without a version bump, so only a rebuild can move the chunk.
TEST(SplineConformHoldBudget, IsTwiceTheSlowestMeasuredColdSceneLoad)
{
    // The slowest of three cold-cache Release Player loads of a scene with 20,804 meshes and a
    // Scene-conform road: seconds from the start to the startup scene's ready line.
    constexpr float32 kSlowestMeasuredLoadSeconds = 32.42f;
    EXPECT_GE(kConformHoldBudgetSeconds, 2.0f * kSlowestMeasuredLoadSeconds);
}

TEST_F(SplineRecipeBuildStateTests, AMaterialSwapRebindsTheChunkAndRunsNoRebuild)
{
    const ECS::EntityHandle road = CreateExtrude(2);
    m_World.GetComponentForWrite<Components::SplineExtrude>(road)->ConformMode =
        Components::SplinePlacementConform::None;
    Update();
    ECS::EntityHandle chunk{};
    m_World.Query<ECS::Read<Components::RuntimeOnlyEntity>>().Each(
        [&](ECS::EntityHandle e, const Components::RuntimeOnlyEntity&) { chunk = e; });
    ASSERT_TRUE(m_World.IsValid(chunk));
    const float32 builtTop = ChunkTop(chunk);

    const auto& spline = *m_World.GetComponent<Components::SplineComponent>(road);
    auto* data = SplineECS::SplineService::Get().GetSplineData(
        SplineECS::SplineHandle(spline.SplineDataIndex, spline.SplineDataGeneration));
    for (auto& point : data->Points)
        point.Position.y += 10.0f;

    const GUID material = GUID::Generate();
    m_RenderServices.RegisterAndPrewarmMaterial(material, MaterialDocument::CreateDefaultPBR("Swap"));
    auto* recipe = m_World.GetComponentForWrite<Components::SplineExtrude>(road);
    recipe->Material.Set(material);
    recipe->CastShadows = false;
    for (int frame = 0; frame < 3; ++frame)
        Update();

    const auto* renderer = m_World.GetComponent<Components::MeshRenderer>(chunk);
    ASSERT_NE(renderer, nullptr) << "the swap replaced the chunk entity";
    EXPECT_EQ(renderer->materialAssetGuid.ToGuid(), material);
    EXPECT_EQ(renderer->shadowCasting, Components::ShadowCasting::Off);
    EXPECT_FALSE(m_World.GetComponent<Components::LocalBounds>(chunk)->CastShadows);
    EXPECT_FLOAT_EQ(ChunkTop(chunk), builtTop)
        << "the material swap rebuilt the geometry";

    m_World.GetComponentForWrite<Components::SplineExtrude>(road)->VerticalOffset = 1.0f;
    for (int frame = 0; frame < 3; ++frame)
        Update();
    EXPECT_GT(ChunkTop(chunk), builtTop + 10.0f)
        << "a geometry edit did not rebuild, so the check above proves nothing";
    EXPECT_EQ(m_World.GetComponent<Components::MeshRenderer>(chunk)->materialAssetGuid.ToGuid(), material);
}

// The documented reading of the query in a process whose recipe system never ran
// (docs/scripting/spline-recipes.mdx): a recipe is Pending, so only a world with no
// recipe is settled, and an entity with no recipe is Built.
TEST(SplineRecipesQuery, WithNoRecipeSystemOnlyAWorldWithNoRecipeIsSettled)
{
    ECS::World world;
    const ECS::EntityHandle prop = world.CreateEntity();
    world.AddComponentImmediate(prop, Components::WorldTransform{});
    EXPECT_TRUE(IsSceneSettled(world));
    EXPECT_EQ(StateOf(world, prop).State, BuildState::Built);

    const ECS::EntityHandle road = world.CreateEntity();
    world.AddComponentImmediate(road, Components::SplineComponent{});
    world.AddComponentImmediate(road, Components::SplineExtrude{});
    world.AddComponentImmediate(road, Components::WorldTransform{});
    EXPECT_EQ(StateOf(world, road).State, BuildState::Pending);
    EXPECT_FALSE(IsSceneSettled(world));
    EXPECT_TRUE(ReasonOf(world, road).empty());
    EXPECT_EQ(Failures(world, {}), 0);
}

} // namespace
} // namespace GameEngine::SplineRecipes
