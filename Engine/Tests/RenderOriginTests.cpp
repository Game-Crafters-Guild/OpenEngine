// Oracles for camera-relative rendering (Earth-scale precision). These lock the
// falsifiable claims of the render-origin design:
//   * sector pack/unpack round-trips and (0,0,0) packs to zero (byte-identical);
//   * the rebased view is bit-identical to the full-world view when the origin
//     is sector (0,0,0);
//   * world -> (sector,local) -> reconstruct is bit-exact at small coordinates;
//   * the rebased projection of a relative position equals the full-world
//     projection of the world position (mathematical equivalence);
//   * at a planetary offset the reconstructed vertex is sub-pixel stable across
//     camera motion, where the fp32-world baseline swims by many pixels.
//
// The C++ UnpackSector here mirrors the GLSL ge_UnpackInstanceSector decode; a
// drift between the two is caught by the round-trip + fixed-bit-pattern cases.

#include "Engine/Rendering/RenderOrigin.h"
#include "ExtractionHarness.h"

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Common/Frustum.h"     // ExtractFrustumPlanes
#include "Rendering/Core/GPUScene.h"      // SectorLocalPointRelativeTo
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/PostProcessEffects/VolumetricFogEffect.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "ECSModules/Rendering/Systems/ReflectionProbeSystem.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Mathematics;
namespace Origin = GameEngine::Engine::Renderer;

// The sector size in the 32-bit arithmetic the shader and the composition use.
constexpr float32 kSectorSize32 = static_cast<float32>(kWorldSectorSize);

namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;

// Build a camera world transform: a fixed precise orientation plus a (possibly
// huge) world translation — exactly the shape Camera::ComputeViewMatrix inverts.
Matrix4x4 MakeCameraWorld(const Matrix4x4& rotation, const Vector3& pos)
{
    Matrix4x4 m = rotation;
    m.Data()[12] = pos.x;
    m.Data()[13] = pos.y;
    m.Data()[14] = pos.z;
    return m;
}

// The LEGACY fp32 column rebase (out = M * translate(origin), fp32 big+big
// sum) — removed from production by #660 (camera) and its shadow follow-up,
// kept here verbatim as the fails-before contrast oracle.
void LegacyRebaseTranslationColumnFp32(const float* m, float ox, float oy, float oz, float* out)
{
    out[12] = m[0] * ox + m[4] * oy + m[8] * oz + m[12];
    out[13] = m[1] * ox + m[5] * oy + m[9] * oz + m[13];
    out[14] = m[2] * ox + m[6] * oy + m[10] * oz + m[14];
    out[15] = m[3] * ox + m[7] * oy + m[11] * oz + m[15];
}
} // namespace

// ---- Risk 1: the packed sector round-trips and (0,0,0) stays byte-zero -------

TEST(RenderOrigin, SectorPackRoundTrip)
{
    const int32 samples[] = {0,   1,   -1,   2,    -2,   1000, -1000, 65535, -65535,
                             Rendering::kSectorAxisMax, Rendering::kSectorAxisMin, 6221, -6221};
    for (int32 x : samples)
        for (int32 y : samples)
            for (int32 z : samples)
            {
                uint32 p0 = 0, p1 = 0;
                Rendering::PackSector(x, y, z, p0, p1);
                int32 rx = 0, ry = 0, rz = 0;
                Rendering::UnpackSector(p0, p1, rx, ry, rz);
                EXPECT_EQ(rx, x) << "x=" << x << " y=" << y << " z=" << z;
                EXPECT_EQ(ry, y) << "x=" << x << " y=" << y << " z=" << z;
                EXPECT_EQ(rz, z) << "x=" << x << " y=" << y << " z=" << z;
            }
}

TEST(RenderOrigin, SectorZeroPacksToZero)
{
    // Untagged instances (sector 0) must leave the two reserved GPUInstance words
    // zero, so the 240 B memcmp still treats them as identity (quiescence) and the
    // bytes match the pre-feature layout.
    uint32 p0 = 0xDEADBEEFu, p1 = 0xDEADBEEFu;
    Rendering::PackSector(0, 0, 0, p0, p1);
    EXPECT_EQ(p0, 0u);
    EXPECT_EQ(p1, 0u);
}

TEST(RenderOrigin, SectorPackFixedBitLayout)
{
    // Lock the exact bit layout the GLSL decode depends on. p0 = [X:21][Ylo:11],
    // p1 = [Yhi:10][Z:21]. A change here that isn't mirrored in the shader is a
    // silent corruption.
    uint32 p0 = 0, p1 = 0;
    Rendering::PackSector(1, 0, 0, p0, p1); // X=1 -> lowest bit of p0
    EXPECT_EQ(p0, 1u);
    EXPECT_EQ(p1, 0u);

    Rendering::PackSector(0, 1, 0, p0, p1); // Y=1 -> bit 21 of p0
    EXPECT_EQ(p0, 1u << 21);
    EXPECT_EQ(p1, 0u);

    Rendering::PackSector(0, 0, 1, p0, p1); // Z=1 -> bit 10 of p1
    EXPECT_EQ(p0, 0u);
    EXPECT_EQ(p1, 1u << 10);
}

// ---- The origin is the eye's sector; rebased == full-world in sector (0,0,0) ----

TEST(RenderOrigin, TheOriginIsTheEyesSector)
{
    // The origin is the WorldPosition split of the eye at any distance, clamped to the packed range.
    const auto nearOrigin = Origin::ComputeRenderOriginSector(WorldPosition{511.9, -512.0, 0.0});
    EXPECT_EQ(nearOrigin.x, 0);
    EXPECT_EQ(nearOrigin.y, 0);
    EXPECT_EQ(nearOrigin.z, 0);
    const WorldPosition far{125000.0, 512.0, -1536.0};
    Vector3Int split;
    Vector3 local;
    far.ToSector(split, local);
    const auto s = Origin::ComputeRenderOriginSector(far);
    EXPECT_EQ(s.x, split.x);
    EXPECT_EQ(s.y, split.y);
    EXPECT_EQ(s.z, split.z);
    EXPECT_EQ(s.y, 1);
    const auto clamped = Origin::ComputeRenderOriginSector(WorldPosition{1.0e12, -1.0e12, 0.0});
    EXPECT_EQ(clamped.x, Rendering::kSectorAxisMax);
    EXPECT_EQ(clamped.y, Rendering::kSectorAxisMin);
}

TEST(RenderOrigin, CameraEyeIsTheDoubleEyeOfARebasedCamera)
{
    // At 125 km the fp32 step is 7.8 mm: the eye's 3.7 mm fraction is lost in cameraPos and kept here.
    const WorldPosition eye{125000.0037, 31.25, -2048.5};
    const Vector3 eyeF(static_cast<float>(eye.X), static_cast<float>(eye.Y), static_cast<float>(eye.Z));
    const Matrix4x4 rot = MakeRotationY(0.7f) * MakeRotationX(-0.2f);
    Rendering::CameraData cam{};
    cam.view = Inverse(MakeCameraWorld(rot, eyeF));
    cam.proj = MakePerspectiveLH_ZO_ReverseZ(60.0f * kDeg2Rad, 16.0f / 9.0f, 0.1f, 1.0e7f);
    cam.viewProj = cam.proj * cam.view;
    Origin::SetCameraEye(cam, eye);
    const WorldPosition farEye = Origin::CameraEye(cam);
    EXPECT_NEAR(farEye.X, eye.X, 1.0e-4);
    EXPECT_NEAR(farEye.Y, eye.Y, 1.0e-4);
    EXPECT_NEAR(farEye.Z, eye.Z, 1.0e-4);

    // In sector (0,0,0) the view is the fp32 view, so the eye is cameraPos bit for bit.
    Origin::SetCameraEye(cam, WorldPosition{300.0001, 2.0, -40.0});
    const WorldPosition nearEye = Origin::CameraEye(cam);
    EXPECT_EQ(nearEye.X, static_cast<double>(cam.cameraPos.x));
    EXPECT_EQ(nearEye.Y, static_cast<double>(cam.cameraPos.y));
    EXPECT_EQ(nearEye.Z, static_cast<double>(cam.cameraPos.z));
}

