#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Camera.h"
#include "Components/Transform.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Camera/CameraDerivation.h"
#include "Rendering/Camera/CameraTypes.h"
#include "TerrainECS/TerrainRenderedViews.h"
#include "TestCameraEye.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

// Device-free equivalence locks for the camera/view registries (A1.2 L5).
// Written against the CURRENT RenderServices public surface so the S1
// extraction (state + bodies move behind unchanged forwarders) leaves them
// untouched — they never re-point. No device is created: every registry
// operation exercised here is pure CPU bookkeeping (ReleaseView's GPU
// teardown is a no-op with a null device).

// L5(a): GetViews() iterates in allocation order, and that order survives
// interleaved allocate/release (the vector storage + append-on-allocate +
// stable erase drive per-view pass declaration order downstream).
TEST(ViewRegistryTests, GetViewsIterationOrderIsAllocationOrder)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId v1 = rs.Views().AllocateView("v1", cam);
    const ViewId v2 = rs.Views().AllocateView("v2", cam);
    const ViewId v3 = rs.Views().AllocateView("v3", cam);

    ASSERT_TRUE(rs.Views().ReleaseView(v2));
    const ViewId v4 = rs.Views().AllocateView("v4", cam);

    const auto& views = rs.Views().GetViews();
    ASSERT_EQ(views.size(), 3u);
    EXPECT_EQ(views[0].id, v1);
    EXPECT_EQ(views[1].id, v3);
    EXPECT_EQ(views[2].id, v4);
}

// L5(b): ReleaseCamera refuses while a view still references the camera, and
// succeeds once the view is released.
// A camera that a view draws before its owner sets the data stays unset (zero matrices) with a valid eye.
TEST(ViewRegistryTests, ACameraBeforeItsFirstDataHasItsEyeAtTheOrigin)
{
    RenderServices rs;
    const CameraData* data = rs.Views().FindCameraData(rs.Views().AllocateCamera("cam"));
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->proj[0][0], 0.0f);
    EXPECT_EQ(data->proj[1][1], 0.0f);
    EXPECT_EQ(CameraRenderOriginSector(*data), (Mathematics::Vector3Int{0, 0, 0}));
}

TEST(ViewRegistryTests, ReleaseCameraRefusedWhileViewReferencesIt)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("view", cam);

    EXPECT_FALSE(rs.Views().ReleaseCamera(cam)) << "camera still referenced by a live view";

    ASSERT_TRUE(rs.Views().ReleaseView(view));
    EXPECT_TRUE(rs.Views().ReleaseCamera(cam)) << "camera releasable once no view references it";
}

// L5(c): ReleaseCamera erases the per-camera side tables (exposure + post-
// process mask). CameraIds are increment-only (no recycling path exists), so
// this erase is defense-in-depth (A3-iii): the test pins the erase-on-release,
// not recycling protection.
TEST(ViewRegistryTests, ReleaseCameraErasesPerCameraSideTables)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");

    ViewRegistry::CameraExposure exposure{};
    exposure.Mode = 1;
    rs.Views().SetCameraExposure(cam, exposure);
    rs.Views().SetCameraVolumeLayers(cam, GameEngine::Rendering::RenderLayerMask{0x2u});

    ASSERT_NE(rs.Views().FindCameraExposure(cam), nullptr);
    EXPECT_TRUE(rs.Views().HasCameraVolumeLayers(cam));

    ASSERT_TRUE(rs.Views().ReleaseCamera(cam));

    EXPECT_EQ(rs.Views().FindCameraExposure(cam), nullptr);
    EXPECT_FALSE(rs.Views().HasCameraVolumeLayers(cam));
}

// L5(d) — S0 subset: ReleaseView succeeds exactly once (double-release is a
// no-op), and after release the view descriptor is gone and per-view knob
// getters fall back to defaults. (The A1 release-erase assertions —
// forcedLOD/pp/fog/warned + callback fire-count — land in S1 when the fix does.)
TEST(ViewRegistryTests, ReleaseViewIsIdempotentAndDropsPerViewState)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("view", cam);

    ViewLetterbox lb{};
    lb.active = true;
    lb.width = 640;
    lb.height = 360;
    rs.Views().SetViewLetterbox(view, lb);
    EXPECT_TRUE(rs.Views().GetViewLetterbox(view).active);

    EXPECT_TRUE(rs.Views().ReleaseView(view)) << "first release succeeds";
    EXPECT_FALSE(rs.Views().ReleaseView(view)) << "double-release is a no-op";

    EXPECT_EQ(rs.Views().FindViewDesc(view), nullptr);
    EXPECT_FALSE(rs.Views().GetViewLetterbox(view).active) << "per-view knob defaults after release";
}

