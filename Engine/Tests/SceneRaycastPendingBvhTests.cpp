// A ray never builds a large mesh's BVH on the calling thread. The first ray starts a job and
// answers with no hit and the result marked pending, or with the mesh's bounds, marked pending,
// when the bounds fallback is on; once the job publishes, rays hit triangles, and the published
// BVH is the one a synchronous build makes. An invalidated build never publishes. A shadows-only
// mesh draws in no color pass, so no ray hits it.

#include "JobSystem/WorkStealingThreadPool.h"

#include "PendingBvhGrid.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace PendingBvhGrid;
using GameEngine::Scene::PickableKind;
using GameEngine::Scene::RaycastScene;
using GameEngine::Scene::RaycastSceneAll;

namespace
{
class SceneRaycastPendingBvhTests : public PendingBvhGridTest
{
};
} // namespace

TEST_F(SceneRaycastPendingBvhTests, TheFirstLookupIsPendingAtOnceAndPublishesTheSynchronousBvh)
{
    const auto buildStart = std::chrono::steady_clock::now();
    const MeshPicking::MeshBvh reference = MeshPicking::MeshBvh::Build(ViewOf(Grid()));
    const double buildMs = MillisecondsSince(buildStart);

    const auto lookupStart = std::chrono::steady_clock::now();
    const auto first = MeshBvhCache::Instance().TryGet(m_Guid, 0, Grid());
    const double lookupMs = MillisecondsSince(lookupStart);
    EXPECT_TRUE(first.Pending) << "the lookup built the BVH on the calling thread";
    EXPECT_EQ(first.Bvh, nullptr);
    EXPECT_LT(lookupMs, buildMs / 4.0) << "lookup " << lookupMs << " ms against a " << buildMs << " ms build";

    const auto published = MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid());
    ASSERT_NE(published, nullptr);
    std::vector<uint8_t> publishedBytes;
    std::vector<uint8_t> referenceBytes;
    published->Serialize(publishedBytes);
    reference.Serialize(referenceBytes);
    EXPECT_EQ(publishedBytes, referenceBytes) << "the job's BVH differs from a synchronous build of the same mesh";

    const auto later = MeshBvhCache::Instance().TryGet(m_Guid, 0, Grid());
    EXPECT_FALSE(later.Pending);
    EXPECT_EQ(later.Bvh, published);
}

TEST_F(SceneRaycastPendingBvhTests, APickAnswersWithTheBoundsUntilTheBvhIsPublishedThenHitsTheSurface)
{
    const Ray3D ray = DownAt(3.1f, -2.7f);
    PickOptions options = RestrictedToGrid();
    options.IncludeBoundsFallback = true;
    const auto whilePending = RaycastScene(ray, m_World, options);
    ASSERT_TRUE(whilePending.Hit);
    EXPECT_EQ(whilePending.Best.Kind, PickableKind::Bounds);
    EXPECT_NEAR(whilePending.Best.WorldPosition.y, 0.5f, 1e-4f) << "not on the top of the bounds";

    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    const auto published = RaycastScene(ray, m_World, options);
    ASSERT_TRUE(published.Hit);
    EXPECT_EQ(published.Best.Kind, PickableKind::Mesh);
    EXPECT_NEAR(published.Best.WorldPosition.y, SurfaceHeight(3.1f, -2.7f), 0.01f);
}

// A renderer with no CPU mesh data has always answered with its bounds; that answer is final.
TEST_F(SceneRaycastPendingBvhTests, ABoundsHitForAMeshWithoutCpuDataIsNotPending)
{
    const GameEngine::ECS::EntityHandle runtime = m_World.CreateEntity();
    Components::MeshRenderer renderer{};
    renderer.modelAssetGuid.Set(GUID::Generate());
    m_World.AddComponentImmediate(runtime, renderer);
    m_World.AddComponentImmediate(runtime, Components::WorldTransform{});
    m_World.AddComponentImmediate(runtime, Components::LocalBounds{});
    PickOptions options;
    options.RestrictToEntity = runtime;
    options.IncludeBoundsFallback = true;

    const auto pick = RaycastScene(DownAt(0.1f, 0.1f), m_World, options);
    ASSERT_TRUE(pick.Hit);
    EXPECT_EQ(pick.Best.Kind, PickableKind::Bounds);
    EXPECT_FALSE(pick.Best.MeshBvhPending);
}