TEST(RenderOrigin, RebasedViewIsBitIdenticalWhenInactive)
{
    // Camera inside sector (0,0,0): the rebased matrices must be a
    // bit-for-bit copy of the full-world ones (byte-identical render output).
    const Vector3 camPos(300.0f, 200.0f, -400.0f); // inside sector (0,0,0)
    const Matrix4x4 rot = MakeRotationY(0.7f) * MakeRotationX(-0.2f);
    const Matrix4x4 view = Inverse(MakeCameraWorld(rot, camPos));
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(60.0f * kDeg2Rad, 16.0f / 9.0f, 0.1f, 1.0e7f);
    const Matrix4x4 viewProj = proj * view;

    Rendering::CameraData cam{};
    cam.view = view;
    cam.proj = proj;
    cam.viewProj = viewProj;
    cam.cameraPos.x = camPos.x;
    cam.cameraPos.y = camPos.y;
    cam.cameraPos.z = camPos.z;

    Origin::ComputeRebasedView(cam.view, cam.proj, cam.viewProj, WorldPosition::FromVector3(camPos), cam.viewRel,
                               cam.viewProjRel, cam.renderOriginSector);

    EXPECT_EQ(cam.renderOriginSector[0], 0);
    EXPECT_EQ(cam.renderOriginSector[1], 0);
    EXPECT_EQ(cam.renderOriginSector[2], 0);
    EXPECT_EQ(0, std::memcmp(cam.viewRel.Data(), cam.view.Data(), sizeof(cam.view)));
    EXPECT_EQ(0, std::memcmp(cam.viewProjRel.Data(), cam.viewProj.Data(), sizeof(cam.viewProj)));
}

// ---- Reconstruction round-trip: world -> (sector,local) -> world -------------

TEST(RenderOrigin, ReconstructionIsBitExactForSmallLocal)
{
    // A sector-tagged entity stores a small local remainder; reconstructing
    // absolute world = sector*size + local must be exact, and the reconstructed
    // full world (as the shader computes it) must equal the sector-local + origin
    // path bit-for-bit for small local coordinates.
    const auto originSector = Origin::ComputeRenderOriginSector(WorldPosition{6.371e6, 0.0, 0.0});
    ASSERT_NE(originSector.x, 0); // origin active at Earth radius

    // Entity 12.5 m from the render origin, tagged to the origin's sector.
    const Vector3 local(12.5f, -3.25f, 7.0f);
    const Mathematics::Vector3Int instSector = originSector;

    // Shader math: relWorldPos = (instSector - originSector)*size + local.
    const float s = kSectorSize32;
    const Vector3 rel((instSector.x - originSector.x) * s + local.x,
                      (instSector.y - originSector.y) * s + local.y,
                      (instSector.z - originSector.z) * s + local.z);
    EXPECT_FLOAT_EQ(rel.x, local.x); // same sector -> rel == local, exact
    EXPECT_FLOAT_EQ(rel.y, local.y);
    EXPECT_FLOAT_EQ(rel.z, local.z);

    // Reconstruct full world = rel + origin.
    const Vector3 origin = Origin::SectorToWorld(originSector);
    const Vector3 full = rel + origin;
    // Direct absolute world = instSector*size + local.
    const Vector3 direct(instSector.x * s + local.x, instSector.y * s + local.y, instSector.z * s + local.z);
    EXPECT_FLOAT_EQ(full.x, direct.x);
    EXPECT_FLOAT_EQ(full.y, direct.y);
    EXPECT_FLOAT_EQ(full.z, direct.z);
}

TEST(RenderOrigin, RebasedProjectionMatchesFullWorldWhenClean)
{
    // Correctness (not precision): at 34 km fp32 is still clean, so
    // viewProjRel*relPos and viewProj*worldPos must agree to well under a pixel:
    // the rebased transform is the same transform. (Far out they diverge by
    // design: the rebased path is the more accurate one, see the far-field oracle.)
    const Vector3 camPos(34000.0f, 800.0f, -12000.0f); // ~34 km, origin active
    const Matrix4x4 rot = MakeRotationY(-0.5f) * MakeRotationX(0.15f);
    const Matrix4x4 view = Inverse(MakeCameraWorld(rot, camPos));
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(55.0f * kDeg2Rad, 1.5f, 0.1f, 1.0e6f);
    const Matrix4x4 viewProj = proj * view;

    Rendering::CameraData cam{};
    cam.view = view;
    cam.proj = proj;
    cam.viewProj = viewProj;
    cam.cameraPos.x = camPos.x;
    cam.cameraPos.y = camPos.y;
    cam.cameraPos.z = camPos.z;
    Origin::ComputeRebasedView(cam.view, cam.proj, cam.viewProj, WorldPosition::FromVector3(camPos), cam.viewRel,
                               cam.viewProjRel, cam.renderOriginSector);
    ASSERT_NE(cam.renderOriginSector[0] | cam.renderOriginSector[1] | cam.renderOriginSector[2], 0);

    Matrix4x4 vpRel = cam.viewProjRel;

    const Vector3 origin = Origin::SectorToWorld(Origin::CameraRenderOriginSector(cam));

    const Vector3 worldPos(34050.0f, 780.0f, -11985.0f); // ~55 m from the camera
    const Vector3 relPos = worldPos - origin;

    const Vector4 full = viewProj.Transform(Vector4(worldPos.x, worldPos.y, worldPos.z, 1.0f));
    const Vector4 rel = vpRel.Transform(Vector4(relPos.x, relPos.y, relPos.z, 1.0f));

    EXPECT_NEAR(rel.x / rel.w, full.x / full.w, 2e-3f); // < ~2 px on a 1080p frame
    EXPECT_NEAR(rel.y / rel.w, full.y / full.w, 2e-3f);
    EXPECT_NEAR(rel.z / rel.w, full.z / full.w, 2e-3f);
}

// ---- Rotation-smooth rebased translation (the planet-scale camera-jitter fix) -

TEST(RenderOrigin, RebasedTranslationSmoothUnderRotationAtRange)
{
    // The old origin-active rebase computed viewRel's translation as the fp32
    // sum (stored translation + M3x3*origin) — two ~|eye|-magnitude terms whose
    // small result inherits ULP(|eye|) rounding, and the rounding CHANGES with
    // the rotation: ~4-8 mm wobble at |eye| 5e4, ~0.5-1 m at Earth radius,
    // visible as world shimmer during interactive camera rotation.
    // ComputeRebasedView now rebuilds the column as -M3x3*(eye - origin) in
    // double. Assert it tracks the exact double reference tightly through a
    // rotation sweep — and that the legacy fp32 column rebase measurably does
    // not at this magnitude.
    const Vector3 camPos(3.681e6f, 500.0f, 3.681e6f);

    double maxNewErr = 0.0;
    double maxOldErr = 0.0;
    for (int i = 0; i <= 60; ++i)
    {
        const float yaw = 0.6f + 1.0e-3f * static_cast<float>(i); // slow rotation sweep
        const Matrix4x4 rot = MakeRotationY(yaw) * MakeRotationX(-0.15f);
        const Matrix4x4 view = Inverse(MakeCameraWorld(rot, camPos));
        const Matrix4x4 proj =
            MakePerspectiveLH_ZO_ReverseZ(55.0f * kDeg2Rad, 16.0f / 9.0f, 0.5f, 4000.0f);
        const Matrix4x4 viewProj = proj * view;

        Rendering::CameraData cam{};
        cam.view = view;
        cam.proj = proj;
        cam.viewProj = viewProj;
        cam.cameraPos.x = camPos.x;
        cam.cameraPos.y = camPos.y;
        cam.cameraPos.z = camPos.z;
        Origin::ComputeRebasedView(cam.view, cam.proj, cam.viewProj, WorldPosition::FromVector3(camPos),
                                   cam.viewRel, cam.viewProjRel, cam.renderOriginSector);
        ASSERT_NE(cam.renderOriginSector[0] | cam.renderOriginSector[1] | cam.renderOriginSector[2], 0);

        // Exact double reference: -M3x3 * (eye - origin), origin from the
        // integer sector (exact in double).
        const double size = kWorldSectorSize;
        const double lx = static_cast<double>(camPos.x) - cam.renderOriginSector[0] * size;
        const double ly = static_cast<double>(camPos.y) - cam.renderOriginSector[1] * size;
        const double lz = static_cast<double>(camPos.z) - cam.renderOriginSector[2] * size;
        const float* v = view.Data();
        const double ref[3] = {
            -(static_cast<double>(v[0]) * lx + static_cast<double>(v[4]) * ly + static_cast<double>(v[8]) * lz),
            -(static_cast<double>(v[1]) * lx + static_cast<double>(v[5]) * ly + static_cast<double>(v[9]) * lz),
            -(static_cast<double>(v[2]) * lx + static_cast<double>(v[6]) * ly + static_cast<double>(v[10]) * lz)};

        for (int c = 0; c < 3; ++c)
            maxNewErr = std::max(maxNewErr, std::fabs(static_cast<double>(cam.viewRel[3][c]) - ref[c]));

        // Legacy fp32 column rebase for the fails-before contrast.
        const Vector3 origin = Origin::SectorToWorld(Origin::CameraRenderOriginSector(cam));
        float oldRel[16];
        std::memcpy(oldRel, view.Data(), sizeof(oldRel));
        LegacyRebaseTranslationColumnFp32(view.Data(), origin.x, origin.y, origin.z, oldRel);
        for (int c = 0; c < 3; ++c)
            maxOldErr = std::max(maxOldErr, std::fabs(static_cast<double>(oldRel[12 + c]) - ref[c]));
    }

    EXPECT_LT(maxNewErr, 1.0e-3) << "double rebase must track the exact reference (sub-mm)";
    EXPECT_GT(maxOldErr, 0.05) << "legacy fp32 rebase no longer shows the cancellation hazard"
                                  " at 3.68e6 — revisit whether the double path is still needed";
}

