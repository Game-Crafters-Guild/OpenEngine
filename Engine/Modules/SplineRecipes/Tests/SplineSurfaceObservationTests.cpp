// The surface term of a recipe's rebuild-settle observation. A recipe that reads no ground adds no term, or terrain
// movement holds its settle clock at zero; an extrude that fits its banks reads the ground in every ConformMode. The
// suite pins both directions: a missing term never fits again, a term on every recipe delays the rebuilds.

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplinePlacement.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/Geometry.h"
#include "Scene/SceneTlas.h"
#include "SplineRecipes/SplineSurfaceConform.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace GameEngine;
using Components::ReadsSurface;
using Components::SplineExtrude;
using Components::SplineExtrudeWidthMode;
using Components::SplinePlacementConform;
using SplineRecipes::ObservedSurfaceRevision;

namespace
{

// "Conforms at all" — the whole ground-read story for the placement and fence
// recipes, which have no width fit.
bool Conforms(SplinePlacementConform mode)
{
    return mode != SplinePlacementConform::None;
}

} // namespace

// THE starvation case: a None recipe observed across a moving digest must see
// the same value every frame, so terrain churn cannot reset its settle clock.
TEST(SplineSurfaceObservation, ANoneRecipeObservesNoSurfaceTerm)
{
    const uint64 digestFrameA = 0x1234'5678'9ABC'DEF0ull;
    const uint64 digestFrameB = 0x0FED'CBA9'8765'4321ull;

    EXPECT_EQ(ObservedSurfaceRevision(Conforms(SplinePlacementConform::None), digestFrameA),
              ObservedSurfaceRevision(Conforms(SplinePlacementConform::None), digestFrameB))
        << "a recipe that never probes the ground must be immune to surface churn";
    EXPECT_EQ(ObservedSurfaceRevision(Conforms(SplinePlacementConform::None), digestFrameA), 0u);
}

// Conforming recipes hold measured altitudes, so the digest passes through
// unchanged: ground movement must re-arm their rebuild.
TEST(SplineSurfaceObservation, AConformingRecipeObservesTheWorldDigest)
{
    const uint64 digest = 0xA5A5'5A5A'DEAD'BEEFull;

    EXPECT_EQ(ObservedSurfaceRevision(Conforms(SplinePlacementConform::Height), digest), digest);
    EXPECT_EQ(ObservedSurfaceRevision(Conforms(SplinePlacementConform::HeightAndSlope), digest), digest);
}

// The premise the None rule rested on — "a None recipe never probes the ground"
// — stopped being true when the extrude gained a width mode that measures the
// banks. A FitToBanks recipe reads the ground at every station whatever its
// ConformMode is, so it must observe the world digest: without it, carving the
// bed under a non-conforming river never re-fits its width, and the surface
// keeps the channel it was built against.
TEST(SplineSurfaceObservation, ANoneFitToBanksRecipeObservesTheWorldDigest)
{
    const uint64 digestFrameA = 0x1234'5678'9ABC'DEF0ull;
    const uint64 digestFrameB = 0x0FED'CBA9'8765'4321ull;

    SplineExtrude recipe{};
    recipe.ConformMode = SplinePlacementConform::None;
    recipe.WidthMode = SplineExtrudeWidthMode::FitToBanks;

    EXPECT_TRUE(ReadsSurface(recipe))
        << "fitting to banks is a ground read, conform mode notwithstanding";
    EXPECT_EQ(ObservedSurfaceRevision(ReadsSurface(recipe), digestFrameA), digestFrameA);
    EXPECT_NE(ObservedSurfaceRevision(ReadsSurface(recipe), digestFrameA),
              ObservedSurfaceRevision(ReadsSurface(recipe), digestFrameB))
        << "ground movement must re-arm a fitted recipe's rebuild";
}

