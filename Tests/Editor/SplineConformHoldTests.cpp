// The conform's targets and its hold, the same in every process. A conform ray over a mesh whose
// BVH is still being built neither waits for it nor lands on the mesh's box: it holds the rebuild
// and lands once the BVH is published, and a recipe controller builds nothing until then. A pick
// provider's mesh and a renderer with no CPU mesh are never ground. A built recipe follows a mesh
// that arrives under it. The world here has no editor running: the fixture is the engine and one model.

#include "Components/Hierarchy.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplineFence.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Spline/SplinePaving.h"
#include "Components/Spline/SplinePlacement.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Picking/EditorPickProviders.h"
#include "SplineRecipes/RecipeBuildStates.h"
#include "SplineRecipes/RecipeControllers.h"
#include "SplineRecipes/SplineExtrudeController.h"
#include "SplineRecipes/SplineFenceController.h"
#include "SplineRecipes/SplinePlacementController.h"
#include "SplineRecipes/SplineSurfaceConform.h"
#include "SharedHeadlessDevice.h"
#include "SplineECS/SplineNetwork.h"
#include "SplineECS/SplineService.h"

#include "PendingBvhGrid.h"

#include <algorithm>
#include <iterator>
#include <vector>

#include <gtest/gtest.h>

using namespace PendingBvhGrid;
using GameEngine::SplineRecipes::ConformHold;
using GameEngine::SplineRecipes::ConformMissTally;
using GameEngine::SplineRecipes::ConformRayDown;
using GameEngine::SplineRecipes::HoldForPendingBvh;

namespace
{
namespace Picking = GameEngine::Editor::Picking;

// A component type id no component uses: the provider below is this suite's alone.
constexpr GameEngine::ECS::ComponentTypeId kConformTestProviderType = 0xC0F0'4D00'0000'0001ull;

// The world the test provider enumerates in; null outside the provider test, so the registered
// provider offers nothing to any other suite.
GameEngine::ECS::World* s_ProviderWorld = nullptr;
GameEngine::ECS::EntityHandle s_ProviderEntity{};
GUID s_ProviderModel{};

void EnumerateTestProvider(GameEngine::ECS::World& world,
                           const std::function<void(GameEngine::ECS::EntityHandle,
                                                    const Picking::SyntheticMeshPick&)>& emit)
{
    if (&world != s_ProviderWorld)
        return;
    Picking::SyntheticMeshPick pick;
    pick.MeshGpuHandleId = 1u;
    pick.ModelAssetGuid = s_ProviderModel;
    emit(s_ProviderEntity, pick);
}

class SplineConformHoldTests : public PendingBvhGridTest
{
  protected:
    void SetUp() override
    {
        PendingBvhGridTest::SetUp();
        m_OwnsSplines = !GameEngine::SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            GameEngine::SplineECS::SplineService::Initialize();
    }

    void TearDown() override
    {
        s_ProviderWorld = nullptr;
        if (m_Box)
            MeshBvhCache::Instance().InvalidateAsset(m_BoxGuid);
        if (m_OwnsSplines)
            GameEngine::SplineECS::SplineService::Shutdown();
        PendingBvhGridTest::TearDown();
    }

    // An entity at (x, 0, z) with the grid's bounds and no MeshRenderer.
    GameEngine::ECS::EntityHandle CreateBareEntityAt(float x, float z)
    {
        const GameEngine::ECS::EntityHandle entity = m_World.CreateEntity();
        Components::WorldTransform transform{};
        transform.Position = GameEngine::Mathematics::WorldPosition{x, 0.0, z};
        m_World.AddComponentImmediate(entity, transform);
        Components::LocalBounds bounds{};
        bounds.Box.halfExtents = Vector3(kGridSize * 0.5f, 0.5f, kGridSize * 0.5f);
        m_World.AddComponentImmediate(entity, bounds);
        return entity;
    }