// ---- The far-field oracle: z-fighting resolved where the baseline collapses ---

TEST(RenderOrigin, FarFieldDepthResolvedWhereBaselineZFights)
{
    // Two sector-tagged surfaces 5 cm apart in DEPTH at ~6371 km. fp32-world
    // storage cannot represent 5 cm below the ~0.76 m ULP at that magnitude, so
    // the baseline collapses both to the same view-space depth — the exact
    // condition that makes far coplanar surfaces z-fight. The sector-local
    // relative path keeps the 5 cm in its small local remainder and resolves it.
    // View-space depth with an axis-aligned camera is worldZ - cameraZ, so the
    // difference isolates depth resolution directly and deterministically.
    //
    // Disable-and-fail evidence: baseDz (full-world storage) is ~0 (both surfaces
    // land on the same depth); relDz (sector-local) recovers the true 5 cm.
    const double R = 6.371e6;
    const double sep = 0.05; // 5 cm in depth
    const double w1d[3] = {5000.0, 200.0, R};
    const double w2d[3] = {5000.0, 200.0, R + sep};

    auto sectorOf = [](const double w[3]) {
        return Mathematics::Vector3Int{
            static_cast<int32>(std::llround(w[0] / kWorldSectorSize)),
            static_cast<int32>(std::llround(w[1] / kWorldSectorSize)),
            static_cast<int32>(std::llround(w[2] / kWorldSectorSize))};
    };
    auto localOf = [](const double w[3], const Mathematics::Vector3Int& s) {
        return Vector3(static_cast<float>(w[0] - s.x * kWorldSectorSize),
                       static_cast<float>(w[1] - s.y * kWorldSectorSize),
                       static_cast<float>(w[2] - s.z * kWorldSectorSize));
    };
    const Mathematics::Vector3Int sec1 = sectorOf(w1d), sec2 = sectorOf(w2d);
    const Vector3 loc1 = localOf(w1d, sec1), loc2 = localOf(w2d, sec2);
    const Vector3 w1f(static_cast<float>(w1d[0]), static_cast<float>(w1d[1]), static_cast<float>(w1d[2]));
    const Vector3 w2f(static_cast<float>(w2d[0]), static_cast<float>(w2d[1]), static_cast<float>(w2d[2]));

    // fp32 world storage cannot separate the two depths — the precondition for the
    // z-fight the relative path fixes.
    ASSERT_FLOAT_EQ(w1f.z, w2f.z);

    const Vector3 camPos(5000.0f, 200.0f, static_cast<float>(R) - 30.0f); // 30 m back, axis-aligned
    const Matrix4x4 view = Inverse(MakeCameraWorld(Matrix4x4::Identity(), camPos));

    Rendering::CameraData cam{};
    cam.view = view;
    cam.proj = Matrix4x4::Identity(); // proj irrelevant for view-space depth
    cam.viewProj = view;
    cam.cameraPos.x = camPos.x;
    cam.cameraPos.y = camPos.y;
    cam.cameraPos.z = camPos.z;
    Origin::ComputeRebasedView(cam.view, cam.proj, cam.viewProj, WorldPosition::FromVector3(camPos), cam.viewRel,
                               cam.viewProjRel, cam.renderOriginSector);
    ASSERT_NE(cam.renderOriginSector[0] | cam.renderOriginSector[1] | cam.renderOriginSector[2], 0);

    Matrix4x4 viewRel = cam.viewRel;
    const auto toCameraRelative = [&cam](const Mathematics::Vector3Int& sector, const Vector3& local) {
        return Vector3((sector.x - cam.renderOriginSector[0]) * kSectorSize32 + local.x,
                       (sector.y - cam.renderOriginSector[1]) * kSectorSize32 + local.y,
                       (sector.z - cam.renderOriginSector[2]) * kSectorSize32 + local.z);
    };

    // View-space depth (z of view * worldPos) difference.
    const float baseDz = view.Transform(Vector4(w1f.x, w1f.y, w1f.z, 1.0f)).z -
                         view.Transform(Vector4(w2f.x, w2f.y, w2f.z, 1.0f)).z;
    const Vector3 rel1 = toCameraRelative(sec1, loc1), rel2 = toCameraRelative(sec2, loc2);
    const float relDz = viewRel.Transform(Vector4(rel1.x, rel1.y, rel1.z, 1.0f)).z -
                        viewRel.Transform(Vector4(rel2.x, rel2.y, rel2.z, 1.0f)).z;

    EXPECT_NEAR(std::fabs(relDz), static_cast<float>(sep), 0.005f) << "relative path resolves the 5 cm depth";
    EXPECT_LT(std::fabs(baseDz), 0.01f) << "fp32-world baseline collapses the two depths (z-fight)";
    EXPECT_GT(std::fabs(relDz) - std::fabs(baseDz), 0.03f) << "relative recovers depth the baseline lost";
}

// ---- Slice 2 + #660 shadow follow-up: cascade shadow VP rebase ---------------
//
// ShadowMapRenderFeature::ComputeCascadeLightVP fits a cascade orthographic VP
// in world space, then builds CascadeFrameData::LightVPRel via RenderOrigin.h::
// ComputeRebasedLightVP — rebuilt in DOUBLE from the fit's rotation-only
// view + projection + shadow-camera eye, never through the world VP's fp32
// translation column (whose storage rounding at planetary magnitude is coarser
// than a shadow texel) nor through MakeLookAtLH(eye, eye + dir) (whose forward
// reconstruction fl(eye + dir) − eye quantizes the light DIRECTION to
// ULP(|eye|)). Both the depth pass (caster) and the receiver (ge_shadowVP)
// project render-origin-relative positions through the SAME matrix. These
// oracles lock the primitive at the math level with cascade-shaped inputs.