// L5(d) — A1 extension (S1): ReleaseView additionally erases the per-view
// override maps that historically leaked (insert-only), and fires the view-
// released callback exactly once per successful release. After release the
// post-process override for the (now-defunct) view is gone, so the effective
// settings fall back to the world value. Double-release is a no-op that does
// not fire the callback.
TEST(ViewRegistryTests, ReleaseViewErasesOverridesAndFiresCallbackOnce)
{
    RenderServices rs;

    int releaseCount = 0;
    rs.Views().SetViewReleasedCallback([&](ViewId) { ++releaseCount; });

    const uint64_t worldId = 7ull;
    PostProcessSettings worldPp{};
    worldPp.Exposure = 2.0f;
    rs.SetWorldPostProcessSettings(worldId, worldPp);

    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("view", cam);

    PostProcessSettings viewPp{};
    viewPp.Exposure = 5.0f;
    rs.Views().SetViewPostProcessOverride(view, viewPp);
    EXPECT_FLOAT_EQ(rs.GetEffectivePostProcessSettings(view, worldId).Exposure, 5.0f)
        << "per-view override wins while set";

    EXPECT_TRUE(rs.Views().ReleaseView(view)) << "first release succeeds";
    EXPECT_EQ(releaseCount, 1) << "released callback fires exactly once on success";

    // A1: the post-process override was erased with the rest of the per-view
    // state, so the effective settings fall back to the world value.
    EXPECT_FLOAT_EQ(rs.GetEffectivePostProcessSettings(view, worldId).Exposure, 2.0f)
        << "per-view post-process override erased on release (A1)";

    EXPECT_FALSE(rs.Views().ReleaseView(view)) << "double-release is a no-op";
    EXPECT_EQ(releaseCount, 1) << "double-release does not fire the callback";
}

// L5(e): an OnDemand view starts expired — ActiveRenderLayerMask() is 0 until
// RequestViewFrame arms it (the mechanism probe capture / hidden panes lean on
// to lapse out of GPU work without a ReleaseView).
TEST(ViewRegistryTests, OnDemandViewStartsExpiredUntilArmed)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("onDemand", cam, ViewPurpose::EditorScene,
                                        ViewParticipation::OnDemand);
    rs.Views().SetViewRenderLayerMask(view, GameEngine::Rendering::RenderLayerMask::Default());

    const ViewDesc* dormant = rs.Views().FindViewDesc(view);
    ASSERT_NE(dormant, nullptr);
    EXPECT_EQ(dormant->ActiveRenderLayerMask().Bits, 0u) << "unarmed OnDemand view is dormant";

    rs.Views().RequestViewFrame(view);

    const ViewDesc* armed = rs.Views().FindViewDesc(view);
    ASSERT_NE(armed, nullptr);
    EXPECT_NE(armed->ActiveRenderLayerMask().Bits, 0u) << "armed OnDemand view participates";
}

// A1.6 (D9) hide -> lapse: when a scene/game pane hides, the editor routing
// STOPS re-arming the view but does NOT release it. Modeled device-free: after
// one armed (visible) frame, two BeginFrame decrements with no re-arm drive
// participationFrames to 0. The view stays registered (viewId survives) and
// ActiveRenderLayerMask() gates to 0 within two frames, so extraction / batch-
// key build / culling / shadows cost nothing while hidden — and nothing re-arms
// it on its own.
TEST(ViewRegistryTests, A16_HideLapsesOnDemandViewWithoutReleasing)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("onDemand", cam, ViewPurpose::EditorScene,
                                        ViewParticipation::OnDemand);
    rs.Views().SetViewRenderLayerMask(view, GameEngine::Rendering::RenderLayerMask::Default());

    // Visible frame: the pane declares, arming the view for this frame + the next.
    rs.Views().RequestViewFrame(view);
    ASSERT_NE(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask().Bits, 0u)
        << "armed OnDemand view participates while visible";

    // Frame 1 after hide (no re-arm): still fed by the prior request.
    rs.Views().BeginFrame();
    EXPECT_NE(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask().Bits, 0u)
        << "still participates one frame after hide (request covers this frame)";

    // Frame 2 after hide: lapses to dormant. View is NOT released — it is still
    // registered, just contributing zero per-frame work.
    rs.Views().BeginFrame();
    const ViewDesc* lapsed = rs.Views().FindViewDesc(view);
    ASSERT_NE(lapsed, nullptr) << "hidden view stays registered (release is destroy-only)";
    EXPECT_EQ(lapsed->ActiveRenderLayerMask().Bits, 0u) << "lapsed within two frames of hide";

    // A further frame with no re-arm stays dormant — nothing self-arms it.
    rs.Views().BeginFrame();
    const ViewDesc* stillDormant = rs.Views().FindViewDesc(view);
    ASSERT_NE(stillDormant, nullptr);
    EXPECT_EQ(stillDormant->ActiveRenderLayerMask().Bits, 0u)
        << "no re-arm while hidden: stays dormant, no orphan cull loop";
}