    // A straight spline across the grid at y = 0, with a recipe `recipe`.
    template <class Recipe>
    GameEngine::ECS::EntityHandle CreateRecipeAcrossTheGrid(const Recipe& recipe)
    {
        auto& splines = GameEngine::SplineECS::SplineService::Get();
        const auto handle = splines.CreateSpline(GameEngine::Spline::SplineType::Linear, false);
        auto* data = splines.GetSplineData(handle);
        data->AddPoint(Vector3(-6.0f, 0.0f, 0.5f), 1.0f);
        data->AddPoint(Vector3(6.0f, 0.0f, 0.5f), 1.0f);
        splines.RebuildCache(handle);

        const GameEngine::ECS::EntityHandle entity = m_World.CreateEntity();
        Components::SplineComponent spline{};
        spline.SplineDataIndex = handle.Index();
        spline.SplineDataGeneration = handle.Generation();
        m_World.AddComponentImmediate(entity, spline);
        m_World.AddComponentImmediate(entity, recipe);
        m_World.AddComponentImmediate(entity, Components::WorldTransform{});
        return entity;
    }

    // A straight extrude recipe across the grid, conforming to the scene (the recipe's defaults).
    GameEngine::ECS::EntityHandle CreateExtrudeAcrossTheGrid()
    {
        return CreateRecipeAcrossTheGrid(Components::SplineExtrude{});
    }

    // A unit cube model registered with the asset manager, its BVH published: the pieces' mesh
    // and the mesh that arrives under a run.
    GUID RegisterBox()
    {
        m_Box = GameEngine::MakeShared<GameEngine::ModelAsset>(m_BoxGuid, "conform_box.glb");
        m_Box->SetMeshesForTest({GameEngine::Engine::Renderer::PrimitiveGenerator::GenerateCube()});
        EngineCore::GetInstance().GetAssetManager().RegisterLoadedAsset(m_BoxGuid, m_Box);
        EXPECT_NE(MeshBvhCache::Instance().GetOrBuild(m_BoxGuid, 0, m_Box->GetMesh(0)), nullptr);
        return m_BoxGuid;
    }

    // The box stretched to 6 m across the middle of the run, its top at y = 1.5, above the grid.
    void AddSlabUnderTheRun()
    {
        const GameEngine::ECS::EntityHandle slab = m_World.CreateEntity();
        Components::MeshRenderer renderer{};
        renderer.modelAssetGuid.Set(m_BoxGuid);
        m_World.AddComponentImmediate(slab, renderer);
        Components::WorldTransform transform{};
        transform.Basis[0].x = 6.0f;
        transform.Basis[2].z = 4.0f;
        transform.Position = GameEngine::Mathematics::WorldPosition{0.0, 1.0, 0.5};
        transform.Version = 1u;
        m_World.AddComponentImmediate(slab, transform);
        Components::LocalBounds bounds{};
        bounds.Box.halfExtents = Vector3(0.5f, 0.5f, 0.5f);
        m_World.AddComponentImmediate(slab, bounds);
    }

    // The local poses (position, then basis columns) of the pieces under `parent`, sorted: they
    // change when a piece moves.
    std::vector<std::vector<double>> PiecePosesUnder(GameEngine::ECS::EntityHandle parent)
    {
        std::vector<std::vector<double>> poses;
        m_World.Query<GameEngine::ECS::Read<Components::Transform>, GameEngine::ECS::Read<Components::Parent>>().Each(
            [&](GameEngine::ECS::EntityHandle, const Components::Transform& transform, const Components::Parent& link)
            {
                if (link.Entity != parent)
                    return;
                const auto& p = transform.Position;
                const auto& b = transform.Basis;
                poses.push_back({p.X, p.Y, p.Z, b[0].x, b[0].y, b[0].z, b[1].x, b[1].y, b[1].z, b[2].x, b[2].y,
                                 b[2].z});
            });
        std::sort(poses.begin(), poses.end());
        return poses;
    }

    static constexpr GameEngine::uint64 kSettleUpdates = 10u;

    // Enough updates of a fence or a placement controller for a change to settle, a held
    // rebuild included.
    template <class Controller>
    void UpdateUntilSettled(Controller& controller, GameEngine::Engine::Renderer::RenderServices& renderServices)
    {
        constexpr float kFrameSeconds = 0.3f;
        GameEngine::SplineRecipes::RecipeBuildStates states;
        for (GameEngine::uint64 i = 0; i < kSettleUpdates; ++i)
        {
            controller.Update(m_World, renderServices, kFrameSeconds, states);
            m_World.ProcessCommands();
        }
    }