namespace
{
// The fit shape ComputeCascadeLightVP produces with the origin ACTIVE:
// rotation from the rotation-only look-at, world translation −R·eye derived
// from the rotation's own 3x3, reverse-Z ortho.
struct CascadeFitShape
{
    Matrix4x4 RotView; // rotation-only light view (translation zero)
    Matrix4x4 Proj;    // reverse-Z ortho
    Matrix4x4 WorldVP; // Proj * [R | -R*eye], fp32 world composition
};

CascadeFitShape MakeCascadeFit(const Vector3& lightPos, const Vector3& lightDir, float halfExtent,
                               float depthRange)
{
    Vector3 ld = lightDir;
    ld = ld.Normalize();
    const Vector3 up = std::fabs(Vector3::Dot(ld, Vector3{0, 1, 0})) > 0.99f ? Vector3{0, 0, 1}
                                                                             : Vector3{0, 1, 0};
    CascadeFitShape f;
    f.RotView = MakeLookAtLH(Vector3{0, 0, 0}, ld, up);
    f.Proj = MakeOrthographicLH_ZO_ReverseZ(-halfExtent, halfExtent, -halfExtent, halfExtent,
                                            0.0f, depthRange);
    Matrix4x4 view = f.RotView;
    float* v = view.Data();
    v[12] = static_cast<float>(-(static_cast<double>(v[0]) * lightPos.x +
                                 static_cast<double>(v[4]) * lightPos.y +
                                 static_cast<double>(v[8]) * lightPos.z));
    v[13] = static_cast<float>(-(static_cast<double>(v[1]) * lightPos.x +
                                 static_cast<double>(v[5]) * lightPos.y +
                                 static_cast<double>(v[9]) * lightPos.z));
    v[14] = static_cast<float>(-(static_cast<double>(v[2]) * lightPos.x +
                                 static_cast<double>(v[6]) * lightPos.y +
                                 static_cast<double>(v[10]) * lightPos.z));
    f.WorldVP = f.Proj * view;
    return f;
}

// The LEGACY cascade world VP: MakeLookAtLH(eye, eye + dir) — the pre-fix
// construction whose forward reconstruction quantizes to ULP(|eye|).
Matrix4x4 MakeLegacyCascadeWorldVP(const Vector3& lightPos, const Vector3& lightDir,
                                   float halfExtent, float depthRange)
{
    Vector3 ld = lightDir;
    ld = ld.Normalize();
    const Vector3 up = std::fabs(Vector3::Dot(ld, Vector3{0, 1, 0})) > 0.99f ? Vector3{0, 0, 1}
                                                                             : Vector3{0, 1, 0};
    const Matrix4x4 view = MakeLookAtLH(lightPos, lightPos + ld, up);
    const Matrix4x4 proj = MakeOrthographicLH_ZO_ReverseZ(-halfExtent, halfExtent, -halfExtent,
                                                          halfExtent, 0.0f, depthRange);
    return proj * view;
}

// Exact double reference: the clip position of a render-origin-relative point
// through the fit, every term small or exact in double. lightPosRel is the
// shadow-camera eye relative to the origin (exact in double).
void RefClipOfRel(const CascadeFitShape& f, const double lightPosRel[3], const Vector3& pRel,
                  double outClip[3])
{
    const float* r = f.RotView.Data();
    double viewRel[16];
    for (int i = 0; i < 12; ++i)
        viewRel[i] = static_cast<double>(r[i]);
    for (int row = 0; row < 3; ++row)
        viewRel[12 + row] = -(static_cast<double>(r[0 + row]) * lightPosRel[0] +
                              static_cast<double>(r[4 + row]) * lightPosRel[1] +
                              static_cast<double>(r[8 + row]) * lightPosRel[2]);
    viewRel[15] = static_cast<double>(r[15]);
    const float* p = f.Proj.Data();
    double vp[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            double acc = 0.0;
            for (int k = 0; k < 4; ++k)
                acc += static_cast<double>(p[k * 4 + row]) * viewRel[c * 4 + k];
            vp[c * 4 + row] = acc;
        }
    for (int row = 0; row < 3; ++row)
        outClip[row] = vp[0 + row] * pRel.x + vp[4 + row] * pRel.y + vp[8 + row] * pRel.z +
                       vp[12 + row];
}
} // namespace

TEST(RenderOrigin, CascadeVPRebaseBitIdenticalWhenInactive)
{
    // With the origin at sector (0,0,0), ComputeRebasedLightVP is a byte-for-byte
    // copy of the world VP, so LightVPRel equals LightVP.
    const CascadeFitShape f =
        MakeCascadeFit(Vector3{20.0f, 80.0f, -15.0f}, Vector3{-0.4f, -1.0f, -0.25f}, 40.0f, 220.0f);
    const Matrix4x4 vpRel = Origin::ComputeRebasedLightVP(f.RotView, f.Proj, f.WorldVP,
                                                          WorldPosition{20.0, 80.0, -15.0}, Vector3Int{0, 0, 0});
    EXPECT_EQ(0, std::memcmp(vpRel.Data(), f.WorldVP.Data(), 16 * sizeof(float)));
}

TEST(RenderOrigin, CascadeVPRebaseIsTheSameTransform)
{
    // Correctness: the double-rebuilt cascade VP applied to a render-origin-
    // relative caster position equals the world VP applied to the full world
    // position — proof the rebase is the SAME projection, not garbage. At
    // 40 km fp32 is still clean, so the two agree to well under a shadow texel.
    const Vector3 camPos(40000.0f, 300.0f, -9000.0f); // ~40 km, origin active
    const auto originSector = Origin::ComputeRenderOriginSector(WorldPosition::FromVector3(camPos));
    ASSERT_NE(originSector.x | originSector.y | originSector.z, 0);
    const Vector3 origin = Origin::SectorToWorld(originSector);

    // A cascade fit near the camera.
    const Vector3 lightPos(camPos.x + 30.0f, camPos.y + 120.0f, camPos.z - 20.0f);
    const CascadeFitShape f = MakeCascadeFit(lightPos, Vector3{-0.5f, -1.0f, -0.3f}, 60.0f, 300.0f);
    const Matrix4x4 vpRel = Origin::ComputeRebasedLightVP(f.RotView, f.Proj, f.WorldVP,
                                                          WorldPosition::FromVector3(lightPos), originSector);

    const Vector3 caster(camPos.x + 12.0f, camPos.y, camPos.z + 8.0f);
    const Vector3 rel = caster - origin;

    const Vector4 world = f.WorldVP.Transform(Vector4(caster.x, caster.y, caster.z, 1.0f));
    const Vector4 relClip = vpRel.Transform(Vector4(rel.x, rel.y, rel.z, 1.0f));

    // Orthographic => w == 1; compare NDC directly. 2048 shadow texels over the
    // [-1,1] NDC span => one texel is ~1e-3 NDC; require < half a texel.
    EXPECT_NEAR(relClip.x, world.x, 5e-4f);
    EXPECT_NEAR(relClip.y, world.y, 5e-4f);
    EXPECT_NEAR(relClip.z, world.z, 5e-4f);
}

TEST(RenderOrigin, CascadeVPRebaseResolvesSelfShadowDepthAtRange)
{
    // The acne mechanism, isolated: two caster surfaces 5 cm apart along the
    // light's depth axis at ~3.68e6 m (the #514 repro distance). The world-space
    // orthographic VP computes light-space depth as big·big cancellation, so both
    // surfaces round to the SAME NDC depth — the receiver then can't separate its
    // own surface from itself and speckles. The rebased VP projects small relative
    // positions; the constant coarse offset cancels in the per-vertex difference,
    // so the 5 cm resolves and the self-shadow is clean.
    const double R = 3.681e6; // matches the tagged-sphere sector (3595 * 1024)
    const Vector3 lightDir{-0.5f, -1.0f, -0.3f};
    const float halfExtent = 60.0f;
    const float depthRange = 300.0f;

    // Camera / render origin at the repro distance.
    const Vector3 camPos(static_cast<float>(R), 2.0f, static_cast<float>(R));
    const auto originSector = Origin::ComputeRenderOriginSector(WorldPosition::FromVector3(camPos));
    ASSERT_NE(originSector.x | originSector.y | originSector.z, 0);
    const Vector3 origin = Origin::SectorToWorld(originSector);

    // Light positioned near the camera; two casters 5 cm apart along +Y (a floor
    // fragment vs. the same fragment offset by the anti-acne bias magnitude).
    const Vector3 lightPos(camPos.x + 20.0f, camPos.y + 150.0f, camPos.z - 10.0f);
    const CascadeFitShape f = MakeCascadeFit(lightPos, lightDir, halfExtent, depthRange);
    const Matrix4x4 vpRel = Origin::ComputeRebasedLightVP(f.RotView, f.Proj, f.WorldVP,
                                                          WorldPosition::FromVector3(lightPos), originSector);

    const double sep = 0.05; // 5 cm
    const Vector3 c1(camPos.x + 5.0f, 2.0f, camPos.z + 5.0f);
    const Vector3 c2(camPos.x + 5.0f, 2.0f + static_cast<float>(sep), camPos.z + 5.0f);

    // World-space NDC depth (ortho => w==1).
    const float worldZ1 = f.WorldVP.Transform(Vector4(c1.x, c1.y, c1.z, 1.0f)).z;
    const float worldZ2 = f.WorldVP.Transform(Vector4(c2.x, c2.y, c2.z, 1.0f)).z;

    // Rebased NDC depth from render-origin-relative positions.
    const Vector3 r1 = c1 - origin;
    const Vector3 r2 = c2 - origin;
    const float relZ1 = vpRel.Transform(Vector4(r1.x, r1.y, r1.z, 1.0f)).z;
    const float relZ2 = vpRel.Transform(Vector4(r2.x, r2.y, r2.z, 1.0f)).z;

    const float worldDz = std::fabs(worldZ1 - worldZ2);
    const float relDz = std::fabs(relZ1 - relZ2);

    EXPECT_LT(worldDz, 1e-6f) << "fp32-world ortho VP collapses the 5 cm caster separation (acne)";
    EXPECT_GT(relDz, 1e-5f) << "rebased ortho VP resolves the caster separation (acne fixed)";
    EXPECT_GT(relDz, worldDz * 10.0f) << "relative path recovers depth resolution the world path lost";
}