// A1.6 (D9) hide -> quick reshow: because a hidden pane lapses instead of
// releasing, the SAME viewId is still registered when the pane reopens. The
// show path reuses it (SceneView/GameView DeclareTargetsRG only Allocate when
// m_ViewId == 0), so a single RequestViewFrame re-arms participation and the
// view renders the next frame — the warm reopen (viewId + HZB history intact).
TEST(ViewRegistryTests, A16_QuickReshowReusesSameViewIdAndRearms)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("onDemand", cam, ViewPurpose::EditorScene,
                                        ViewParticipation::OnDemand);
    rs.Views().SetViewRenderLayerMask(view, GameEngine::Rendering::RenderLayerMask::Default());

    // Visible, then hide -> lapse to dormant (two decrements, no re-arm).
    rs.Views().RequestViewFrame(view);
    rs.Views().BeginFrame();
    rs.Views().BeginFrame();
    ASSERT_EQ(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask().Bits, 0u)
        << "dormant after hide";

    // Reshow: the controller declares again and re-arms. It never re-allocated,
    // so the id is byte-identical to the pre-hide id (warm reopen).
    rs.Views().RequestViewFrame(view);
    const ViewDesc* rearmed = rs.Views().FindViewDesc(view);
    ASSERT_NE(rearmed, nullptr);
    EXPECT_EQ(rearmed->id, view) << "same viewId across hide/reshow (never released)";
    EXPECT_NE(rearmed->ActiveRenderLayerMask().Bits, 0u) << "reshow re-arms participation";

    // Next frame's extraction is fed: still participating after one decrement.
    rs.Views().BeginFrame();
    EXPECT_NE(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask().Bits, 0u)
        << "reshown view renders the next frame";
}

// A1.6 (D9) destroy: release-on-destroy stays mandatory. The controller
// destructor path (DeactivateRenderView -> ReleaseView) actually removes the
// view from the registry, unlike hide which only lapses it.
TEST(ViewRegistryTests, A16_DestroyReleasesViewFromRegistry)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("onDemand", cam, ViewPurpose::EditorScene,
                                        ViewParticipation::OnDemand);
    rs.Views().RequestViewFrame(view);
    ASSERT_NE(rs.Views().FindViewDesc(view), nullptr) << "view registered while alive";

    // Controller teardown releases the view (and then its camera).
    EXPECT_TRUE(rs.Views().ReleaseView(view)) << "destroy releases the view";
    EXPECT_EQ(rs.Views().FindViewDesc(view), nullptr) << "view gone from registry after destroy";
    EXPECT_TRUE(rs.Views().ReleaseCamera(cam)) << "camera releasable once its view is gone";
}

// A Game View whose world has no camera sleeps: it is OnDemand and nothing arms it, so its effective
// layers are None and no extraction, culling slice or pass reads it. With a camera it draws the camera's
// Visible Layers for the frame it is declared in and the next.
TEST(ViewRegistryTests, ACameraLessGameViewSleepsAndACameraGivesItsLayers)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("Game View", cam, ViewPurpose::Game, ViewParticipation::OnDemand);

    DeclareGameCameraView(rs.Views(), view, nullptr);
    EXPECT_EQ(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask(), RenderLayerMask::None())
        << "a camera-less Game View gets no submissions and no culling slice";

    Camera camera{};
    camera.params.VisibleLayers = RenderLayerMask::FromLayer(0) | RenderLayerMask::FromLayer(4);
    DeclareGameCameraView(rs.Views(), view, &camera);
    EXPECT_EQ(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask(), camera.params.VisibleLayers);

    rs.Views().BeginFrame();
    rs.Views().BeginFrame();
    DeclareGameCameraView(rs.Views(), view, nullptr);
    EXPECT_EQ(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask(), RenderLayerMask::None())
        << "the camera went away: the view sleeps again once its last arm lapses";
}