TEST_F(SceneRaycastPendingBvhTests, ABuildInvalidatedWhileItRunsPublishesNothingAndTheNextBuildUsesTheNewGeometry)
{
    const auto buildStart = std::chrono::steady_clock::now();
    (void)MeshPicking::MeshBvh::Build(ViewOf(Grid()));
    const auto buildTime = std::chrono::steady_clock::now() - buildStart;

    MeshBvhCache& cache = MeshBvhCache::Instance();
    const size_t sizeBefore = cache.Size();
    ASSERT_TRUE(cache.TryGet(m_Guid, 0, Grid()).Pending);
    cache.InvalidateAsset(m_Guid);
    Mesh raised = MakeGrid();
    for (GameEngine::Vertex& vertex : raised.Vertices)
        vertex.Position[1] += 5.0f;
    m_Model->SetMeshesForTest({raised});

    // Long past the invalidated job's own build; it must not have published.
    std::this_thread::sleep_for(buildTime * 4 + std::chrono::milliseconds(200));
    EXPECT_EQ(cache.Size(), sizeBefore) << "the invalidated build published its BVH";

    const auto next = cache.TryGet(m_Guid, 0, Grid());
    EXPECT_TRUE(next.Pending) << "the lookup after the invalidation did not start a new build";
    const auto rebuilt = cache.GetOrBuild(m_Guid, 0, Grid());
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_GT(rebuilt->RootBounds().min.y, 4.0f) << "the BVH was built from the geometry before the invalidation";
}

// The pick trusts a ready BVH: when it misses, the pick misses, with no per-triangle pass over
// the mesh after it. The cache key is seeded with a BVH of the grid moved far aside, so a ray
// onto the grid's real triangles misses that BVH; only a per-triangle pass would find them.
TEST_F(SceneRaycastPendingBvhTests, AMissAgainstAReadyBvhIsTheAnswer)
{
    Mesh aside = MakeGrid();
    for (GameEngine::Vertex& vertex : aside.Vertices)
        vertex.Position[0] += 100.0f;
    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, aside), nullptr);

    const auto pick = RaycastScene(DownAt(3.1f, -2.7f), m_World, RestrictedToGrid());
    EXPECT_FALSE(pick.Hit) << "the pick tested the mesh triangle by triangle after its BVH missed";
}

// A synchronous lookup never waits for the Background lane. A ray's lookup queued the key's
// build behind jobs that hold every compute worker; the synchronous lookup that follows (the one
// these tests publish a BVH with) builds the key itself and returns while the lane is still held,
// and the queued job, reached later, finds the build done and returns.
TEST_F(SceneRaycastPendingBvhTests, AWaitingLookupBuildsAQueuedBuildItselfInsteadOfWaitingForTheLane)
{
    JobSystem::WorkStealingThreadPool& pool = EngineCore::GetInstance().GetJobSystem();
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    // Shared: the queued held jobs run after the test body returns.
    const auto held = std::make_shared<std::atomic<size_t>>(0);
    const size_t workers = pool.GetWorkerCount();
    // Every worker held, and more held jobs queued ahead of the build.
    for (size_t i = 0; i < workers + 2; ++i)
    {
        pool.EnqueueWork([held, released]()
                         {
                             held->fetch_add(1);
                             released.wait();
                         },
                         JobSystem::JobPriority::Background);
    }
    const auto heldDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (held->load() < workers && std::chrono::steady_clock::now() < heldDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (held->load() != workers)
        release.set_value();
    ASSERT_EQ(held->load(), workers) << "the held jobs never occupied every worker";

    ASSERT_TRUE(MeshBvhCache::Instance().TryGet(m_Guid, 0, Grid()).Pending);
    std::future<std::shared_ptr<const MeshPicking::MeshBvh>> lookup = std::async(
        std::launch::async, [this]() { return MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()); });
    const bool returnedWhileHeld = lookup.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
    release.set_value();
    const auto bvh = lookup.get();
    EXPECT_TRUE(returnedWhileHeld) << "the synchronous lookup waited for the jobs queued ahead of the build";
    ASSERT_NE(bvh, nullptr);
    EXPECT_FALSE(MeshBvhCache::Instance().TryGet(m_Guid, 0, Grid()).Pending);
}