TEST(RenderOrigin, CascadeVPRebaseClosureAtPlanetScale)
{
    // The #660 shadow-wobble gate, unit level. At |origin| 5e4 and Earth radius,
    // sweep the shadow-camera eye through micro-steps (what a translating camera
    // does to the fit every frame) and measure the worst clip-space deviation of
    // the rebased VP from the exact double reference over caster points near the
    // camera. The deviation bounds frame-to-frame shadow-edge wobble (each
    // frame's matrix is within eps of the same exact transform).
    //   * New path (ComputeRebasedLightVP): < 1e-5 NDC (~0.01 texel @2048).
    //   * Legacy path (MakeLookAtLH(eye, eye+dir) world VP + fp32 column
    //     rebase): the ULP(|eye|) direction quantization + big+big translation
    //     cancellation — texel-scale at 5e4, catastrophic at Earth radius.
    struct Case
    {
        Vector3 CamPos;
        double LegacyFloor; // the legacy error the fix must beat
    };
    const Case cases[] = {
        {Vector3(50000.0f, 300.0f, -9000.0f), 2.0e-4},   // R=50000 (the report)
        {Vector3(6371000.0f, 500.0f, 6371000.0f), 2.0e-2}, // Earth radius
    };
    const Vector3 lightDir{-0.5f, -1.0f, -0.3f};
    const float halfExtent = 60.0f;
    const float depthRange = 300.0f;

    for (const Case& tc : cases)
    {
        const auto originSector = Origin::ComputeRenderOriginSector(WorldPosition::FromVector3(tc.CamPos));
        ASSERT_NE(originSector.x | originSector.y | originSector.z, 0);
        const double size = kWorldSectorSize;
        const double originD[3] = {originSector.x * size, originSector.y * size,
                                   originSector.z * size};

        double maxNewErr = 0.0;
        double maxLegacyErr = 0.0;
        for (int step = 0; step < 24; ++step)
        {
            // Micro-step the fit's shadow-camera eye — 12.5 cm per step, the
            // scale at which a walking camera re-fits the cascade.
            const float dt = 0.125f * static_cast<float>(step);
            const Vector3 lightPos(tc.CamPos.x + 20.0f + dt, tc.CamPos.y + 150.0f,
                                   tc.CamPos.z - 10.0f + 0.5f * dt);
            const CascadeFitShape f = MakeCascadeFit(lightPos, lightDir, halfExtent, depthRange);
            const Matrix4x4 vpRel = Origin::ComputeRebasedLightVP(
                f.RotView, f.Proj, f.WorldVP, WorldPosition::FromVector3(lightPos), originSector);
            const Matrix4x4 legacyVP =
                MakeLegacyCascadeWorldVP(lightPos, lightDir, halfExtent, depthRange);
            Matrix4x4 legacyRel = legacyVP;
            LegacyRebaseTranslationColumnFp32(legacyVP.Data(),
                                              static_cast<float>(originD[0]),
                                              static_cast<float>(originD[1]),
                                              static_cast<float>(originD[2]), legacyRel.Data());

            const double lightPosRel[3] = {static_cast<double>(lightPos.x) - originD[0],
                                           static_cast<double>(lightPos.y) - originD[1],
                                           static_cast<double>(lightPos.z) - originD[2]};
            // Caster/receiver points near the camera, exact small rel coords.
            const Vector3 camRel(static_cast<float>(static_cast<double>(tc.CamPos.x) - originD[0]),
                                 static_cast<float>(static_cast<double>(tc.CamPos.y) - originD[1]),
                                 static_cast<float>(static_cast<double>(tc.CamPos.z) - originD[2]));
            const Vector3 offsets[] = {{5.0f, 0.0f, 5.0f}, {-12.0f, 2.0f, 8.0f},
                                       {25.0f, 10.0f, -14.0f}, {0.0f, 0.25f, 0.0f}};
            for (const Vector3& off : offsets)
            {
                const Vector3 pRel(camRel.x + off.x, camRel.y + off.y, camRel.z + off.z);
                double ref[3];
                RefClipOfRel(f, lightPosRel, pRel, ref);

                const Vector4 c = vpRel.Transform(Vector4(pRel.x, pRel.y, pRel.z, 1.0f));
                maxNewErr = std::max({maxNewErr, std::fabs(c.x - ref[0]),
                                      std::fabs(c.y - ref[1]), std::fabs(c.z - ref[2])});

                // Legacy consumed the fp32 WORLD position through the fp32
                // world VP + fp32 rebase — evaluate the whole legacy chain.
                const Vector3 pWorld(static_cast<float>(originD[0] + pRel.x),
                                     static_cast<float>(originD[1] + pRel.y),
                                     static_cast<float>(originD[2] + pRel.z));
                const Vector3 pRelLegacy(pWorld.x - static_cast<float>(originD[0]),
                                         pWorld.y - static_cast<float>(originD[1]),
                                         pWorld.z - static_cast<float>(originD[2]));
                const Vector4 lc =
                    legacyRel.Transform(Vector4(pRelLegacy.x, pRelLegacy.y, pRelLegacy.z, 1.0f));
                maxLegacyErr = std::max({maxLegacyErr, std::fabs(lc.x - ref[0]),
                                         std::fabs(lc.y - ref[1]), std::fabs(lc.z - ref[2])});
            }
        }

        // Evidence line for the fails-before record (clip units; 1 texel @2048
        // is ~9.8e-4; world meters = clip * halfExtent).
        std::cout << "[closure] |origin|~" << originD[0] << "  legacy=" << maxLegacyErr
                  << " clip (" << maxLegacyErr * halfExtent << " m)  new=" << maxNewErr
                  << " clip (" << maxNewErr * halfExtent << " m)\n";

        EXPECT_LT(maxNewErr, 1e-5) << "double-rebuilt cascade VP must track the exact reference"
                                   << " at |origin| ~" << originD[0];
        EXPECT_GT(maxLegacyErr, tc.LegacyFloor)
            << "legacy fp32 cascade path no longer shows the hazard at |origin| ~" << originD[0]
            << " — revisit whether the double path is still needed";
        EXPECT_GT(maxLegacyErr, maxNewErr * 100.0)
            << "fix must collapse the wobble by orders of magnitude";
    }
}