// The control that keeps the fix from becoming "always observe the digest":
// a None recipe on the authored width channel reads no ground and must stay
// immune to terrain churn, which is the starvation the None rule exists to
// prevent.
TEST(SplineSurfaceObservation, ANoneChannelRecipeStillObservesNoSurfaceTerm)
{
    const uint64 digestFrameA = 0x1234'5678'9ABC'DEF0ull;
    const uint64 digestFrameB = 0x0FED'CBA9'8765'4321ull;

    SplineExtrude recipe{};
    recipe.ConformMode = SplinePlacementConform::None;
    recipe.WidthMode = SplineExtrudeWidthMode::Channel;

    EXPECT_FALSE(ReadsSurface(recipe));
    EXPECT_EQ(ObservedSurfaceRevision(ReadsSurface(recipe), digestFrameA), 0u);
    EXPECT_EQ(ObservedSurfaceRevision(ReadsSurface(recipe), digestFrameA),
              ObservedSurfaceRevision(ReadsSurface(recipe), digestFrameB));
}

// A conforming recipe reads the ground for its drape whatever its width mode,
// so neither mode may switch the surface term off.
TEST(SplineSurfaceObservation, AConformingRecipeReadsTheSurfaceInEitherWidthMode)
{
    SplineExtrude channel{};
    channel.ConformMode = SplinePlacementConform::Height;
    channel.WidthMode = SplineExtrudeWidthMode::Channel;
    EXPECT_TRUE(ReadsSurface(channel));

    SplineExtrude fitted{};
    fitted.ConformMode = SplinePlacementConform::Height;
    fitted.WidthMode = SplineExtrudeWidthMode::FitToBanks;
    EXPECT_TRUE(ReadsSurface(fitted));
}

// ---------------------------------------------------------------------------
// The readiness half: whether the ground is THERE yet, as opposed to whether it
// MOVED. The digest above cannot answer it — an unprovisioned terrain folds no
// terms, so it reads exactly like a settled one — and a recipe's first build has
// no earlier state to re-place from when the real ground finally arrives. These
// pin the gate that holds that first build, and each direction is a live failure
// mode: holding too little reinstates paving on rooftops, holding too much
// leaves a terrainless scene permanently empty.
// ---------------------------------------------------------------------------

using SplineRecipes::ConformSurfaceReadiness;
using SplineRecipes::DeferFirstBuild;
using SplineRecipes::kFirstBuildDeferBudgetSeconds;

namespace
{

// The call the controllers make, with the per-recipe defer state owned here.
struct DeferState
{
    float32 Seconds = 0.0f;
    bool Warned = false;

    bool Defer(SplineRecipes::ConformSurfaceReadiness readiness, bool readsSurface, bool appliedOnce,
               bool surfaceSettled, float32 deltaSeconds = 1.0f / 60.0f)
    {
        return DeferFirstBuild(readiness, readsSurface, appliedOnce, surfaceSettled, deltaSeconds,
                               /*entityId*/ 7u, Seconds, Warned);
    }
};

} // namespace

// THE case: a ground-reading recipe whose terrain is declared but not yet
// provisioned must not commit a first build. This is the whole defect -- on
// scene load every entity exists while terrain data arrives from the
// asset-resolve pump tens of seconds later, and a build taken in that window
// measures the props instead.
TEST(SplineSurfaceReadiness, DefersAFirstBuildWhileTheTerrainIsStillProvisioning)
{
    DeferState d;
    EXPECT_TRUE(d.Defer(SplineRecipes::ConformSurfaceReadiness::Provisioning, true, false, /*settled*/ true));
    EXPECT_GT(d.Seconds, 0.0f) << "the clock must run while it defers, or the budget below can "
                                  "never be spent";
}

// The second reason, and the one a readiness check alone would miss: the ground
// is here but still moving. A terrain fills its base heights on one frame and
// composes its modifiers on another, so Ready is not by itself a safe moment to
// take a first measurement.
TEST(SplineSurfaceReadiness, DefersAFirstBuildWhileAReadySurfaceIsStillMoving)
{
    DeferState d;
    EXPECT_TRUE(d.Defer(SplineRecipes::ConformSurfaceReadiness::Ready, true, false, /*settled*/ false));
}