    // A linear network member of `kind` through `points` at y = 0, closed when `closed`.
    GameEngine::ECS::EntityHandle CreateMember(std::initializer_list<Vector3> points, bool closed,
                                               Components::SplineNetworkMemberKind kind)
    {
        auto& splines = GameEngine::SplineECS::SplineService::Get();
        const auto handle = splines.CreateSpline(GameEngine::Spline::SplineType::Linear, closed);
        auto* data = splines.GetSplineData(handle);
        for (const Vector3& point : points)
            data->AddPoint(point, 1.0f);
        splines.RebuildCache(handle);
        const GameEngine::ECS::EntityHandle entity = m_World.CreateEntity();
        Components::SplineComponent spline{};
        spline.SplineDataIndex = handle.Index();
        spline.SplineDataGeneration = handle.Generation();
        m_World.AddComponentImmediate(entity, spline);
        m_World.AddComponentImmediate(entity, Components::WorldTransform{});
        m_World.AddComponentImmediate(entity, Components::SplineNetworkMember{kind});
        return entity;
    }

    // The sum of the bound centers of the chunks under `parent`: it moves when they rebuild.
    Vector3 ChunkCentersUnder(GameEngine::ECS::EntityHandle parent)
    {
        Vector3 sum(0.0f, 0.0f, 0.0f);
        m_World.Query<GameEngine::ECS::Read<Components::LocalBounds>, GameEngine::ECS::Read<Components::Parent>>().Each(
            [&](GameEngine::ECS::EntityHandle, const Components::LocalBounds& bounds, const Components::Parent& link)
            {
                if (link.Entity == parent)
                    sum = sum + bounds.Box.center;
            });
        return sum;
    }

    // One frame of the network and the recipe controllers.
    void UpdateRecipes(GameEngine::SplineRecipes::RecipeControllers& controllers,
                       GameEngine::Engine::Renderer::RenderServices& renderServices)
    {
        constexpr float kFrameSeconds = 0.3f;
        GameEngine::SplineECS::SplineNetwork::Get().Update(m_World);
        controllers.Update(m_World, &renderServices, nullptr, kFrameSeconds);
        m_World.ProcessCommands();
    }

    bool m_OwnsSplines = false;
    const GUID m_BoxGuid = GUID::Generate();
    GameEngine::SharedPtr<GameEngine::ModelAsset> m_Box;

    size_t CountGeneratedChunks()
    {
        size_t count = 0;
        m_World.Query<GameEngine::ECS::Read<Components::RuntimeOnlyEntity>>().Each(
            [&count](GameEngine::ECS::EntityHandle, const Components::RuntimeOnlyEntity&) { ++count; });
        return count;
    }
};
} // namespace

TEST_F(SplineConformHoldTests, AConformOverAMeshWithAPendingBvhHoldsAndLandsWhenItIsPublished)
{
    const Vector3 sample(3.1f, 0.0f, -2.7f);
    GameEngine::SplineRecipes::ConformMissTally whilePending;
    Vector3 hit;
    EXPECT_FALSE(GameEngine::SplineRecipes::ConformRayDown(m_World, sample, {}, hit, nullptr, &whilePending))
        << "the conform waited for the BVH or landed on the mesh's box";
    EXPECT_EQ(whilePending.PendingBvh, 1u);
    EXPECT_EQ(whilePending.Misses, 0u) << "a pending mesh was reported as no ground";
    GameEngine::SplineRecipes::ConformHold hold;
    EXPECT_TRUE(HoldForPendingBvh(whilePending, 7u, hold));
    EXPECT_TRUE(hold.Held);

    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    GameEngine::SplineRecipes::ConformMissTally published;
    ASSERT_TRUE(GameEngine::SplineRecipes::ConformRayDown(m_World, sample, {}, hit, nullptr, &published));
    EXPECT_NEAR(hit.y, SurfaceHeight(sample.x, sample.z), 0.01f);
    EXPECT_EQ(published.PendingBvh, 0u);
    EXPECT_FALSE(HoldForPendingBvh(published, 7u, hold));
    EXPECT_FALSE(hold.Held);
    EXPECT_FALSE(hold.Reported) << "the next hold would not be logged";
}