TEST(RenderOrigin, CascadeSnapshotSectorDeltaRebaseExactAcrossSectorStep)
{
    // The motion round-robin's deferral-window absorption: a retained rel fit
    // committed under sector A, re-anchored to the camera's new sector B via
    // RebaseTranslationColumnBySectorDelta (delta exact in double from the
    // integer sectors). Must match building the rel VP directly at sector B to
    // sub-texel precision — at Earth radius, where the legacy world-VP re-rebase
    // carried ULP(|origin|) storage rounding.
    const Vector3 camPos(6371000.0f, 500.0f, 6371000.0f);
    const auto secA = Origin::ComputeRenderOriginSector(WorldPosition::FromVector3(camPos));
    ASSERT_NE(secA.x | secA.y | secA.z, 0);
    const int32 from[3] = {secA.x, secA.y, secA.z};
    const int32 to[3] = {secA.x + 1, secA.y, secA.z - 2}; // a sector step mid-deferral

    const Vector3 lightPos(camPos.x + 20.0f, camPos.y + 150.0f, camPos.z - 10.0f);
    const CascadeFitShape f = MakeCascadeFit(lightPos, Vector3{-0.5f, -1.0f, -0.3f}, 60.0f, 300.0f);

    const WorldPosition eye = WorldPosition::FromVector3(lightPos);
    const Matrix4x4 relA =
        Origin::ComputeRebasedLightVP(f.RotView, f.Proj, f.WorldVP, eye, Vector3Int{from[0], from[1], from[2]});
    const Matrix4x4 relB =
        Origin::ComputeRebasedLightVP(f.RotView, f.Proj, f.WorldVP, eye, Vector3Int{to[0], to[1], to[2]});

    Matrix4x4 stepped = relA;
    Origin::RebaseTranslationColumnBySectorDelta(relA.Data(), from, to, stepped.Data());

    // Rotation/scale columns untouched (bit-for-bit), translation column within
    // ~1 fp32 ULP of the direct build (both are one rounding away from the same
    // double value). 1e-5 clip ≈ 0.01 texel @2048.
    EXPECT_EQ(0, std::memcmp(stepped.Data(), relA.Data(), 12 * sizeof(float)));
    for (int i = 12; i < 16; ++i)
        EXPECT_NEAR(stepped.Data()[i], relB.Data()[i], 1e-5f) << "column element " << i;
}

// A model matrix moves to another sector on the left of the product: the translation moves by
// (from - to) * 1,024 m in world axes, whatever the matrix's rotation and scale.
TEST(RenderOrigin, PremultiplyTranslationBySectorDeltaMovesAModelMatrixInWorldAxes)
{
    // A quarter turn about +Y with scale 2: a right-side product would move the translation along Z.
    Matrix4x4 m;
    m[0] = glm::vec4(0.0f, 0.0f, -2.0f, 0.0f);
    m[1] = glm::vec4(0.0f, 2.0f, 0.0f, 0.0f);
    m[2] = glm::vec4(2.0f, 0.0f, 0.0f, 0.0f);
    m[3] = glm::vec4(100.0f, 5.0f, -7.0f, 1.0f);
    const Matrix4x4 out = Origin::PremultiplyTranslationBySectorDelta(m, Vector3Int(1, 0, 0), Vector3Int(2, 0, 0));

    EXPECT_EQ(0, std::memcmp(out.Data(), m.Data(), 12 * sizeof(float))) << "the 3x3 and its last row stay";
    EXPECT_EQ(out[3][0], 100.0f - 1024.0f);
    EXPECT_EQ(out[3][1], 5.0f);
    EXPECT_EQ(out[3][2], -7.0f);
    EXPECT_EQ(out[3][3], 1.0f);
}

// The DDGI and RT-shadow TLAS builds compose an instance's world matrix from its split. For a
// world position that is a float, the composed translation is that float bit for bit.
TEST(RenderOrigin, ComposeWorldModelMatrixGivesAFloatPositionBitForBit)
{
    const float positions[] = {0.25f,      0.37f,      -511.99f,   511.99f,    512.0f,     -512.0f,
                               1535.9f,    1536.1f,    -1536.1f,   4095.75f,   300000.0123f,
                               -299000.0071f, 1200.5f, 1048575.5f, -2097152.25f, 16000000.0f};
    for (const float x : positions)
    {
        for (const float y : {x, -x * 0.75f, 3.0f})
        {
            const WorldPosition position{x, y, -x};
            Vector3Int sector;
            Vector3 local;
            position.ToSector(sector, local);
            Matrix4x4 m;
            m[3] = glm::vec4(local.x, local.y, local.z, 1.0f);
            uint32 p0 = 0u;
            uint32 p1 = 0u;
            Rendering::PackSector(sector.x, sector.y, sector.z, p0, p1);
            const Matrix4x4 out = Origin::ComposeWorldModelMatrix(m, p0, p1);
            // No input is zero, so float equality here is equality of the bits.
            EXPECT_EQ(out[3][0], x);
            EXPECT_EQ(out[3][1], y);
            EXPECT_EQ(out[3][2], -x);
            EXPECT_EQ(0, std::memcmp(out.Data(), m.Data(), 12 * sizeof(float)));
        }
    }
}

// ---- Culling in the frame of the render origin sector ---------------------------
//
// The culling kernels test a sector-local center composed against the view's sector
// (ge_SectorRelativePoint) with planes from viewProjRel. This oracle is the CPU replica
// of that predicate against a reference computed from the exact relative position.

namespace
{
// CPU replica of SphereOutsideFrustum (Shaders/Includes/frustum_tests.glsl) as a visible verdict, without the margin.
bool TestSphereFrustumExact(const Vector3& center, float radius, const Vector4* planes)
{
    for (int i = 0; i < 6; ++i)
    {
        const float dist = center.x * planes[i].x + center.y * planes[i].y + center.z * planes[i].z + planes[i].w;
        if (dist < -radius)
            return false;
    }
    return true;
}
} // namespace

TEST(RenderOrigin, CullPredicateOnComposedCentersMatchesReferenceAtRange)
{
    // A camera at about 5,200 km. Each instance is stored local to its own sector, one
    // sector from the camera's at most, and is composed against the camera's sector.
    const Vector3 camPos(3.681e6f, 500.0f, 3.681e6f);
    const Matrix4x4 rot = MakeRotationY(0.6f) * MakeRotationX(-0.15f);
    const Matrix4x4 view = Inverse(MakeCameraWorld(rot, camPos));
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(55.0f * kDeg2Rad, 16.0f / 9.0f, 0.5f, 4000.0f);

    Rendering::CameraData cam{};
    cam.view = view;
    cam.proj = proj;
    cam.viewProj = proj * view;
    Origin::ComputeRebasedView(cam.view, cam.proj, cam.viewProj, WorldPosition::FromVector3(camPos), cam.viewRel,
                               cam.viewProjRel, cam.renderOriginSector);
    const Vector3Int cameraSector{cam.renderOriginSector[0], cam.renderOriginSector[1],
                                  cam.renderOriginSector[2]};
    ASSERT_NE(cameraSector.x | cameraSector.y | cameraSector.z, 0);

    Vector4 planes[6];
    GameEngine::Rendering::ExtractFrustumPlanes(cam.viewProjRel, planes);

    const double sectorSize = kWorldSectorSize;
    const Vector3 eye(static_cast<float>(camPos.x - cameraSector.x * sectorSize),
                         static_cast<float>(camPos.y - cameraSector.y * sectorSize),
                         static_cast<float>(camPos.z - cameraSector.z * sectorSize));
    const float radius = 1.5f;
    int insideCount = 0;
    for (int gx = -6; gx <= 6; ++gx)
        for (int gy = -4; gy <= 4; ++gy)
            for (int gz = 1; gz <= 8; ++gz)
            {
                // A grid in front of the eye, relative to the origin of the camera's sector.
                const Vector4 step = rot.Transform(Vector4(static_cast<float>(gx) * 40.0f, static_cast<float>(gy) * 40.0f,
                                                           static_cast<float>(gz) * 60.0f, 0.0f));
                const Vector3 center(eye.x + step.x, eye.y + step.y, eye.z + step.z);
                const Vector3Int offset{static_cast<int32_t>(std::floor(center.x / sectorSize)),
                                        static_cast<int32_t>(std::floor(center.y / sectorSize)),
                                        static_cast<int32_t>(std::floor(center.z / sectorSize))};
                const Vector3 local(static_cast<float>(center.x - offset.x * sectorSize),
                                    static_cast<float>(center.y - offset.y * sectorSize),
                                    static_cast<float>(center.z - offset.z * sectorSize));
                uint32_t p0 = 0, p1 = 0;
                Rendering::PackSector(cameraSector.x + offset.x, cameraSector.y + offset.y, cameraSector.z + offset.z,
                                   p0, p1);
                const Vector3 composed =
                    GameEngine::Rendering::SectorLocalPointRelativeTo(local, p0, p1, cameraSector);

                const bool reference = TestSphereFrustumExact(center, radius, planes);
                EXPECT_EQ(TestSphereFrustumExact(composed, radius, planes), reference)
                    << "(" << gx << "," << gy << "," << gz << ")";
                insideCount += reference ? 1 : 0;
            }
    EXPECT_GT(insideCount, 0) << "the grid must straddle the frustum";
    EXPECT_LT(insideCount, 13 * 9 * 8) << "the grid must straddle the frustum";
}

