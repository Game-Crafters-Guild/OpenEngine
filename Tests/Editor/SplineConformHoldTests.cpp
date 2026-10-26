// The conform's targets and its hold, the same in every process. A conform ray over a mesh whose
// BVH is still being built neither waits for it nor lands on the mesh's box: it holds the rebuild
// and lands once the BVH is published, and a recipe controller builds nothing until then. A pick
// provider's mesh and a renderer with no CPU mesh are never ground. The world here has no editor
// running: the fixture is the engine and one model.

#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineExtrude.h"
#include "Engine/Rendering/RenderServices.h"
#include "Picking/EditorPickProviders.h"
#include "SplineRecipes/RecipeBuildStates.h"
#include "SplineRecipes/SplineExtrudeController.h"
#include "SplineRecipes/SplineSurfaceConform.h"
#include "SharedHeadlessDevice.h"
#include "SplineECS/SplineService.h"

#include "PendingBvhGrid.h"

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
        if (m_OwnsSplines)
            GameEngine::SplineECS::SplineService::Shutdown();
        PendingBvhGridTest::TearDown();
    }

    // An entity at (x, 0, z) with the grid's bounds and no MeshRenderer.
    GameEngine::ECS::EntityHandle CreateBareEntityAt(float x, float z)
    {
        const GameEngine::ECS::EntityHandle entity = m_World.CreateEntity();
        Components::WorldTransform transform{};
        transform.matrix[12] = x;
        transform.matrix[14] = z;
        m_World.AddComponentImmediate(entity, transform);
        Components::LocalBounds bounds{};
        bounds.Box.halfExtents = Vector3(kGridSize * 0.5f, 0.5f, kGridSize * 0.5f);
        m_World.AddComponentImmediate(entity, bounds);
        return entity;
    }

    // A straight extrude recipe across the grid, conforming to the scene (the recipe's defaults).
    GameEngine::ECS::EntityHandle CreateExtrudeAcrossTheGrid()
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
        m_World.AddComponentImmediate(entity, Components::SplineExtrude{});
        m_World.AddComponentImmediate(entity, Components::WorldTransform{});
        return entity;
    }

    bool m_OwnsSplines = false;

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

    controller.Update(m_World, renderServices, 0.0f, states);
    m_World.ProcessCommands();
    EXPECT_EQ(CountGeneratedChunks(), 0u) << "the held rebuild built pieces over the pending mesh";

    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    controller.Update(m_World, renderServices, 0.0f, states);
    m_World.ProcessCommands();
    EXPECT_GT(CountGeneratedChunks(), 0u) << "the held recipe never built once the BVH was published";

    controller.Shutdown(m_World, &renderServices);
}