TEST_F(SceneRaycastPendingBvhTests, AShadowsOnlyMeshIsNotHit)
{
    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    ASSERT_TRUE(RaycastScene(DownAt(1.0f, 1.0f), m_World, PickOptions{}).Hit) << "the control pick";
    m_World.GetComponentForWrite<Components::MeshRenderer>(m_Entity)->shadowCasting =
        Components::ShadowCasting::ShadowsOnly;
    EXPECT_FALSE(RaycastScene(DownAt(1.0f, 1.0f), m_World, PickOptions{}).Hit);
    EXPECT_FALSE(RaycastScene(DownAt(1.0f, 1.0f), m_World, RestrictedToGrid()).Hit);
    std::vector<GameEngine::Scene::PickHit> hits;
    RaycastSceneAll(DownAt(1.0f, 1.0f), m_World, hits, PickOptions{});
    EXPECT_TRUE(hits.empty());
}

// Without the bounds fallback, the default, a pending mesh gives no hit, and the result says the
// answer may change, so a caller that measures the ground holds instead of using the miss.
TEST_F(SceneRaycastPendingBvhTests, WithoutTheBoundsFallbackAPendingMeshIsNoHitMarkedPendingUntilPublished)
{
    PickOptions options;
    options.IncludeBoundsFallback = false;
    options.IncludeTerrain = false;
    const Ray3D ray = DownAt(3.1f, -2.7f);
    const auto whilePending = RaycastScene(ray, m_World, options);
    EXPECT_FALSE(whilePending.Hit) << "the pending mesh answered without its triangles";
    EXPECT_TRUE(whilePending.MeshBvhPending) << "the miss does not say the mesh is pending";

    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    const auto published = RaycastScene(ray, m_World, options);
    ASSERT_TRUE(published.Hit);
    EXPECT_FALSE(published.MeshBvhPending);
    EXPECT_EQ(published.Best.Kind, PickableKind::Mesh);
    EXPECT_NEAR(published.Best.WorldPosition.y, SurfaceHeight(3.1f, -2.7f), 0.01f);
}

// RaycastSceneAll returns true while a mesh on the ray is pending, so a caller that walks the
// hits holds instead of using the list without that mesh.
TEST_F(SceneRaycastPendingBvhTests, RaycastSceneAllReturnsPendingWithNoHitUntilTheBvhIsPublished)
{
    PickOptions options;
    options.IncludeBoundsFallback = false;
    options.IncludeTerrain = false;
    const Ray3D ray = DownAt(3.1f, -2.7f);
    std::vector<GameEngine::Scene::PickHit> hits;
    EXPECT_TRUE(RaycastSceneAll(ray, m_World, hits, options)) << "the call does not say the mesh is pending";
    EXPECT_TRUE(hits.empty()) << "the pending mesh answered without its triangles";

    ASSERT_NE(MeshBvhCache::Instance().GetOrBuild(m_Guid, 0, Grid()), nullptr);
    EXPECT_FALSE(RaycastSceneAll(ray, m_World, hits, options));
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].Kind, PickableKind::Mesh);
    EXPECT_NEAR(hits[0].WorldPosition.y, SurfaceHeight(3.1f, -2.7f), 0.01f);
}