TEST_F(SplineConformHoldTests, AConformOverAPickProviderEntityGivesNoHit)
{
    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    s_ProviderEntity = CreateBareEntityAt(100.0f, 0.0f);
    s_ProviderModel = m_Guid;
    s_ProviderWorld = &m_World;
    Picking::EditorPickProvider provider;
    provider.Enumerate = &EnumerateTestProvider;
    Picking::EditorPickProviderRegistry::Get().Register(kConformTestProviderType, std::move(provider));

    Vector3 hit;
    EXPECT_FALSE(GameEngine::SplineRecipes::ConformRayDown(m_World, Vector3(101.0f, 0.0f, 1.0f), {}, hit))
        << "the conform landed on an editor pick provider's mesh, which a Player has no provider for";
}

TEST_F(SplineConformHoldTests, AConformOverARendererWithNoCpuMeshGivesNoHit)
{
    const GameEngine::ECS::EntityHandle runtime = CreateBareEntityAt(-100.0f, 0.0f);
    Components::MeshRenderer renderer{};
    renderer.modelAssetGuid.Set(GUID::Generate());
    m_World.AddComponentImmediate(runtime, renderer);

    Vector3 hit;
    EXPECT_FALSE(GameEngine::SplineRecipes::ConformRayDown(m_World, Vector3(-99.0f, 0.0f, 1.0f), {}, hit))
        << "the conform landed on the top of a renderer's box";
}

// The extrude controller over a mesh whose BVH is pending: the first update holds and builds no
// piece, and the first update after the BVH is published builds the pieces, with no edit between.
TEST_F(SplineConformHoldTests, AnExtrudeHeldByAPendingBvhBuildsOnTheUpdateAfterItIsPublished)
{
    // The pieces register their mesh and material with a device's render services.
    GameEngine::Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device available";
    GameEngine::Engine::Renderer::RenderServices renderServices;
    ASSERT_TRUE(renderServices.Initialize(device));
    CreateExtrudeAcrossTheGrid();
    GameEngine::SplineRecipes::SplineExtrudeController controller;
    GameEngine::SplineRecipes::RecipeBuildStates states;
    const GameEngine::SplineRecipes::PavingRoadTrims trims;

    controller.Update(m_World, renderServices, 0.0f, states, trims, {});
    m_World.ProcessCommands();
    EXPECT_EQ(CountGeneratedChunks(), 0u) << "the held rebuild built pieces over the pending mesh";

    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    controller.Update(m_World, renderServices, 0.0f, states, trims, {});
    m_World.ProcessCommands();
    EXPECT_GT(CountGeneratedChunks(), 0u) << "the held recipe never built once the BVH was published";

    controller.Shutdown(m_World, &renderServices);
}

