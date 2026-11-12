// Sorted transparent CPU frustum cull (CullTransparentsToFrustum) with mock
// spheres: in/out/straddle/behind, the "culled don't reach the drain"
// invariant, the empty (zero-cost) path, and the sector composition that keeps
// a far transparent culling as it does near the origin. The back-to-front order
// runs on the GPU: SortedTransparentRunTests and GPUSortTests cover it.

#include <gtest/gtest.h>

#include "Engine/Rendering/SortedTransparentCull.h"

#include "Mathematics/MatrixOps.h"
#include "Rendering/Camera/CameraDerivation.h" // DeriveCameraData (Forward convention pin)
#include "Rendering/Common/Frustum.h" // ExtractFrustumPlanes
#include "Rendering/Core/GPUScene.h"    // PackSector, SectorLocalPointRelativeTo

#include <cmath>
#include <cstring>
#include <memory_resource>
#include <vector>

using namespace GameEngine::Engine::Renderer;

namespace Math = GameEngine::Mathematics;
namespace Rndr = GameEngine::Rendering;

namespace {
TransparentSphere MakeSphere(uint32_t key, float x, float y, float z, float radius)
{
    return TransparentSphere{key, Rndr::Vector3(x, y, z), radius};
}

// Camera at `eye` looking down +Z (left-handed), 90 deg vertical FOV, square
// aspect — so the left/right/top/bottom planes sit at 45 deg: a point (x,y,z)
// in front of the camera is inside when |x-eye.x| < z-eye.z and similarly for
// y. Reverse-Z near 0.1 / far 100 matches the engine's projection convention,
// so ExtractFrustumPlanes behaves exactly as it does in production.
void MakeForwardFrustum(const Math::Vector3& eye, Rndr::Vector4* outPlanes)
{
    constexpr float kFovY90 = 1.5707963267948966f; // pi/2
    const Math::Matrix4x4 view = Math::MakeLookAtLH(
        eye, eye + Math::Vector3(0.0f, 0.0f, 1.0f), Math::Vector3(0.0f, 1.0f, 0.0f));
    const Math::Matrix4x4 proj = Math::MakePerspectiveLH_ZO_ReverseZ(kFovY90, 1.0f, 0.1f, 100.0f);
    const Math::Matrix4x4 viewProj = proj * view;
    Rndr::ExtractFrustumPlanes(viewProj, outPlanes);
}
} // namespace

TEST(SortedTransparentCull, KeepsInside_DropsOutsideAndBehind)
{
    Rndr::Vector4 planes[6];
    MakeForwardFrustum(Math::Vector3(0.0f, 0.0f, 0.0f), planes);

    // Camera at origin looks down +Z; the 45 deg frustum keeps |x|,|y| < z.
    const std::vector<TransparentSphere> candidates = {
        MakeSphere(0, 0.0f, 0.0f, 10.0f, 1.0f),    // dead centre, in front -> visible
        MakeSphere(1, 1000.0f, 0.0f, 10.0f, 1.0f), // far to the right -> culled
        MakeSphere(2, 0.0f, 1000.0f, 10.0f, 1.0f), // far above -> culled
        MakeSphere(3, 0.0f, 0.0f, -10.0f, 1.0f),   // behind the camera -> culled
    };

    std::pmr::vector<TransparentCentroid> visible;
    const uint32_t culled = CullTransparentsToFrustum(candidates, planes, visible);

    EXPECT_EQ(culled, 3u);
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible[0].Key, 0u); // only the centred sphere survives
    // Survivors keep their center (the depth key reads it).
    EXPECT_FLOAT_EQ(visible[0].Center.z, 10.0f);
}

TEST(SortedTransparentCull, EdgeStraddlingSphereIsKept)
{
    Rndr::Vector4 planes[6];
    MakeForwardFrustum(Math::Vector3(0.0f, 0.0f, 0.0f), planes);

    // Centre just OUTSIDE the right plane (x=10.5 vs the 45 deg boundary x=z=10 at
    // z=10), radius 1. Its bounding sphere straddles the plane, so the conservative
    // test (1.5x built-in inflation) must KEEP it — a false reject here would pop a
    // transparent that is still partially on screen.
    const std::vector<TransparentSphere> straddle = {MakeSphere(7, 10.5f, 0.0f, 10.0f, 1.0f)};
    // A sphere fully past the plane by many radii must be culled (sanity contrast).
    const std::vector<TransparentSphere> fullyOut = {MakeSphere(8, 20.0f, 0.0f, 10.0f, 1.0f)};

    std::pmr::vector<TransparentCentroid> keep;
    EXPECT_EQ(CullTransparentsToFrustum(straddle, planes, keep), 0u);
    ASSERT_EQ(keep.size(), 1u);
    EXPECT_EQ(keep[0].Key, 7u);

    std::pmr::vector<TransparentCentroid> drop;
    EXPECT_EQ(CullTransparentsToFrustum(fullyOut, planes, drop), 1u);
    EXPECT_TRUE(drop.empty());
}

// The drain sorts VISIBLE transparents only: a scene whose total Blend count is
// huge must stage only what the frustum keeps (this is also what keeps the
// GPU-sort capacity a per-VIEW bound, not a per-scene one).
TEST(SortedTransparentCull, CulledInstancesNeverReachTheDrain)
{
    Rndr::Vector4 planes[6];
    MakeForwardFrustum(Math::Vector3(0.0f, 0.0f, 0.0f), planes);

    std::vector<TransparentSphere> candidates;
    // 3 visible (in front, centred).
    for (uint32_t i = 0; i < 3; ++i)
        candidates.push_back(MakeSphere(i, 0.0f, 0.0f, 5.0f + static_cast<float>(i), 1.0f));
    // 20 behind the camera.
    for (uint32_t i = 0; i < 20; ++i)
        candidates.push_back(MakeSphere(100u + i, 0.0f, 0.0f, -50.0f, 1.0f));

    std::pmr::vector<TransparentCentroid> visible;
    const uint32_t culled = CullTransparentsToFrustum(candidates, planes, visible);
    EXPECT_EQ(culled, 20u);
    ASSERT_EQ(visible.size(), 3u);
}