// A first build is deferred on a Ready-but-moving surface WITHOUT having to have
// observed Provisioning first. This is not a preference — a latch on "saw
// Provisioning" was tried and falsified on a large island scene: the terrain's handles
// resolve before the recipes become eligible, so the recipes this gate exists
// for never observe that state, and the latch restored the whole defect.
TEST(SplineSurfaceReadiness, DefersOnAMovingSurfaceEvenIfProvisioningWasNeverObserved)
{
    DeferState d;
    // First call of this recipe's life, and the world is already Ready.
    EXPECT_TRUE(d.Defer(SplineRecipes::ConformSurfaceReadiness::Ready, true, false, /*settled*/ false))
        << "the recipes this gate exists for never see Provisioning; requiring it "
           "reinstates the defect";
}

// THE STARVATION GUARD. A tiled terrain bumps the surface digest for as long as
// tiles keep arriving, so "wait for the digest to hold still" can wait forever.
// An unbounded settle would trade paving-on-rooftops for no paving at all --
// a quieter defect, not a fixed one.
TEST(SplineSurfaceReadiness, StopsDeferringAReadySurfaceThatNeverSettles)
{
    DeferState d;
    ASSERT_TRUE(d.Defer(SplineRecipes::ConformSurfaceReadiness::Ready, true, false, false,
                        SplineRecipes::kFirstBuildDeferBudgetSeconds - 1.0f));
    EXPECT_FALSE(d.Warned);

    EXPECT_FALSE(d.Defer(SplineRecipes::ConformSurfaceReadiness::Ready, true, false, false, 2.0f))
        << "a surface that never stops moving must not strand the first build forever";
    EXPECT_TRUE(d.Warned);
}

// The opposite failure, and the constraint that rules out "just wait for a
// stable digest": a scene with no terrain at all has nothing coming, so its
// conforming recipes must place on sight rather than wait out the budget --
// and they must not be held for settling either.
TEST(SplineSurfaceReadiness, NeverDefersWhenTheSceneHasNoTerrain)
{
    DeferState d;
    EXPECT_FALSE(d.Defer(SplineRecipes::ConformSurfaceReadiness::NoSurface, true, false, /*settled*/ false));
    EXPECT_EQ(d.Seconds, 0.0f);
}

// A recipe that never probes the ground has nothing to wait for; deferring it
// would delay every non-conforming placement in a scene behind terrain load.
TEST(SplineSurfaceReadiness, NeverDefersARecipeThatDoesNotReadTheGround)
{
    DeferState d;
    EXPECT_FALSE(d.Defer(SplineRecipes::ConformSurfaceReadiness::Provisioning, false, false, false));
    EXPECT_EQ(d.Seconds, 0.0f);
}

// Only the FIRST build is gated. Later builds already re-place through the
// settle window, and deferring them would starve edits made while a terrain is
// mid-provision.
TEST(SplineSurfaceReadiness, NeverDefersOnceTheRecipeHasBuiltBefore)
{
    DeferState d;
    EXPECT_FALSE(d.Defer(SplineRecipes::ConformSurfaceReadiness::Provisioning, true, /*appliedOnce*/ true, false));
}

// Ready AND settled is the release edge -- the only state a first measurement
// can be trusted against.
TEST(SplineSurfaceReadiness, ReleasesTheBuildOnceTheSurfaceIsReadyAndSettled)
{
    DeferState d;
    ASSERT_TRUE(d.Defer(SplineRecipes::ConformSurfaceReadiness::Provisioning, true, false, true));
    EXPECT_FALSE(d.Defer(SplineRecipes::ConformSurfaceReadiness::Ready, true, false, /*settled*/ true));
    EXPECT_EQ(d.Seconds, 0.0f) << "the clock must reset when the ground is final, so a later "
                                  "provision gets the full budget again";
}

// Giving up is once, not every frame: re-warning would flood the log.
TEST(SplineSurfaceReadiness, WarnsOnlyOnceAfterGivingUp)
{
    DeferState d;
    ASSERT_FALSE(d.Defer(SplineRecipes::ConformSurfaceReadiness::Provisioning, true, false, true,
                         SplineRecipes::kFirstBuildDeferBudgetSeconds + 1.0f));
    ASSERT_TRUE(d.Warned);
    const bool warnedBefore = d.Warned;
    EXPECT_FALSE(d.Defer(SplineRecipes::ConformSurfaceReadiness::Provisioning, true, false, true, 1.0f));
    EXPECT_EQ(d.Warned, warnedBefore);
}