// A road edited while the paved region it enters waits for a pending BVH keeps its strip until the
// region builds: a strip cut at the old arc lengths never shows against the old paving.
TEST_F(SplineConformHoldTests, ARoadEditedWhileItsPavedRegionIsHeldKeepsItsStripUntilTheRegionBuilds)
{
    GameEngine::Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device available";
    GameEngine::Engine::Renderer::RenderServices renderServices;
    ASSERT_TRUE(renderServices.Initialize(device));
    const bool ownsNetwork = !GameEngine::SplineECS::SplineNetwork::IsInitialized();
    if (ownsNetwork)
        GameEngine::SplineECS::SplineNetwork::Initialize();
    GameEngine::SplineRecipes::RecipeControllers controllers;

    // The square paves over the terrain-only ground (none here) first: the grid's BVH stays unasked.
    const GameEngine::ECS::EntityHandle square =
        CreateMember({Vector3(-5.0f, 0.0f, -5.0f), Vector3(5.0f, 0.0f, -5.0f), Vector3(5.0f, 0.0f, 5.0f),
                      Vector3(-5.0f, 0.0f, 5.0f)},
                     true, Components::SplineNetworkMemberKind::Area);
    m_World.AddComponentImmediate(square, Components::SplinePaving{});
    const GameEngine::ECS::EntityHandle road = CreateMember({Vector3(-9.0f, 0.0f, 0.0f), Vector3(9.0f, 0.0f, 0.0f)},
                                                            false, Components::SplineNetworkMemberKind::Road);
    Components::SplineExtrude strip{};
    strip.ConformMode = Components::SplinePlacementConform::None;
    m_World.AddComponentImmediate(road, strip);
    for (int i = 0; i < 8; ++i)
        UpdateRecipes(controllers, renderServices);
    ASSERT_EQ(controllers.BuildStates().StateOf(m_World, square).State, GameEngine::SplineRecipes::BuildState::Built);

    // One edit to each: the square now reads the scene (the grid, its BVH pending), the road starts 3 m
    // further out, so every arc length on it moves.
    m_World.GetComponentForWrite<Components::SplinePaving>(square)->ConformTarget =
        Components::SplineConformTarget::Scene;
    const auto* roadSpline = m_World.GetComponent<Components::SplineComponent>(road);
    auto& splines = GameEngine::SplineECS::SplineService::Get();
    const GameEngine::SplineECS::SplineHandle handle(roadSpline->SplineDataIndex, roadSpline->SplineDataGeneration);
    splines.GetSplineData(handle)->SetPointPosition(0, Vector3(-12.0f, 0.0f, 0.0f));
    splines.RebuildCache(handle);

    bool roadHeld = false;
    bool lidMoved = false;
    for (int i = 0; i < 8 && !lidMoved; ++i)
    {
        const Vector3 lidBefore = ChunkCentersUnder(square);
        const Vector3 roadBefore = ChunkCentersUnder(road);
        UpdateRecipes(controllers, renderServices);
        lidMoved = (ChunkCentersUnder(square) - lidBefore).LengthSquared() > 0.0f;
        if (!lidMoved)
        {
            EXPECT_EQ((ChunkCentersUnder(road) - roadBefore).LengthSquared(), 0.0f)
                << "the strip rebuilt on the old trims on update " << i;
        }
        roadHeld |= controllers.BuildStates().ReasonOf(m_World, road) ==
                    GameEngine::SplineRecipes::kHeldForPickingDataReason;
        if (i == 2)
        {
            ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
        }
    }
    EXPECT_TRUE(roadHeld) << "the region never held, so the test saw no pending BVH";
    EXPECT_TRUE(lidMoved) << "the region never built once the BVH was published";

    controllers.ReleaseSceneScoped(m_World, &renderServices);
    if (ownsNetwork)
        GameEngine::SplineECS::SplineNetwork::Shutdown();
}

// A fence conforms to the meshes under it. A mesh that arrives under a built fence moves its
// pieces onto the mesh, as a first build over the mesh places them.
TEST_F(SplineConformHoldTests, AFenceRidesAMeshThatArrivesUnderItAfterItsFirstBuild)
{
    // The pieces register their mesh and material with a device's render services.
    GameEngine::Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device available";
    GameEngine::Engine::Renderer::RenderServices renderServices;
    ASSERT_TRUE(renderServices.Initialize(device));
    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    Components::SplineFence recipe{};
    recipe.Parts[0] = {Components::ModelRef(RegisterBox()), Components::SplineFencePartKind::Span};
    const GameEngine::ECS::EntityHandle fence = CreateRecipeAcrossTheGrid(recipe);

    GameEngine::SplineRecipes::SplineFenceController controller;
    UpdateUntilSettled(controller, renderServices);
    const std::vector<std::vector<double>> beforeTheMesh = PiecePosesUnder(fence);
    ASSERT_FALSE(beforeTheMesh.empty()) << "the fence built no pieces";

    AddSlabUnderTheRun();
    UpdateUntilSettled(controller, renderServices);
    const std::vector<std::vector<double>> afterTheMesh = PiecePosesUnder(fence);
    // A settled fence requests the scene TLAS once per update and casts no conform ray.
    const GameEngine::uint64 requestsBefore = GameEngine::Scene::GetSceneTlas(m_World).GetRequestCountForTesting();
    UpdateUntilSettled(controller, renderServices);
    EXPECT_EQ(GameEngine::Scene::GetSceneTlas(m_World).GetRequestCountForTesting() - requestsBefore, kSettleUpdates);
    controller.Shutdown(m_World, &renderServices);
    m_World.ProcessCommands();

    GameEngine::SplineRecipes::SplineFenceController firstBuild;
    UpdateUntilSettled(firstBuild, renderServices);
    const std::vector<std::vector<double>> builtOverTheMesh = PiecePosesUnder(fence);
    EXPECT_NE(builtOverTheMesh, beforeTheMesh) << "positive control: the fence does not reach the mesh";
    EXPECT_EQ(afterTheMesh, builtOverTheMesh) << "the fence kept the ground it measured before the mesh arrived";
    firstBuild.Shutdown(m_World, &renderServices);
}