// All candidates off-screen -> nothing visible -> the view stays inactive and
// the off-screen blends ride the GPU-culled batched path.
TEST(SortedTransparentCull, AllCulledYieldsEmpty)
{
    Rndr::Vector4 planes[6];
    MakeForwardFrustum(Math::Vector3(0.0f, 0.0f, 0.0f), planes);

    std::vector<TransparentSphere> candidates;
    for (uint32_t i = 0; i < 5; ++i)
        candidates.push_back(MakeSphere(i, 0.0f, 0.0f, -20.0f, 1.0f)); // all behind

    std::pmr::vector<TransparentCentroid> visible;
    EXPECT_EQ(CullTransparentsToFrustum(candidates, planes, visible), 5u);
    EXPECT_TRUE(visible.empty());
}

// Sector composition: candidates in a far sector, composed against the camera's
// sector, cull with the planes of the relative view exactly as the same scene does
// at the origin.
TEST(SortedTransparentCull, FarSectorCompositionMatchesNearOriginDecisions)
{
    Rndr::Vector4 planes[6];
    MakeForwardFrustum(Math::Vector3(0.0f, 0.0f, 0.0f), planes);
    const std::vector<TransparentSphere> refCandidates = {
        MakeSphere(0, 0.0f, 0.0f, 10.0f, 1.0f),  // visible
        MakeSphere(1, 30.0f, 0.0f, 10.0f, 1.0f), // outside the 45 deg cone
        MakeSphere(2, 0.0f, 0.0f, -5.0f, 1.0f),  // behind
        MakeSphere(3, 9.0f, 0.0f, 10.0f, 1.0f),  // near the edge, inside
    };
    std::pmr::vector<TransparentCentroid> refVisible;
    const uint32_t refCulled = CullTransparentsToFrustum(refCandidates, planes, refVisible);

    // The camera in a sector about 300 km out; the same centers stored local to the next
    // sector in +X, so the composition crosses one sector boundary.
    const Math::Vector3Int cameraSector{293, 0, -4};
    uint32_t p0 = 0, p1 = 0;
    Rndr::PackSector(cameraSector.x + 1, cameraSector.y, cameraSector.z, p0, p1);
    const float sectorSize = static_cast<float>(Math::kWorldSectorSize);
    std::vector<TransparentSphere> farCandidates = refCandidates;
    for (TransparentSphere& c : farCandidates)
    {
        const Rndr::Vector3 local(c.Center.x - sectorSize, c.Center.y, c.Center.z);
        c.Center = Rndr::SectorLocalPointRelativeTo(local, p0, p1, cameraSector);
    }
    std::pmr::vector<TransparentCentroid> farVisible;
    const uint32_t farCulled = CullTransparentsToFrustum(farCandidates, planes, farVisible);

    EXPECT_EQ(farCulled, refCulled);
    ASSERT_EQ(farVisible.size(), refVisible.size());
    for (size_t i = 0; i < refVisible.size(); ++i)
        EXPECT_EQ(farVisible[i].Key, refVisible[i].Key) << "survivor set diverged at " << i;
}

// The drain's depth key is dot(centre - camPos, CameraDerivedData::Forward) —
// its back-to-front order depends on Forward pointing INTO the view. This
// engine is left-handed +Z-forward; DeriveCameraData shipped with the
// right-handed -Z convention for a while, which negated every view depth and
// inverted the blend sort front-to-back (caught by the per-run depth-range
// log at the ER overview). Pin the convention.
TEST(SortedTransparentCull, DerivedCameraForwardPointsIntoTheView)
{
    const Math::Vector3 eye(3.0f, 2.0f, -8.0f);
    const Math::Vector3 target(-1.0f, 0.5f, 6.0f);
    const Math::Matrix4x4 view = Math::MakeLookAtLH(eye, target, Math::Vector3(0.0f, 1.0f, 0.0f));
    const Math::Matrix4x4 proj =
        Math::MakePerspectiveLH_ZO_ReverseZ(1.0472f, 1.7778f, 0.1f, 500.0f);
    const Math::Matrix4x4 viewProj = proj * view;

    Rndr::CameraData cam{};
    cam.view = view;
    cam.proj = proj;
    cam.viewProj = viewProj;

    const Rndr::CameraDerivedData derived = Rndr::DeriveCameraData(cam, {0.1f, 500.0f});

    // Position round-trips.
    EXPECT_NEAR(derived.Position.x, eye.x, 1e-4f);
    EXPECT_NEAR(derived.Position.y, eye.y, 1e-4f);
    EXPECT_NEAR(derived.Position.z, eye.z, 1e-4f);

    // A point dead ahead has POSITIVE view depth (larger = farther).
    const Math::Vector3 toTarget = target - eye;
    const float viewDepth = toTarget.x * derived.Forward.x + toTarget.y * derived.Forward.y +
                            toTarget.z * derived.Forward.z;
    EXPECT_GT(viewDepth, 0.0f);
    // And Forward is the normalized look direction, not merely in front.
    const float len = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y +
                                toTarget.z * toTarget.z);
    EXPECT_NEAR(viewDepth, len, 1e-3f);
}