namespace
{

// A unit cube mesh at `position`; `runtimeOnly` tags it as generated output.
ECS::EntityHandle CreateMesh(ECS::World& world, const Mathematics::Vector3& position, bool runtimeOnly)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::MeshRenderer renderer{};
    renderer.modelAssetGuid.Set(Engine::Renderer::PrimitiveGenerator::CubeGuid());
    world.AddComponentImmediate(entity, renderer);
    Components::WorldTransform transform{};
    transform.matrix[12] = position.x;
    transform.matrix[13] = position.y;
    transform.matrix[14] = position.z;
    transform.Version = 1u;
    world.AddComponentImmediate(entity, transform);
    Components::LocalBounds bounds{};
    bounds.Box = Mathematics::BoundingBox::FromMinMax(Mathematics::Vector3(-0.5f, -0.5f, -0.5f),
                                                      Mathematics::Vector3(0.5f, 0.5f, 0.5f));
    world.AddComponentImmediate(entity, bounds);
    if (runtimeOnly)
        world.AddComponentImmediate(entity, Components::RuntimeOnlyEntity{});
    return entity;
}

// Holds a one-worker pool's only worker until destroyed, so a scene TLAS job queued behind it
// stays in flight.
class WorkerBlocker
{
public:
    explicit WorkerBlocker(JobSystem::WorkStealingThreadPool& pool) : m_Pool(pool)
    {
        m_Pool.Run([this] { HoldTheWorker(); }, m_Counter);
        while (!m_Started.load(std::memory_order_acquire))
            std::this_thread::yield();
    }
    ~WorkerBlocker()
    {
        m_Released.store(true, std::memory_order_release);
        m_Pool.Wait(m_Counter);
    }
    WorkerBlocker(const WorkerBlocker&) = delete;
    WorkerBlocker& operator=(const WorkerBlocker&) = delete;

private:
    void HoldTheWorker()
    {
        m_Started.store(true, std::memory_order_release);
        while (!m_Released.load(std::memory_order_acquire))
            std::this_thread::yield();
    }

    JobSystem::WorkStealingThreadPool& m_Pool;
    JobSystem::JobCounter m_Counter;
    std::atomic<bool> m_Started{false};
    std::atomic<bool> m_Released{false};
};

uint64 DigestWhenCurrent(ECS::World& world, const Mathematics::AABB& run)
{
    Scene::GetSceneTlas(world).WaitCurrent(world);
    SplineRecipes::SceneConformDigest digest;
    SplineRecipes::RefreshSceneConformDigest(world, run, digest);
    return digest.Value;
}

} // namespace

TEST(SplineSceneConformDigest, MovesWithAMeshUnderTheRunAndNotWithGeneratedOrFarMeshes)
{
    ECS::World world;
    Mathematics::AABB run = Mathematics::AABB::Empty();
    run.Expand(Mathematics::Vector3(0.0f, 0.0f, 0.0f));
    run.Expand(Mathematics::Vector3(50.0f, 0.0f, 0.0f));
    const ECS::EntityHandle prop = CreateMesh(world, Mathematics::Vector3(10.0f, 0.0f, 0.0f), false);
    const ECS::EntityHandle far = CreateMesh(world, Mathematics::Vector3(500.0f, 0.0f, 500.0f), false);
    const uint64 built = DigestWhenCurrent(world, run);
    EXPECT_NE(built, 0u) << "the mesh under the run is not in the digest";

    CreateMesh(world, Mathematics::Vector3(20.0f, 0.0f, 0.0f), true);
    EXPECT_EQ(DigestWhenCurrent(world, run), built) << "a runtime-only mesh moved the digest";

    auto* farTransform = world.GetComponentForWrite<Components::WorldTransform>(far);
    farTransform->matrix[12] = 600.0f;
    ++farTransform->Version;
    EXPECT_EQ(DigestWhenCurrent(world, run), built) << "a mesh far from the run moved the digest";

    auto* propTransform = world.GetComponentForWrite<Components::WorldTransform>(prop);
    propTransform->matrix[12] = 30.0f;
    ++propTransform->Version;
    EXPECT_NE(DigestWhenCurrent(world, run), built) << "the mesh under the run moved and the digest did not";
    Scene::ReleaseSceneTlas(world);
}