// Tiles placed along a spline conform to the meshes under them. A mesh that arrives under a
// built row moves its tiles onto the mesh, as a first build over the mesh places them.
TEST_F(SplineConformHoldTests, APlacementRidesAMeshThatArrivesUnderItAfterItsFirstBuild)
{
    GameEngine::Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device available";
    GameEngine::Engine::Renderer::RenderServices renderServices;
    ASSERT_TRUE(renderServices.Initialize(device));
    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    Components::SplinePlacement recipe{};
    recipe.StraightPool[0] = Components::ModelRef(RegisterBox());
    const GameEngine::ECS::EntityHandle row = CreateRecipeAcrossTheGrid(recipe);

    GameEngine::SplineRecipes::SplinePlacementController controller;
    UpdateUntilSettled(controller, renderServices);
    const std::vector<std::vector<double>> beforeTheMesh = PiecePosesUnder(row);
    ASSERT_FALSE(beforeTheMesh.empty()) << "the placement built no tiles";

    AddSlabUnderTheRun();
    UpdateUntilSettled(controller, renderServices);
    const std::vector<std::vector<double>> afterTheMesh = PiecePosesUnder(row);
    // A settled row requests the scene TLAS once per update and casts no conform ray.
    const GameEngine::uint64 requestsBefore = GameEngine::Scene::GetSceneTlas(m_World).GetRequestCountForTesting();
    UpdateUntilSettled(controller, renderServices);
    EXPECT_EQ(GameEngine::Scene::GetSceneTlas(m_World).GetRequestCountForTesting() - requestsBefore, kSettleUpdates);
    controller.Shutdown(m_World);
    m_World.ProcessCommands();

    GameEngine::SplineRecipes::SplinePlacementController firstBuild;
    UpdateUntilSettled(firstBuild, renderServices);
    const std::vector<std::vector<double>> builtOverTheMesh = PiecePosesUnder(row);
    EXPECT_NE(builtOverTheMesh, beforeTheMesh) << "positive control: the tiles do not reach the mesh";
    EXPECT_EQ(afterTheMesh, builtOverTheMesh) << "the tiles kept the ground they measured before the mesh arrived";
    firstBuild.Shutdown(m_World);
}

// One update of the extrude recipes requests the scene TLAS once, however many of them conform
// to the scene.
TEST_F(SplineConformHoldTests, AnExtrudeUpdateRequestsTheSceneTlasOnceForAllItsRecipes)
{
    constexpr int kUpdates = 10;
    constexpr int kSteadyUpdates = 4;
    constexpr float kFrameSeconds = 0.3f;
    GameEngine::Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device available";
    GameEngine::Engine::Renderer::RenderServices renderServices;
    ASSERT_TRUE(renderServices.Initialize(device));
    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    CreateExtrudeAcrossTheGrid();
    CreateExtrudeAcrossTheGrid();
    GameEngine::SplineRecipes::SplineExtrudeController controller;
    GameEngine::SplineRecipes::RecipeBuildStates states;
    const GameEngine::SplineRecipes::PavingRoadTrims trims;
    for (int i = 0; i < kUpdates; ++i)
    {
        controller.Update(m_World, renderServices, kFrameSeconds, states, trims, {});
        m_World.ProcessCommands();
    }
    ASSERT_GT(CountGeneratedChunks(), 0u) << "the recipes never built";

    const GameEngine::Scene::SceneTlas& tlas = GameEngine::Scene::GetSceneTlas(m_World);
    const GameEngine::uint64 requestsBefore = tlas.GetRequestCountForTesting();
    for (int i = 0; i < kSteadyUpdates; ++i)
    {
        controller.Update(m_World, renderServices, kFrameSeconds, states, trims, {});
        m_World.ProcessCommands();
    }
    EXPECT_EQ(tlas.GetRequestCountForTesting() - requestsBefore, static_cast<GameEngine::uint64>(kSteadyUpdates));

    controller.Shutdown(m_World, &renderServices);
}