// A center in the sector of the view keeps its local value bit for bit at any distance, as
// ge_SectorRelativePoint gives on the GPU: the integer sectors subtract first, then one rounding.
TEST(RenderOrigin, SectorLocalPointRoundsOnceAtAnyDistance)
{
    const Vector3Int sector{Rendering::kSectorAxisMax, -6222, 1024};
    const Vector3 local(0.3f, 0.003f, 0.001f);
    uint32 p0 = 0, p1 = 0;
    Rendering::PackSector(sector.x, sector.y, sector.z, p0, p1);
    const Vector3 composed = Rendering::SectorLocalPointRelativeTo(local, p0, p1, sector);
    EXPECT_EQ(composed.x, local.x);
    EXPECT_EQ(composed.y, local.y);
    EXPECT_EQ(composed.z, local.z);
}

namespace
{
// A camera whose view is built from the fp32 eye and whose double eye is set by SetCameraEye.
Rendering::CameraData MakeCameraAtEye(const WorldPosition& eye)
{
    const Vector3 eyeF(static_cast<float>(eye.X), static_cast<float>(eye.Y), static_cast<float>(eye.Z));
    Rendering::CameraData cam{};
    cam.view = Inverse(MakeCameraWorld(MakeRotationY(0.7f), eyeF));
    cam.proj = MakePerspectiveLH_ZO_ReverseZ(60.0f * kDeg2Rad, 16.0f / 9.0f, 0.1f, 1.0e5f);
    cam.viewProj = cam.proj * cam.view;
    Origin::SetCameraEye(cam, eye);
    return cam;
}

// A point light spawned at `position` and extracted for one frame: the world's one extracted light.
Origin::ExtractedLight ExtractPointLightAt(Testing::ExtractionFastPath::ExtractionHarness& harness,
                                           const WorldPosition& position)
{
    ECS::Entity e = harness.world->Create();
    e.Set(Components::Transform::FromTRS(position, Quaternion{}, Vector3{1.0f, 1.0f, 1.0f}));
    Components::Light light{};
    light.Type = Components::LightType::Point;
    e.Set(light);
    harness.StepFrame();
    const auto lights = harness.rs.GetWorldLights(harness.worldId);
    EXPECT_EQ(lights.size(), 1u);
    return lights.empty() ? Origin::ExtractedLight{} : lights.front();
}

void ExpectBitEqual(const Vector3& actual, const Vector3& expected)
{
    EXPECT_EQ(std::bit_cast<uint32>(actual.x), std::bit_cast<uint32>(expected.x));
    EXPECT_EQ(std::bit_cast<uint32>(actual.y), std::bit_cast<uint32>(expected.y));
    EXPECT_EQ(std::bit_cast<uint32>(actual.z), std::bit_cast<uint32>(expected.z));
}

// A non-global 2 m box volume at `center` with a volumetric fog effect of density 1.
void AddFogVolumeAt(Testing::ExtractionFastPath::ExtractionHarness& harness, const WorldPosition& center,
                    float blendDistance)
{
    ECS::Entity e = harness.world->Create();
    e.Set(Components::Transform::FromTRS(center, Quaternion{}, Vector3{2.0f, 2.0f, 2.0f}));
    Components::PostProcessVolume volume{};
    volume.IsGlobal = false;
    volume.BlendDistance = blendDistance;
    e.Set(volume);
    Components::VolumetricFogEffect fog{};
    fog.Density = 1.0f;
    e.Set(fog);
}

// The world's resolved fog density for a fog volume at `center` seen from `eye`.
float ResolvedFogDensity(Testing::ExtractionFastPath::ExtractionHarness& harness, const WorldPosition& center,
                         const WorldPosition& eye)
{
    AddFogVolumeAt(harness, center, 0.01f);
    harness.rs.Views().SetCameraData(harness.cameraId, MakeCameraAtEye(eye),
                                     Testing::ExtractionFastPath::kIdentityCameraRanges);
    harness.StepFrame();
    return harness.rs.GetWorldPostProcessSettings(harness.worldId).VolumetricFogDensity;
}

// A 4 m box probe at `center` with its capture origin moved by `originOffset`, a camera at `eye`, and one
// selection pass. Returns the probe source that the IBL feature captures, or null.
const Origin::SceneReflectionProbeEnvironmentSource* SelectBoxProbeAt(
    Testing::ExtractionFastPath::ExtractionHarness& harness, const WorldPosition& center,
    const Vector3& originOffset, const WorldPosition& eye)
{
    ECS::Entity probeEntity = harness.world->Create();
    probeEntity.Set(Components::Transform::FromTRS(center, Quaternion{}, Vector3{4.0f, 4.0f, 4.0f}));
    Components::ReflectionProbe probe{};
    probe.BoxProjection = true;
    probe.BlendDistance = 0.0f;
    probe.OriginOffsetX = originOffset.x;
    probe.OriginOffsetY = originOffset.y;
    probe.OriginOffsetZ = originOffset.z;
    probeEntity.Set(probe);
    ECS::Entity cameraEntity = harness.world->Create();
    cameraEntity.Set(Components::Transform::FromTRS(eye, Quaternion{}, Vector3{1.0f, 1.0f, 1.0f}));
    cameraEntity.Set(Components::Camera{});
    harness.StepFrame();

    Origin::ReflectionProbeSystem system(&harness.rs);
    system.Update(*harness.world, 0.016f);
    const auto* feature = harness.rs.GetFeature<Origin::ImageBasedLightingFeature>();
    return feature ? dynamic_cast<const Origin::SceneReflectionProbeEnvironmentSource*>(feature->GetEnvironmentSource())
                   : nullptr;
}

int32 UlpDistance(float a, float b)
{
    return std::abs(std::bit_cast<int32>(a) - std::bit_cast<int32>(b));
}

class RenderOriginExtraction : public Testing::ExtractionFastPath::ExtractionFastPathTest
{
};
} // namespace

// A light 2 m from the eye at 100 km: the point and the eye relative to the view's sector differ by the
// exact 2 m. The fp32 world positions differ by up to 7.8 mm there.
TEST(RenderOrigin, ViewRelativePointSubtractsInDoubleAndRoundsOnce)
{
    const WorldPosition eye{100002.3, 2.0, 100000.0};
    const WorldPosition light{100000.3, 2.0, 100000.0};
    const Rendering::CameraData cam = MakeCameraAtEye(eye);

    const Vector3 eyeRel = Origin::CameraEyeRelativeToOrigin(cam);
    const Vector3 toLight = Origin::ViewRelativePoint(cam, light) - eyeRel;
    EXPECT_NEAR(toLight.x, -2.0f, 1.0e-4f);
    EXPECT_NEAR(toLight.y, 0.0f, 1.0e-4f);
    EXPECT_NEAR(toLight.z, 0.0f, 1.0e-4f);
    const WorldPosition eyeBack =
        WorldPosition::FromSector(Origin::CameraRenderOriginSector(cam), Vector3{}) + eyeRel;
    EXPECT_NEAR(eyeBack.X, eye.X, 1.0e-4);
    EXPECT_NEAR(eyeBack.Z, eye.Z, 1.0e-4);

    // In sector (0,0,0) the point is the position rounded to fp32 and the eye is cameraPos, bit for bit.
    const Rendering::CameraData nearCam = MakeCameraAtEye(WorldPosition{200.1, 2.3, -40.7});
    const WorldPosition nearLight{198.13, 2.71, -40.07};
    const Vector3 nearRel = Origin::ViewRelativePoint(nearCam, nearLight);
    const Vector3 rounded = nearLight - WorldPosition{};
    ExpectBitEqual(nearRel, rounded);
    const Vector3 nearEye = Origin::CameraEyeRelativeToOrigin(nearCam);
    ExpectBitEqual(nearEye, Vector3{nearCam.cameraPos.x, nearCam.cameraPos.y, nearCam.cameraPos.z});
}

