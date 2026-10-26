// The editor's pick over a mesh whose BVH is pending. A Scene View drop reports a point or a mesh
// that is only a pending stand-in, so the drop can refuse it; a mark-up region's knot lands on the
// ground beneath; a mesh the Scene View eye hides draws in no Scene View pass, so no pick hits it;
// a pick provider's mesh, which has no MeshRenderer, is hit like a renderer's.

#include "Components/SceneViewHidden.h"
#include "Markups/MarkupTool.h"
#include "Picking/EditorPickProviders.h"
#include "Picking/ScenePick.h"
#include "SceneView/SceneViewDropPlacement.h"

#include "PendingBvhGrid.h"

#include <optional>
#include <vector>

#include <gtest/gtest.h>

using namespace PendingBvhGrid;
using GameEngine::Editor::Picking::PickScene;
using GameEngine::Editor::Picking::PickSceneAll;

namespace
{
namespace Picking = GameEngine::Editor::Picking;

// A component type id no component uses: the provider below is this suite's alone.
constexpr GameEngine::ECS::ComponentTypeId kScenePickTestProviderType = 0xC0F0'4D00'0000'0002ull;

// The world the test provider enumerates in; null outside the provider test, so the registered
// provider offers nothing to any other test.
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

class ScenePickPendingBvhTests : public PendingBvhGridTest
{
  protected:
    void TearDown() override
    {
        s_ProviderWorld = nullptr;
        PendingBvhGridTest::TearDown();
    }
};
} // namespace

TEST_F(ScenePickPendingBvhTests, ADropPointOnAPendingMeshIsMarkedSoPlacementRefusesIt)
{
    const Ray3D ray = DownAt(3.1f, -2.7f);
    const auto whilePending = GameEngine::Editor::ResolveSceneViewDropPoint(ray, m_World);
    EXPECT_TRUE(whilePending.OnPendingMesh) << "a model would be placed on the bounds";
    EXPECT_NEAR(whilePending.Position.y, 0.5f, 1e-4f);

    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    const auto published = GameEngine::Editor::ResolveSceneViewDropPoint(ray, m_World);
    EXPECT_FALSE(published.OnPendingMesh);
    EXPECT_NEAR(published.Position.y, SurfaceHeight(3.1f, -2.7f), 0.01f);
}

// A region's knot is ground, never refused for a mesh whose picking is still being prepared: the
// Mark-up tool's Region click over a road's fresh mesh lands on the ground beneath (here no
// terrain, so the ground plane), where a drop would refuse it.
TEST_F(ScenePickPendingBvhTests, ARegionClickOverAPendingMeshLandsOnTheGround)
{
    const Ray3D ray = DownAt(3.1f, -2.7f);
    ASSERT_TRUE(GameEngine::Editor::ResolveSceneViewDropPoint(ray, m_World).OnPendingMesh);
    const std::optional<Vector3> ground = GameEngine::Editor::ResolveMarkupRegionGround(ray, m_World);
    ASSERT_TRUE(ground.has_value()) << "the click was dropped";
    EXPECT_NEAR(ground->x, 3.1f, 1e-4f);
    EXPECT_NEAR(ground->y, 0.0f, 1e-4f);
    EXPECT_NEAR(ground->z, -2.7f, 1e-4f);
}

// Texture and material drops both assign to this query's mesh.
TEST_F(ScenePickPendingBvhTests, ADropMeshStillPendingIsMarkedSoAssignmentRefusesIt)
{
    const Ray3D ray = DownAt(-4.3f, 6.2f);
    const auto whilePending = GameEngine::Editor::PickSceneViewDropMesh(ray, m_World);
    EXPECT_EQ(whilePending.Entity, m_Entity);
    EXPECT_TRUE(whilePending.Pending) << "a texture or material would be assigned through the bounds";

    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    const auto published = GameEngine::Editor::PickSceneViewDropMesh(ray, m_World);
    EXPECT_EQ(published.Entity, m_Entity);
    EXPECT_FALSE(published.Pending);
}

TEST_F(ScenePickPendingBvhTests, AMeshTheSceneViewEyeHidesIsNotHit)
{
    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    ASSERT_TRUE(PickScene(DownAt(1.0f, 1.0f), m_World, PickOptions{}).Hit) << "the control pick";
    m_World.AddComponentImmediate<Components::SceneViewHidden>(m_Entity, {});
    EXPECT_FALSE(PickScene(DownAt(1.0f, 1.0f), m_World, PickOptions{}).Hit);
    EXPECT_FALSE(PickScene(DownAt(1.0f, 1.0f), m_World, RestrictedToGrid()).Hit);
    std::vector<GameEngine::Scene::PickHit> hits;
    PickSceneAll(DownAt(1.0f, 1.0f), m_World, hits, PickOptions{});
    EXPECT_TRUE(hits.empty());
}

TEST_F(ScenePickPendingBvhTests, APickProviderMeshIsHitByTheEditorPick)
{
    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    const GameEngine::ECS::EntityHandle bare = m_World.CreateEntity();
    Components::WorldTransform transform{};
    transform.matrix[12] = 100.0f;
    m_World.AddComponentImmediate(bare, transform);
    Components::LocalBounds bounds{};
    bounds.Box.halfExtents = Vector3(kGridSize * 0.5f, 0.5f, kGridSize * 0.5f);
    m_World.AddComponentImmediate(bare, bounds);
    s_ProviderEntity = bare;
    s_ProviderModel = m_Guid;
    s_ProviderWorld = &m_World;
    Picking::EditorPickProvider provider;
    provider.Enumerate = &EnumerateTestProvider;
    Picking::EditorPickProviderRegistry::Get().Register(kScenePickTestProviderType, std::move(provider));

    const auto pick = PickScene(DownAt(101.0f, 1.0f), m_World, PickOptions{});
    ASSERT_TRUE(pick.Hit) << "the editor pick did not test the provider's mesh";
    EXPECT_EQ(pick.Best.Entity, bare);
    EXPECT_EQ(pick.Best.Kind, GameEngine::Scene::PickableKind::Mesh);
    std::vector<GameEngine::Scene::PickHit> hits;
    PickSceneAll(DownAt(101.0f, 1.0f), m_World, hits, PickOptions{});
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].Entity, bare);
}