// A camera with World Drawing Paused pauses its Game View: the view draws no layer, so no extraction,
// culling or shadow pass reads it. Clearing the field gives the camera's layers back.
TEST(ViewRegistryTests, ACameraWithWorldDrawingPausedPausesItsGameView)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("Game View", cam, ViewPurpose::Game, ViewParticipation::OnDemand);

    Camera camera{};
    camera.params.VisibleLayers = RenderLayerMask::FromLayer(2);
    camera.params.WorldDrawingPaused = true;
    DeclareGameCameraView(rs.Views(), view, &camera);
    EXPECT_TRUE(rs.Views().FindViewDesc(view)->worldPaused);
    EXPECT_EQ(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask(), RenderLayerMask::None())
        << "a paused view gets no submissions and no culling slice";

    camera.params.WorldDrawingPaused = false;
    DeclareGameCameraView(rs.Views(), view, &camera);
    EXPECT_FALSE(rs.Views().FindViewDesc(view)->worldPaused);
    EXPECT_EQ(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask(), camera.params.VisibleLayers);
}

namespace
{
// A view of `purpose` drawing world `worldId` through its own new camera; returns that camera.
CameraId AddWorldView(RenderServices& rs, const char* name, ViewPurpose purpose, uint64 worldId)
{
    const CameraId camera = rs.Views().AllocateCamera(name);
    rs.Views().SetViewWorldId(rs.Views().AllocateView(name, camera, purpose), worldId);
    return camera;
}
} // namespace

// The main camera is the first Game view's, else the first Scene view's, whichever was allocated
// first; a world no view draws has none.
TEST(ViewRegistryTests, TheMainCameraIsTheGameViewsElseTheSceneViews)
{
    RenderServices sceneFirst;
    AddWorldView(sceneFirst, "scene", ViewPurpose::EditorScene, 7);
    const CameraId sceneFirstGame = AddWorldView(sceneFirst, "game", ViewPurpose::Game, 7);
    EXPECT_EQ(sceneFirst.Views().FindMainCameraData(7), sceneFirst.Views().FindCameraData(sceneFirstGame));

    RenderServices gameFirst;
    const CameraId gameFirstGame = AddWorldView(gameFirst, "game", ViewPurpose::Game, 7);
    AddWorldView(gameFirst, "scene", ViewPurpose::EditorScene, 7);
    EXPECT_EQ(gameFirst.Views().FindMainCameraData(7), gameFirst.Views().FindCameraData(gameFirstGame));

    RenderServices gameOnly;
    const CameraId onlyGame = AddWorldView(gameOnly, "game", ViewPurpose::Game, 7);
    EXPECT_EQ(gameOnly.Views().FindMainCameraData(7), gameOnly.Views().FindCameraData(onlyGame));

    RenderServices sceneOnly;
    const CameraId onlyScene = AddWorldView(sceneOnly, "scene", ViewPurpose::EditorScene, 7);
    EXPECT_EQ(sceneOnly.Views().FindMainCameraData(7), sceneOnly.Views().FindCameraData(onlyScene));

    RenderServices otherWorld;
    AddWorldView(otherWorld, "game", ViewPurpose::Game, 9);
    EXPECT_EQ(otherWorld.Views().FindMainCameraData(7), nullptr);
    RenderServices empty;
    EXPECT_EQ(empty.Views().FindMainCameraData(7), nullptr);
}

// The main camera's view draws the world: a view with world id 0 draws every world, and world id 0
// asks for every world.
TEST(ViewRegistryTests, TheMainCameraComesFromAViewThatDrawsTheWorld)
{
    RenderServices rs;
    const CameraId other = AddWorldView(rs, "other world", ViewPurpose::Game, 9);
    const CameraId main = AddWorldView(rs, "world 7", ViewPurpose::Game, 7);
    const CameraId shared = AddWorldView(rs, "every world", ViewPurpose::Game, 0);

    EXPECT_EQ(rs.Views().FindMainCameraData(7), rs.Views().FindCameraData(main));
    EXPECT_EQ(rs.Views().FindMainCameraData(9), rs.Views().FindCameraData(other));
    EXPECT_EQ(rs.Views().FindMainCameraData(3), rs.Views().FindCameraData(shared));
    EXPECT_EQ(rs.Views().FindMainCameraData(0), rs.Views().FindCameraData(other));
}

