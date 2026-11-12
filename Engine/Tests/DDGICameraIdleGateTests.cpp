// The camera-motion signal DDGI's idle-gated solve mode holds on.
//
// The two failure modes worth pinning are both invisible in a screenshot:
//
//   * Counting a view a human never looks through. A reflection-probe capture
//     or thumbnail bake re-points its camera every frame by design, so if those
//     purposes are eligible the gate reports "moving" forever and an idle-gated
//     field never solves at all — which reads as "DDGI is broken", not as "the
//     gate is too eager".
//
//   * Getting the rest debounce backwards. Motion must arm the gate on the tick
//     it is first seen (a debounce on that side would let a solve land mid-orbit),
//     while release waits out the quiet window.

#include <gtest/gtest.h>

#include "Engine/Rendering/DDGICameraIdleGate.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "TestCameraEye.h"

#include <cmath>

using GameEngine::Engine::Renderer::DDGICameraIdleGate;
using GameEngine::Engine::Renderer::ViewRegistry;
using GameEngine::Rendering::CameraData;
using GameEngine::Rendering::CameraId;
using GameEngine::Rendering::ViewPurpose;

namespace
{

// The poses carry no projection, so the cameras carry no ranges.
constexpr GameEngine::Rendering::CameraRanges kNoRanges{};

// Identity view matrix at a given world position. The gate reads cameraPos for
// translation and the view matrix's 3x3 basis for orientation.
CameraData PoseAt(float x, float y, float z)
{
    CameraData data{};
    data.view = GameEngine::Mathematics::Matrix4x4::Identity();
    data.cameraPos.x = x;
    data.cameraPos.y = y;
    data.cameraPos.z = z;
    return data;
}

// A view matrix rotated by `radians` about Y, so a test can move orientation
// without moving position.
CameraData PoseYawed(float radians)
{
    CameraData data = PoseAt(0.0f, 0.0f, 0.0f);
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    data.view[0][0] = c;
    data.view[0][2] = -s;
    data.view[2][0] = s;
    data.view[2][2] = c;
    return data;
}

// Longer than the gate's own rest window, so one call of this settles it.
constexpr float kWellPastDebounceSeconds = 1.0f;

struct GateFixture
{
    ViewRegistry Registry;
    DDGICameraIdleGate Gate;
    CameraId Camera = 0;

    explicit GateFixture(ViewPurpose purpose = ViewPurpose::EditorScene)
    {
        Camera = Registry.AllocateCamera("Test.Camera");
        Registry.AllocateView("Test.View", Camera, purpose);
        Registry.SetCameraData(Camera, ::GameEngine::Tests::TestCameraWithEye(PoseAt(0.0f, 0.0f, 0.0f)), kNoRanges);
    }

    bool Tick(float deltaSeconds) { return Gate.UpdateAndIsMoving(Registry, /*worldId*/ 0, deltaSeconds); }
    void Settle() { Tick(kWellPastDebounceSeconds); }
};

}  // namespace

TEST(DDGICameraIdleGateTests, ReportsStillOnceTheRestWindowElapses)
{
    GateFixture f;
    // First tick is a baseline sighting, not motion — but the rest window still
    // has to elapse before the gate releases.
    EXPECT_TRUE(f.Tick(0.016f));
    EXPECT_FALSE(f.Tick(kWellPastDebounceSeconds));
}

TEST(DDGICameraIdleGateTests, TranslationArmsTheGateOnTheSameTick)
{
    GateFixture f;
    f.Settle();
    f.Registry.SetCameraData(f.Camera, ::GameEngine::Tests::TestCameraWithEye(PoseAt(0.0f, 0.0f, 5.0f)), kNoRanges);
    EXPECT_TRUE(f.Tick(0.016f));
}

TEST(DDGICameraIdleGateTests, RotationAloneArmsTheGate)
{
    GateFixture f;
    f.Settle();
    // Well past the ~6e-4 rad the reference's quaternion-dot threshold admits.
    f.Registry.SetCameraData(f.Camera, ::GameEngine::Tests::TestCameraWithEye(PoseYawed(0.05f)), kNoRanges);
    EXPECT_TRUE(f.Tick(0.016f));
}

