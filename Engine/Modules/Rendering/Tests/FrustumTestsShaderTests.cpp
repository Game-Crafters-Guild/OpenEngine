// The frustum tests of Shaders/Includes/frustum_tests.glsl, lifted verbatim out of the shader at build
// time (ExtractShaderBlock.cmake) and compiled as C++ through GlslShim.h, so these tests run the
// shader's own code. The planes come from Rendering::ExtractFrustumPlanes, the producer every kernel reads.

#include <gtest/gtest.h>

#include "GlslShim.h"
#include "Mathematics/Angle.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Frustum.h"

#include <array>
#include <initializer_list>

namespace FrustumTestsShader
{
using namespace GameEngine::GlslShim;
#include "FrustumTestsExtracted.h"
} // namespace FrustumTestsShader

namespace
{
using GameEngine::GlslShim::vec3;
using GameEngine::GlslShim::vec4;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector3;
using GameEngine::Mathematics::Vector4;
using Planes = std::array<vec4, 6>;

// Plane indices in Rendering::ExtractFrustumPlanes order.
constexpr int kNearPlane = 4;
constexpr int kFarPlane = 5;
// A plane every point is inside: it removes one plane from a test.
const vec4 kAlwaysInsidePlane{0.0f, 0.0f, 0.0f, 1.0f};

Planes ExtractPlanes(const Matrix4x4& viewProj)
{
    Vector4 extracted[6];
    GameEngine::Rendering::ExtractFrustumPlanes(viewProj, extracted);
    Planes planes;
    for (int i = 0; i < 6; ++i)
        planes[i] = vec4(extracted[i].x, extracted[i].y, extracted[i].z, extracted[i].w);
    return planes;
}

// The cube |x|, |y|, |z| <= 10 as six inward planes in ExtractFrustumPlanes order. Integer
// coordinates make a sphere that touches a plane land exactly on the cull threshold.
Planes AxisCubePlanes()
{
    return {vec4(1.0f, 0.0f, 0.0f, 10.0f), vec4(-1.0f, 0.0f, 0.0f, 10.0f), vec4(0.0f, 1.0f, 0.0f, 10.0f),
            vec4(0.0f, -1.0f, 0.0f, 10.0f), vec4(0.0f, 0.0f, 1.0f, 10.0f), vec4(0.0f, 0.0f, -1.0f, 10.0f)};
}

// A camera at the origin that looks along +Z, reverse-Z, near 0.5 m and far 100 m.
Planes ForwardCameraPlanes()
{
    return ExtractPlanes(GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(
        GameEngine::Mathematics::ToRadians(60.0f), 1.0f, 0.5f, 100.0f));
}

// The sphere verdict of the culling kernels as one plain expression, the oracle that the shared
// test matches on every fixture: visible unless the center is more than `radius` outside one plane.
bool KernelSphereVisible(vec3 center, float radius, const Planes& planes)
{
    for (const vec4& plane : planes)
    {
        const float distance = dot(center, vec3(plane.x, plane.y, plane.z)) + plane.w;
        if (distance < -radius)
            return false;
    }
    return true;
}

// The box verdict with the corner pick of GLSL mix(boxMin, boxMax, greaterThan(n, vec3(0))). Where a
// normal component is zero it picks the other corner from the shared test, and that term is zero for both.
bool StrictCornerBoxOutside(vec3 boxMin, vec3 boxMax, const Planes& planes)
{
    for (const vec4& plane : planes)
    {
        const vec3 corner(plane.x > 0.0f ? boxMax.x : boxMin.x, plane.y > 0.0f ? boxMax.y : boxMin.y,
                          plane.z > 0.0f ? boxMax.z : boxMin.z);
        if (dot(vec3(plane.x, plane.y, plane.z), corner) + plane.w < 0.0f)
            return true;
    }
    return false;
}

// A camera above the ground that looks down a long view, reverse-Z, near 0.5 m and far 500 m.
Planes PerspectiveViewPlanes()
{
    const Matrix4x4 view = GameEngine::Mathematics::MakeLookAtLH(Vector3(3.0f, 2.0f, -7.0f), Vector3(10.0f, 0.0f, 40.0f),
                                                                 Vector3(0.0f, 1.0f, 0.0f));
    const Matrix4x4 perspective = GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(
        GameEngine::Mathematics::ToRadians(60.0f), 16.0f / 9.0f, 0.5f, 500.0f);
    return ExtractPlanes(perspective * view);
}

// A shadow cascade's orthographic light view, reverse-Z.
Planes OrthographicLightPlanes()
{
    const Matrix4x4 lightView = GameEngine::Mathematics::MakeLookAtLH(
        Vector3(0.0f, 100.0f, 0.0f), Vector3(20.0f, 0.0f, 30.0f), Vector3(0.0f, 1.0f, 0.0f));
    const Matrix4x4 orthographic =
        GameEngine::Mathematics::MakeOrthographicLH_ZO_ReverseZ(-50.0f, 50.0f, -30.0f, 30.0f, 0.1f, 200.0f);
    return ExtractPlanes(orthographic * lightView);
}

bool SphereOutside(vec3 center, float radius, Planes planes)
{
    return FrustumTestsShader::SphereOutsideFrustum(center, radius, planes.data());
}

bool BoxOutside(vec3 boxMin, vec3 boxMax, Planes planes)
{
    return FrustumTestsShader::AabbOutsideFrustum(boxMin, boxMax, planes.data());
}

// The C++ twins of the shader tests take the planes as engine vectors.
std::array<Vector4, 6> ToEnginePlanes(const Planes& planes)
{
    std::array<Vector4, 6> enginePlanes;
    for (int i = 0; i < 6; ++i)
        enginePlanes[i] = Vector4(planes[i].x, planes[i].y, planes[i].z, planes[i].w);
    return enginePlanes;
}

struct VerdictCounts
{
    int Kept = 0;
    int Culled = 0;
    int Mismatched = 0;
};

// Every sphere of an integer grid with each radius, against `planes`.
VerdictCounts CompareSphereVerdicts(const Planes& planes, int gridMin, int gridMax, int gridStep,
                                    std::initializer_list<float> radii)
{
    VerdictCounts counts;
    for (int x = gridMin; x <= gridMax; x += gridStep)
        for (int y = gridMin; y <= gridMax; y += gridStep)
            for (int z = gridMin; z <= gridMax; z += gridStep)
                for (const float radius : radii)
                {
                    const vec3 center(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                    const bool visible = KernelSphereVisible(center, radius, planes);
                    (visible ? counts.Kept : counts.Culled) += 1;
                    if (visible == SphereOutside(center, radius, planes))
                        ++counts.Mismatched;
                }
    return counts;
}

// Every box of an integer grid with each half extent, against `planes`.
VerdictCounts CompareBoxVerdicts(const Planes& planes, int gridMin, int gridMax, int gridStep,
                                 std::initializer_list<vec3> halfExtents)
{
    VerdictCounts counts;
    for (int x = gridMin; x <= gridMax; x += gridStep)
        for (int y = gridMin; y <= gridMax; y += gridStep)
            for (int z = gridMin; z <= gridMax; z += gridStep)
                for (const vec3& halfExtent : halfExtents)
                {
                    const vec3 center(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                    const bool outside = StrictCornerBoxOutside(center - halfExtent, center + halfExtent, planes);
                    (outside ? counts.Culled : counts.Kept) += 1;
                    if (outside != BoxOutside(center - halfExtent, center + halfExtent, planes))
                        ++counts.Mismatched;
                }
    return counts;
}
} // namespace

TEST(FrustumTestsShader, SphereVerdictsEqualTheCullingKernelsOnEveryFixture)
{
    const VerdictCounts perspectiveCounts =
        CompareSphereVerdicts(PerspectiveViewPlanes(), -200, 600, 25, {0.25f, 3.0f, 40.0f});
    const VerdictCounts orthographicCounts =
        CompareSphereVerdicts(OrthographicLightPlanes(), -100, 150, 10, {0.25f, 3.0f, 40.0f});
    // Integer centers and radii put many spheres exactly on the threshold of the cube's planes.
    const VerdictCounts cubeCounts = CompareSphereVerdicts(AxisCubePlanes(), -16, 16, 1, {1.0f, 2.0f, 4.0f});

    for (const VerdictCounts& counts : {perspectiveCounts, orthographicCounts, cubeCounts})
    {
        EXPECT_EQ(counts.Mismatched, 0);
        EXPECT_GT(counts.Kept, 0);
        EXPECT_GT(counts.Culled, 0);
    }
}

TEST(FrustumTestsShader, BoxVerdictsEqualTheStrictCornerPickOnEveryFixture)
{
    const std::initializer_list<vec3> halfExtents = {vec3(1.0f, 1.0f, 1.0f), vec3(2.0f, 4.0f, 1.0f),
                                                     vec3(0.25f, 3.0f, 40.0f)};
    const VerdictCounts perspectiveCounts = CompareBoxVerdicts(PerspectiveViewPlanes(), -200, 600, 25, halfExtents);
    const VerdictCounts orthographicCounts = CompareBoxVerdicts(OrthographicLightPlanes(), -100, 150, 10, halfExtents);
    // The cube's planes and the forward camera's side planes have zero normal components. Integer
    // centers and half extents put many box faces exactly on a cube plane.
    const VerdictCounts cubeCounts = CompareBoxVerdicts(AxisCubePlanes(), -16, 16, 1, halfExtents);
    const VerdictCounts forwardCounts = CompareBoxVerdicts(ForwardCameraPlanes(), -40, 120, 4, halfExtents);

    for (const VerdictCounts& counts : {perspectiveCounts, orthographicCounts, cubeCounts, forwardCounts})
    {
        EXPECT_EQ(counts.Mismatched, 0);
        EXPECT_GT(counts.Kept, 0);
        EXPECT_GT(counts.Culled, 0);
    }
}

TEST(FrustumTestsShader, VerdictsEqualTheCppTwinsOnEveryFixture)
{
    // The C++ sphere test inflates the radius 1.5x; the shader test takes the inflated radius.
    constexpr float kCppSphereInflation = 1.5f;
    const Planes cube = AxisCubePlanes();
    const std::array<Vector4, 6> enginePlanes = ToEnginePlanes(cube);
    int aabbMismatches = 0;
    int sphereMismatches = 0;
    int aabbCulled = 0;
    for (int x = -16; x <= 16; ++x)
        for (int y = -16; y <= 16; y += 2)
            for (int z = -16; z <= 16; z += 2)
            {
                const vec3 center(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                const vec3 boxMin = center - vec3(1.0f, 2.0f, 0.5f);
                const vec3 boxMax = center + vec3(1.0f, 2.0f, 0.5f);
                const bool shaderAabbOutside = BoxOutside(boxMin, boxMax, cube);
                const bool cppAabbInside =
                    GameEngine::Rendering::TestAabbFrustum(Vector3(boxMin.x, boxMin.y, boxMin.z),
                                                           Vector3(boxMax.x, boxMax.y, boxMax.z), enginePlanes.data());
                aabbMismatches += shaderAabbOutside == cppAabbInside ? 1 : 0;
                aabbCulled += shaderAabbOutside ? 1 : 0;

                const bool shaderSphereOutside = SphereOutside(center, 2.0f * kCppSphereInflation, cube);
                const bool cppSphereInside = GameEngine::Rendering::TestSphereFrustum(
                    Vector3(center.x, center.y, center.z), 2.0f, enginePlanes.data());
                sphereMismatches += shaderSphereOutside == cppSphereInside ? 1 : 0;
            }
    EXPECT_EQ(aabbMismatches, 0);
    EXPECT_EQ(sphereMismatches, 0);
    EXPECT_GT(aabbCulled, 0);
}

TEST(FrustumTestsShader, SphereThatTouchesAPlaneIsKept)
{
    const Planes cube = AxisCubePlanes();
    EXPECT_FALSE(SphereOutside(vec3(-12.0f, 0.0f, 0.0f), 2.0f, cube));
    EXPECT_TRUE(SphereOutside(vec3(-12.5f, 0.0f, 0.0f), 2.0f, cube));
}

TEST(FrustumTestsShader, BoxThatStraddlesEachPlaneIsKept)
{
    const Planes cube = AxisCubePlanes();
    for (const vec4& plane : cube)
    {
        // The box's center lies on the plane, so the box reaches both sides of it.
        const vec3 onPlane(-plane.x * 10.0f, -plane.y * 10.0f, -plane.z * 10.0f);
        EXPECT_FALSE(BoxOutside(onPlane - vec3(1.0f, 1.0f, 1.0f), onPlane + vec3(1.0f, 1.0f, 1.0f), cube));
    }
}

TEST(FrustumTestsShader, BoxWhollyBehindOnePlaneIsCulled)
{
    const Planes cube = AxisCubePlanes();
    for (const vec4& plane : cube)
    {
        // Two meters outside the plane along its normal, and inside the other five planes.
        const vec3 outside(-plane.x * 12.0f, -plane.y * 12.0f, -plane.z * 12.0f);
        EXPECT_TRUE(BoxOutside(outside - vec3(1.0f, 1.0f, 1.0f), outside + vec3(1.0f, 1.0f, 1.0f), cube));
    }
}

TEST(FrustumTestsShader, NearPlaneUnderReverseZCullsBetweenTheEyeAndTheNearPlane)
{
    const Planes planes = ForwardCameraPlanes();
    // Reverse-Z: the near plane is row3 - row2, its normal points along +Z, and it lies at z = 0.5 m.
    EXPECT_GT(planes[kNearPlane].z, 0.0f);
    EXPECT_NEAR(planes[kNearPlane].z * 0.5f + planes[kNearPlane].w, 0.0f, 1.0e-5f);

    Planes withoutNear = planes;
    withoutNear[kNearPlane] = kAlwaysInsidePlane;

    const vec3 nearMin(-0.05f, -0.05f, 0.1f);
    const vec3 nearMax(0.05f, 0.05f, 0.4f);
    EXPECT_TRUE(BoxOutside(nearMin, nearMax, planes));
    EXPECT_FALSE(BoxOutside(nearMin, nearMax, withoutNear));
    EXPECT_FALSE(BoxOutside(vec3(-0.05f, -0.05f, 0.3f), vec3(0.05f, 0.05f, 0.7f), planes));

    EXPECT_TRUE(SphereOutside(vec3(0.0f, 0.0f, 0.2f), 0.1f, planes));
    EXPECT_FALSE(SphereOutside(vec3(0.0f, 0.0f, 0.2f), 0.1f, withoutNear));
    EXPECT_FALSE(SphereOutside(vec3(0.0f, 0.0f, 0.2f), 0.4f, planes));
}

TEST(FrustumTestsShader, FarPlaneUnderReverseZCullsBeyondTheFarPlane)
{
    const Planes planes = ForwardCameraPlanes();
    Planes withoutFar = planes;
    withoutFar[kFarPlane] = kAlwaysInsidePlane;

    EXPECT_TRUE(BoxOutside(vec3(-1.0f, -1.0f, 150.0f), vec3(1.0f, 1.0f, 160.0f), planes));
    EXPECT_FALSE(BoxOutside(vec3(-1.0f, -1.0f, 150.0f), vec3(1.0f, 1.0f, 160.0f), withoutFar));
    EXPECT_TRUE(SphereOutside(vec3(0.0f, 0.0f, 150.0f), 10.0f, planes));
    EXPECT_FALSE(SphereOutside(vec3(0.0f, 0.0f, 150.0f), 10.0f, withoutFar));
}