// Previews, utility captures, a view off the active pipeline and a sleeping view are never the main
// camera, wherever they stand in allocation order; an armed view is.
TEST(ViewRegistryTests, OnlyAViewTakingPartThroughThePipelineIsTheMainCamera)
{
    RenderServices rs;
    AddWorldView(rs, "preview", ViewPurpose::EditorPreview, 7);
    AddWorldView(rs, "capture", ViewPurpose::UtilityCapture, 7);
    const CameraId offPipelineCamera = rs.Views().AllocateCamera("off pipeline");
    const ViewId offPipeline = rs.Views().AllocateView("off pipeline", offPipelineCamera, ViewPurpose::Game);
    rs.Views().SetViewActiveRenderPipeline(offPipeline, false);
    const CameraId sleepingCamera = rs.Views().AllocateCamera("sleeping");
    const ViewId sleeping =
        rs.Views().AllocateView("sleeping", sleepingCamera, ViewPurpose::Game, ViewParticipation::OnDemand);
    const CameraId scene = AddWorldView(rs, "scene", ViewPurpose::EditorScene, 7);
    EXPECT_EQ(rs.Views().FindMainCameraData(7), rs.Views().FindCameraData(scene));

    rs.Views().RequestViewFrame(sleeping);
    EXPECT_EQ(rs.Views().FindMainCameraData(7), rs.Views().FindCameraData(sleepingCamera))
        << "an armed Game view takes part this frame";
}

// A parked Game view that was armed once is not rendered two frames later. The Scene View, armed
// each frame, is rendered. A view off the pipeline is never rendered.
TEST(ViewRegistryTests, AParkedViewIsNotRenderedTwoFramesAfterItsLastRequest)
{
    RenderServices rs;
    const CameraId gameCamera = rs.Views().AllocateCamera("game");
    const ViewId game = rs.Views().AllocateView("Game View", gameCamera, ViewPurpose::Game, ViewParticipation::OnDemand);
    const CameraId sceneCamera = rs.Views().AllocateCamera("scene");
    const ViewId scene =
        rs.Views().AllocateView("Scene View", sceneCamera, ViewPurpose::EditorScene, ViewParticipation::OnDemand);
    const CameraId offCamera = rs.Views().AllocateCamera("off pipeline");
    const ViewId offPipeline = rs.Views().AllocateView("off pipeline", offCamera, ViewPurpose::Game);
    rs.Views().SetViewActiveRenderPipeline(offPipeline, false);

    rs.Views().RequestViewFrame(game);
    for (int frame = 0; frame < 2; ++frame)
    {
        rs.Views().RequestViewFrame(scene);
        rs.Views().BeginFrame();
    }

    EXPECT_FALSE(rs.Views().FindViewDesc(game)->RenderedByPipeline()) << "the parked Game view lapsed";
    EXPECT_TRUE(rs.Views().FindViewDesc(scene)->RenderedByPipeline()) << "the Scene View is armed each frame";
    EXPECT_FALSE(rs.Views().FindViewDesc(offPipeline)->RenderedByPipeline()) << "the pipeline skips this view";
}

namespace
{
// A view whose camera stands at `eye` and looks along +Z.
ViewId AddViewAt(RenderServices& rs, const char* name, ViewPurpose purpose, ViewParticipation participation,
                 const Mathematics::Vector3& eye)
{
    const CameraId camera = rs.Views().AllocateCamera(name);
    CameraData data{};
    data.view = Mathematics::MakeLookAtLH(eye, eye + Mathematics::Vector3{0.0f, 0.0f, 1.0f},
                                          Mathematics::Vector3{0.0f, 1.0f, 0.0f});
    data.proj = Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0f, 1.5f, 0.5f, 100.0f);
    rs.Views().SetCameraData(camera, ::GameEngine::Tests::TestCameraWithEye(data), {0.5f, 100.0f});
    return rs.Views().AllocateView(name, camera, purpose, participation);
}
} // namespace