TEST(DDGICameraIdleGateTests, SubEpsilonDriftIsNotMotion)
{
    GateFixture f;
    f.Settle();
    // 1e-5 units of translation: below the reference's 1e-7 squared-distance
    // threshold, so float noise in a static pose must not hold the solve.
    f.Registry.SetCameraData(f.Camera, ::GameEngine::Tests::TestCameraWithEye(PoseAt(1.0e-5f, 0.0f, 0.0f)), kNoRanges);
    EXPECT_FALSE(f.Tick(0.016f));
}

TEST(DDGICameraIdleGateTests, ReleaseWaitsOutTheQuietWindowButResumesAfterIt)
{
    GateFixture f;
    f.Settle();
    f.Registry.SetCameraData(f.Camera, ::GameEngine::Tests::TestCameraWithEye(PoseAt(0.0f, 0.0f, 5.0f)), kNoRanges);
    EXPECT_TRUE(f.Tick(0.016f));
    // Still: the pose stops changing, but the debounce has not elapsed.
    EXPECT_TRUE(f.Tick(0.016f));
    EXPECT_FALSE(f.Tick(kWellPastDebounceSeconds));
}

TEST(DDGICameraIdleGateTests, CaptureAndPreviewViewsAreIgnored)
{
    for (const ViewPurpose purpose : {ViewPurpose::UtilityCapture, ViewPurpose::EditorPreview})
    {
        GateFixture f(purpose);
        f.Settle();
        // A capture view re-points its camera every frame; counting it would
        // wedge the gate at "moving" and the field would never solve.
        f.Registry.SetCameraData(f.Camera, ::GameEngine::Tests::TestCameraWithEye(PoseAt(0.0f, 0.0f, 50.0f)), kNoRanges);
        EXPECT_FALSE(f.Tick(0.016f)) << "purpose " << static_cast<int>(purpose);
    }
}

TEST(DDGICameraIdleGateTests, AnyEligibleViewMovingHoldsTheField)
{
    GateFixture f;  // EditorScene view
    const CameraId gameCamera = f.Registry.AllocateCamera("Test.GameCamera");
    f.Registry.AllocateView("Test.GameView", gameCamera, ViewPurpose::Game);
    f.Registry.SetCameraData(gameCamera, ::GameEngine::Tests::TestCameraWithEye(PoseAt(0.0f, 0.0f, 0.0f)), kNoRanges);
    f.Settle();

    // The scene view is still; the game view is not. The field is world-space
    // and shared by both, so either one moving holds it.
    f.Registry.SetCameraData(gameCamera, ::GameEngine::Tests::TestCameraWithEye(PoseAt(0.0f, 0.0f, 5.0f)), kNoRanges);
    EXPECT_TRUE(f.Tick(0.016f));
}

TEST(DDGICameraIdleGateTests, VolumeWorldScopesWhichViewsCount)
{
    ViewRegistry registry;
    DDGICameraIdleGate gate;
    const CameraId camera = registry.AllocateCamera("Test.OtherWorldCamera");
    const auto viewId = registry.AllocateView("Test.OtherWorldView", camera, ViewPurpose::EditorScene);
    registry.SetCameraData(camera, ::GameEngine::Tests::TestCameraWithEye(PoseAt(0.0f, 0.0f, 0.0f)), kNoRanges);
    constexpr GameEngine::uint64 kOtherWorld = 7;
    registry.SetViewWorldId(viewId, kOtherWorld);

    constexpr GameEngine::uint64 kVolumeWorld = 42;
    gate.UpdateAndIsMoving(registry, kVolumeWorld, kWellPastDebounceSeconds);
    registry.SetCameraData(camera, ::GameEngine::Tests::TestCameraWithEye(PoseAt(0.0f, 0.0f, 5.0f)), kNoRanges);
    EXPECT_FALSE(gate.UpdateAndIsMoving(registry, kVolumeWorld, 0.016f));
}