// The extracted light keeps its exact world position, so its point relative to a view at 100 km is
// exact. A light rounded to fp32 at extraction is off by up to 7.8 mm there.
TEST_F(RenderOriginExtraction, AFarLightIsExactRelativeToTheView)
{
    Testing::ExtractionFastPath::ExtractionHarness harness;
    ASSERT_TRUE(harness.Initialize(m_Device.get(), /*fastPath*/ true));
    const Origin::ExtractedLight light = ExtractPointLightAt(harness, WorldPosition{100000.3, 2.0, 100000.0});

    const Rendering::CameraData cam = MakeCameraAtEye(WorldPosition{100002.3, 2.0, 100000.0});
    const Vector3 toLight = Origin::ViewRelativePoint(cam, light.positionWS) - Origin::CameraEyeRelativeToOrigin(cam);
    EXPECT_NEAR(toLight.x, -2.0f, 1.0e-4f);
    EXPECT_NEAR(toLight.y, 0.0f, 1.0e-4f);
    EXPECT_NEAR(toLight.z, 0.0f, 1.0e-4f);
}

// Within 512 m the extracted light position, rounded to fp32, is the fp32 rounding of the transform's
// world position, bit for bit.
TEST_F(RenderOriginExtraction, ANearLightRoundsToTheSameBytes)
{
    Testing::ExtractionFastPath::ExtractionHarness harness;
    ASSERT_TRUE(harness.Initialize(m_Device.get(), /*fastPath*/ true));
    const WorldPosition position{198.13, 2.71, -40.07};
    const Origin::ExtractedLight light = ExtractPointLightAt(harness, position);

    const Vector3 uploaded = light.positionWS - WorldPosition{};
    const Vector3 golden = Components::Transform::FromTRS(position, Quaternion{}, Vector3{1.0f, 1.0f, 1.0f})
                               .GetPosition() -
                           WorldPosition{};
    ExpectBitEqual(uploaded, golden);
}

// A fog volume at 100 km with a 1 cm blend band and the eye 5 mm outside its face: the spatial weight
// reads the exact 5 mm, as it does at 100 m. A center rounded to fp32 moves the face by up to 3.9 mm.
TEST_F(RenderOriginExtraction, AFarFogVolumeWeighsTheExactDistanceToTheEye)
{
    float nearDensity = 0.0f;
    {
        Testing::ExtractionFastPath::ExtractionHarness harness;
        ASSERT_TRUE(harness.Initialize(m_Device.get(), /*fastPath*/ true));
        nearDensity = ResolvedFogDensity(harness, WorldPosition{100.3, 2.0, 100.0}, WorldPosition{101.305, 2.0, 100.0});
    }
    Testing::ExtractionFastPath::ExtractionHarness harness;
    ASSERT_TRUE(harness.Initialize(m_Device.get(), /*fastPath*/ true));
    const float farDensity =
        ResolvedFogDensity(harness, WorldPosition{100000.3, 2.0, 100000.0}, WorldPosition{100001.305, 2.0, 100000.0});
    EXPECT_GT(nearDensity, 0.0f);
    EXPECT_NEAR(farDensity, nearDensity, 0.01f);
}

// Within 512 m the fog volume center that the world settings and the local fog volume list upload is the
// fp32 rounding of the transform's world position, bit for bit.
TEST_F(RenderOriginExtraction, ANearFogVolumeUploadsTheSameBytes)
{
    Testing::ExtractionFastPath::ExtractionHarness harness;
    ASSERT_TRUE(harness.Initialize(m_Device.get(), /*fastPath*/ true));
    const WorldPosition center{198.13, 2.71, -40.07};
    ResolvedFogDensity(harness, center, center);

    const Vector3 golden =
        Components::Transform::FromTRS(center, Quaternion{}, Vector3{2.0f, 2.0f, 2.0f}).GetPosition() - WorldPosition{};
    const Origin::PostProcessSettings& settings = harness.rs.GetWorldPostProcessSettings(harness.worldId);
    ExpectBitEqual(Vector3{settings.VolumetricFogVolumeCenterX, settings.VolumetricFogVolumeCenterY,
                           settings.VolumetricFogVolumeCenterZ},
                   golden);
    const auto volumes = harness.rs.GetWorldVolumetricFogVolumes(harness.worldId);
    ASSERT_EQ(volumes.size(), 1u);
    ExpectBitEqual(Vector3{volumes[0].center[0], volumes[0].center[1], volumes[0].center[2]}, golden);
}

// A box probe at 100 km: its box corner and its capture position relative to a view inside it are exact.
// A center rounded to fp32 is off by up to 3.9 mm there.
TEST_F(RenderOriginExtraction, AFarProbeBoxIsExactRelativeToTheView)
{
    Testing::ExtractionFastPath::ExtractionHarness harness;
    ASSERT_TRUE(harness.Initialize(m_Device.get(), /*fastPath*/ true));
    const WorldPosition center{100000.3, 2.0, 100000.0};
    const WorldPosition eye{100001.8, 2.5, 100000.0};
    const auto* source = SelectBoxProbeAt(harness, center, Vector3{0.25f, 0.0f, 0.0f}, eye);
    ASSERT_NE(source, nullptr);

    const Rendering::CameraData cam = MakeCameraAtEye(eye);
    const Vector3 eyeRel = Origin::CameraEyeRelativeToOrigin(cam);
    const Vector3 toCorner =
        Origin::ViewRelativePoint(cam, source->GetProbe().BoxCenter + Vector3{2.0f, 2.0f, 2.0f}) - eyeRel;
    EXPECT_NEAR(toCorner.x, 0.5f, 1.0e-4f);
    EXPECT_NEAR(toCorner.y, 1.5f, 1.0e-4f);
    EXPECT_NEAR(toCorner.z, 2.0f, 1.0e-4f);
    const Vector3 toCapture = Origin::ViewRelativePoint(cam, source->GetProbe().Position) - eyeRel;
    EXPECT_NEAR(toCapture.x, -1.25f, 1.0e-4f);
    EXPECT_NEAR(toCapture.y, -0.5f, 1.0e-4f);
    EXPECT_NEAR(toCapture.z, 0.0f, 1.0e-4f);
}

// Within 512 m the uploaded box center is the fp32 rounding of the transform's world position, bit for bit.
// For this axis-aligned probe with small offsets, the capture position (offset added in double, rounded
// once) is within one unit in the last place of the fp32 sum of the rounded center and the offset.
TEST_F(RenderOriginExtraction, ANearProbeUploadsTheSameBoxCenterAndACapturePositionWithinOneUlp)
{
    Testing::ExtractionFastPath::ExtractionHarness harness;
    ASSERT_TRUE(harness.Initialize(m_Device.get(), /*fastPath*/ true));
    const WorldPosition center{198.13, 2.71, -40.07};
    const Vector3 offset{0.37f, -0.21f, 0.13f};
    const auto* source = SelectBoxProbeAt(harness, center, offset, center);
    ASSERT_NE(source, nullptr);

    const Vector3 roundedCenter =
        Components::Transform::FromTRS(center, Quaternion{}, Vector3{4.0f, 4.0f, 4.0f}).GetPosition() - WorldPosition{};
    const Vector3 fp32Capture = roundedCenter + Vector3{1.0f, 0.0f, 0.0f} * offset.x +
                                Vector3{0.0f, 1.0f, 0.0f} * offset.y + Vector3{0.0f, 0.0f, 1.0f} * offset.z;
    const Origin::EnvironmentLocalReflectionData uploaded = source->LocalReflectionData();
    ASSERT_TRUE(uploaded.BoxProjection);
    ExpectBitEqual(uploaded.BoxCenterWS, roundedCenter);
    EXPECT_LE(UlpDistance(uploaded.ProbePositionWS.x, fp32Capture.x), 1);
    EXPECT_LE(UlpDistance(uploaded.ProbePositionWS.y, fp32Capture.y), 1);
    EXPECT_LE(UlpDistance(uploaded.ProbePositionWS.z, fp32Capture.z), 1);
}