// The terrain collects only the views the pipeline renders: of a parked Game view, a view off the
// pipeline and an armed Scene View, it collects one camera, the Scene View's.
TEST(ViewRegistryTests, TheTerrainCollectsOnlyTheRenderedSceneView)
{
    RenderServices rs;
    const ViewId game =
        AddViewAt(rs, "Game View", ViewPurpose::Game, ViewParticipation::OnDemand, {100.0f, 5.0f, 0.0f});
    const ViewId offPipeline =
        AddViewAt(rs, "off pipeline", ViewPurpose::Game, ViewParticipation::Always, {200.0f, 5.0f, 0.0f});
    rs.Views().SetViewActiveRenderPipeline(offPipeline, false);
    const ViewId scene =
        AddViewAt(rs, "Scene View", ViewPurpose::EditorScene, ViewParticipation::OnDemand, {0.0f, 1000.0f, 0.0f});

    rs.Views().RequestViewFrame(game);
    for (int frame = 0; frame < 2; ++frame)
    {
        rs.Views().RequestViewFrame(scene);
        rs.Views().BeginFrame();
    }

    const TerrainECS::TerrainRenderedViews rendered = TerrainECS::TerrainRenderedViews::Collect(rs.Views());
    ASSERT_EQ(rendered.CameraPositions.size(), 1u) << "the parked view and the view off the pipeline request no page";
    EXPECT_NEAR(rendered.CameraPositions[0].y, 1000.0f, 1e-3f);
    EXPECT_EQ(rendered.PageCameras.size(), 1u);
}

// A game with two views the pipeline renders (a split screen) requests terrain pages for both.
TEST(ViewRegistryTests, TwoAlwaysViewsAreBothRendered)
{
    RenderServices rs;
    const ViewId left = rs.Views().AllocateView("left", rs.Views().AllocateCamera("left"), ViewPurpose::Game);
    const ViewId right = rs.Views().AllocateView("right", rs.Views().AllocateCamera("right"), ViewPurpose::Game);
    rs.Views().BeginFrame();
    rs.Views().BeginFrame();

    EXPECT_TRUE(rs.Views().FindViewDesc(left)->RenderedByPipeline());
    EXPECT_TRUE(rs.Views().FindViewDesc(right)->RenderedByPipeline());
}

// The derived near and culling range are the ranges the owner set, not values read from the
// projection matrix: a matrix built for 0.5..100 m with ranges 0.25..300 m derives 0.25 and 300.
TEST(ViewRegistryTests, TheDerivedCameraTakesTheCarriedRangesNotTheMatrix)
{
    RenderServices rs;
    const CameraId camera = rs.Views().AllocateCamera("camera");
    const ViewId view = rs.Views().AllocateView("view", camera, ViewPurpose::Game);
    CameraData data{};
    data.proj = Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0f, 1.5f, 0.5f, 100.0f);
    data.viewProj = data.proj;
    rs.Views().SetCameraData(camera, ::GameEngine::Tests::TestCameraWithEye(data), {0.25f, 300.0f});

    const CameraRanges ranges = rs.Views().ResolveCameraRanges(view);
    EXPECT_EQ(ranges.Near, 0.25f);
    EXPECT_EQ(ranges.CullingRange, 300.0f);
    const CameraDerivedData derived = DeriveCameraData(*rs.Views().FindCameraData(camera), ranges);
    EXPECT_EQ(derived.NearPlane, 0.25f);
    EXPECT_EQ(derived.FarPlane, 300.0f);

    ASSERT_TRUE(rs.Views().ReleaseView(view));
    ASSERT_TRUE(rs.Views().ReleaseCamera(camera));
    EXPECT_EQ(rs.Views().FindCameraRanges(camera).CullingRange, 0.0f);
}

// A camera 125 km from the origin that moves 0.1 m/s (1.67 mm per frame at 60 Hz) moves on every
// frame: the eye goes from the entity's double position to the rebased view without a float step.
TEST(ViewRegistryTests, ACameraAt125KmMovingATenthOfAMeterPerSecondMovesOnEveryFrame)
{
    RenderServices rs;
    const CameraId camera = rs.Views().AllocateCamera("camera");
    const ViewId view = rs.Views().AllocateView("view", camera, ViewPurpose::Game);
    constexpr double kStartX = 125000.0;
    constexpr double kStepMeters = 0.1 / 60.0;
    float32 previousTranslationX = 0.0f;
    for (int32 frame = 0; frame <= 10; ++frame)
    {
        const Mathematics::WorldPosition eye{kStartX + kStepMeters * frame, 2.0, 0.0};
        const Components::Transform pose =
            Components::Transform::FromTRS(eye, Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f});
        Camera active{};
        active.SetPose(pose.ToMatrix4x4(Mathematics::Vector3{}), eye);
        rs.Views().SetCameraData(camera, active.ToCameraData(1.0f), active.ToCameraRanges());
        const float32 translationX = rs.Views().ResolveCameraData(view).viewRel[3][0];
        if (frame > 0)
        {
            EXPECT_NE(translationX, previousTranslationX) << "frame " << frame;
        }
        previousTranslationX = translationX;
    }
}