// A frame with no change to the scene TLAS walks nothing; a move of a mesh walks once.
TEST(SplineSceneConformDigest, WalksTheSceneOnlyWhenTheSceneTlasChanges)
{
    constexpr int kFrames = 10;
    ECS::World world;
    Mathematics::AABB run = Mathematics::AABB::Empty();
    run.Expand(Mathematics::Vector3(0.0f, 0.0f, 0.0f));
    run.Expand(Mathematics::Vector3(50.0f, 0.0f, 0.0f));
    const ECS::EntityHandle prop = CreateMesh(world, Mathematics::Vector3(10.0f, 0.0f, 0.0f), false);
    SplineRecipes::SceneConformDigest digest;
    int walks = 0;
    for (int frame = 0; frame < kFrames; ++frame)
        walks += SplineRecipes::RefreshSceneConformDigest(world, run, digest) ? 1 : 0;
    EXPECT_EQ(walks, 1) << "a frame with no change to the scene walked it";
    const uint64 atRest = digest.Value;

    auto* propTransform = world.GetComponentForWrite<Components::WorldTransform>(prop);
    propTransform->matrix[12] = 30.0f;
    ++propTransform->Version;
    walks = 0;
    for (int frame = 0; frame < kFrames; ++frame)
        walks += SplineRecipes::RefreshSceneConformDigest(world, run, digest) ? 1 : 0;
    EXPECT_EQ(walks, 1) << "a mesh move did not walk the scene once";
    EXPECT_NE(digest.Value, atRest) << "the walk after the move did not see it";
    Scene::ReleaseSceneTlas(world);
}

// While the scene TLAS has work in flight the digest walks nothing, also for a run that changed,
// so no walk reads a tree that a job writes. The walk runs once the work lands.
TEST(SplineSceneConformDigest, WalksNothingWhileTheSceneTlasIsUpdating)
{
    JobSystem::WorkStealingThreadPool pool(1);
    ECS::World world;
    world.SetJobSystem(&pool);
    Mathematics::AABB run = Mathematics::AABB::Empty();
    run.Expand(Mathematics::Vector3(0.0f, 0.0f, 0.0f));
    run.Expand(Mathematics::Vector3(50.0f, 0.0f, 0.0f));
    const ECS::EntityHandle prop = CreateMesh(world, Mathematics::Vector3(10.0f, 0.0f, 0.0f), false);
    Scene::SceneTlas& tlas = Scene::GetSceneTlas(world);
    tlas.WaitCurrent(world);
    SplineRecipes::SceneConformDigest digest;
    ASSERT_TRUE(SplineRecipes::RefreshSceneConformDigest(world, run, digest));
    const uint64 versionAtRest = digest.TlasVersion;

    auto* propTransform = world.GetComponentForWrite<Components::WorldTransform>(prop);
    propTransform->matrix[12] = 30.0f;
    ++propTransform->Version;
    run.Expand(Mathematics::Vector3(60.0f, 0.0f, 0.0f));
    {
        WorkerBlocker blocker(pool);
        EXPECT_FALSE(SplineRecipes::RefreshSceneConformDigest(world, run, digest))
            << "the digest walked the scene while its TLAS was updating";
        EXPECT_TRUE(tlas.IsWorkInFlightForTesting()) << "the refit did not stay in flight, so the check proves nothing";
        EXPECT_EQ(digest.TlasVersion, versionAtRest);
    }
    while (tlas.IsWorkInFlightForTesting())
        std::this_thread::yield();
    EXPECT_TRUE(SplineRecipes::RefreshSceneConformDigest(world, run, digest)) << "the walk did not run after the work landed";
    EXPECT_NE(digest.TlasVersion, versionAtRest);
    Scene::ReleaseSceneTlas(world);
}
