// Unit tests for the terrain modifier gizmo geometry (pure, read-only footprint
// generation). These lock the two properties the gizmo depends on: the planar
// footprint outline is the modifier's XZ footprint, and the spherical projection
// is the EXACT inverse of the sphere bake's local-coordinate mapping
// (l0 = R*dot(dir, E1)) — so the ring the gizmo draws is the true set of surface
// directions the modifier stack weights, not an approximation.

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "Terrain/TerrainModifier/TerrainModifierGizmoGeometry.h"

namespace Geo = GameEngine::Editor::SceneTools::ModifierGizmoGeometry;

namespace
{
constexpr float kEps = 1e-4f;

using GameEngine::Mathematics::Vector3;

float Dist(const Vector3& a, const Vector3& b)
{
    return (a - b).Length();
}
} // namespace

// ---- planar footprint -------------------------------------------------------

TEST(TerrainModifierGizmoGeometry, PlanarDiscRingPointsLieOnCircle)
{
    std::vector<Vector3> pts;
    const float cx = 10.0f, y = 3.0f, cz = -4.0f, radius = 25.0f;
    Geo::ComputePlanarRingPoints(pts, Geo::FootprintShape::Disc, cx, y, cz, radius, 0.0f, 0.0f, 0.0f,
                                 48u);
    ASSERT_EQ(pts.size(), 48u);
    for (const Vector3& p : pts)
    {
        EXPECT_NEAR(p.y, y, kEps); // planar ring stays at the given height
        const float dx = p.x - cx, dz = p.z - cz;
        EXPECT_NEAR(std::sqrt(dx * dx + dz * dz), radius, 1e-3f);
    }
}

TEST(TerrainModifierGizmoGeometry, PlanarRectFourCornersHonorYaw)
{
    // yaw = 0: corners at (±hx, ±hz) offsets from centre.
    std::vector<Vector3> pts;
    const float cx = 0.0f, cz = 0.0f, hx = 30.0f, hz = 10.0f;
    Geo::ComputePlanarRingPoints(pts, Geo::FootprintShape::Rect, cx, 0.0f, cz, 0.0f, hx, hz, 0.0f,
                                 48u);
    ASSERT_EQ(pts.size(), 4u);
    for (const Vector3& p : pts)
    {
        EXPECT_NEAR(std::abs(p.x), hx, kEps);
        EXPECT_NEAR(std::abs(p.z), hz, kEps);
    }

    // yaw = 90°: the local X axis maps to world Z, so the world extents swap.
    std::vector<Vector3> rot;
    const float halfPi = 1.57079632679489661923f;
    Geo::ComputePlanarRingPoints(rot, Geo::FootprintShape::Rect, cx, 0.0f, cz, 0.0f, hx, hz, halfPi,
                                 48u);
    ASSERT_EQ(rot.size(), 4u);
    for (const Vector3& p : rot)
    {
        EXPECT_NEAR(std::abs(p.x), hz, 1e-3f);
        EXPECT_NEAR(std::abs(p.z), hx, 1e-3f);
    }
}

TEST(TerrainModifierGizmoGeometry, ClosedLoopSegmentsWrapBackToStart)
{
    std::vector<Vector3> pts = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f}};
    std::vector<GameEngine::Mathematics::Vector3> lines;
    Geo::AppendClosedLoopSegments(lines, pts);
    // 3 points -> 3 segments -> 3 * 2 endpoints.
    ASSERT_EQ(lines.size(), 6u);
    // Last segment's "to" is the first point (closed loop).
    EXPECT_NEAR(lines[5].x, 0.0f, kEps);
    EXPECT_NEAR(lines[5].y, 0.0f, kEps);
    EXPECT_NEAR(lines[5].z, 0.0f, kEps);
}

TEST(TerrainModifierGizmoGeometry, PlanarFootprintEmitsClosedLoopLineCount)
{
    std::vector<GameEngine::Mathematics::Vector3> lines;
    Geo::AppendPlanarFootprint(lines, Geo::FootprintShape::Disc, 0.0f, 0.0f, 0.0f, 5.0f, 0.0f, 0.0f,
                               0.0f, 32u);
    EXPECT_EQ(lines.size(), 32u * 2u);
}

// ---- spherical projection ---------------------------------------------------

TEST(TerrainModifierGizmoGeometry, SphereFrameIsOrthonormalAndRadial)
{
    const Vector3 center = {0, 0, 0};
    const Vector3 pos = {100.0f, 250.0f, -70.0f};
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, pos);
    ASSERT_TRUE(f.Valid);

    EXPECT_NEAR(f.N.Length(), 1.0f, kEps);
    EXPECT_NEAR(f.E1.Length(), 1.0f, kEps);
    EXPECT_NEAR(f.E2.Length(), 1.0f, kEps);
    EXPECT_NEAR(Vector3::Dot(f.N, f.E1), 0.0f, kEps);
    EXPECT_NEAR(Vector3::Dot(f.N, f.E2), 0.0f, kEps);
    EXPECT_NEAR(Vector3::Dot(f.E1, f.E2), 0.0f, kEps);

    // N is the unit radial direction from the centre.
    const float len = pos.Length();
    EXPECT_NEAR(f.N.x, pos.x / len, kEps);
    EXPECT_NEAR(f.N.y, pos.y / len, kEps);
    EXPECT_NEAR(f.N.z, pos.z / len, kEps);
}

TEST(TerrainModifierGizmoGeometry, SphereFrameDegenerateAtCentreIsInvalid)
{
    const Vector3 center = {5, 6, 7};
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, center);
    EXPECT_FALSE(f.Valid);
}

TEST(TerrainModifierGizmoGeometry, TangentOffsetInvertsBakeLocalCoords)
{
    // The core correctness property: for a direction produced from tangent-plane
    // offset (l0, l1), the bake reads back l0 = R*dot(dir, E1), l1 = R*dot(dir, E2).
    // If this holds, the gizmo ring coincides with the region the bake weights.
    const Vector3 center = {0, 0, 0};
    const Vector3 pos = {0.0f, 2000.0f, 0.0f};
    const float R = 2000.0f;
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, pos);
    ASSERT_TRUE(f.Valid);

    for (float l0 : {-300.0f, -50.0f, 0.0f, 80.0f, 500.0f})
    {
        for (float l1 : {-200.0f, 0.0f, 120.0f, 400.0f})
        {
            const Vector3 dir = Geo::TangentOffsetToDirection(f, R, l0, l1);
            EXPECT_NEAR(dir.Length(), 1.0f, 1e-4f);
            EXPECT_NEAR(R * Vector3::Dot(dir, f.E1), l0, 0.2f);
            EXPECT_NEAR(R * Vector3::Dot(dir, f.E2), l1, 0.2f);
        }
    }
}

TEST(TerrainModifierGizmoGeometry, SphereProjectionLandsOnBaseSphere)
{
    const Vector3 center = {0, 0, 0};
    const Vector3 pos = {0.0f, 3000.0f, 0.0f};
    const float R = 3000.0f;
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, pos);
    ASSERT_TRUE(f.Valid);

    std::vector<Vector3> ring;
    Geo::ComputeSphereProjectedRingPoints(ring, center, f, R, Geo::FootprintShape::Disc, 200.0f,
                                          0.0f, 0.0f, 0.0f, 48u,
                                          [](float, float, float) { return 0.0f; });
    ASSERT_EQ(ring.size(), 48u);
    for (const auto& p : ring)
        EXPECT_NEAR(Dist(center, p), R, 1e-1f); // every point sits on the base sphere
}

TEST(TerrainModifierGizmoGeometry, SphereProjectionHeightLiftsRadius)
{
    const Vector3 center = {0, 0, 0};
    const Vector3 pos = {1000.0f, 0.0f, 0.0f};
    const float R = 1000.0f;
    const float lift = 37.5f;
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, pos);

    std::vector<Vector3> ring;
    Geo::ComputeSphereProjectedRingPoints(ring, center, f, R, Geo::FootprintShape::Disc, 90.0f,
                                          0.0f, 0.0f, 0.0f, 24u,
                                          [lift](float, float, float) { return lift; });
    ASSERT_EQ(ring.size(), 24u);
    for (const auto& p : ring)
        EXPECT_NEAR(Dist(center, p), R + lift, 1e-1f);
}

TEST(TerrainModifierGizmoGeometry, SphereDiscAngularRadiusMatchesFootprint)
{
    const Vector3 center = {0, 0, 0};
    const Vector3 pos = {0.0f, 5000.0f, 0.0f};
    const float R = 5000.0f;
    const float r = 400.0f;
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, pos);

    std::vector<Vector3> ring;
    Geo::ComputeSphereProjectedRingPoints(ring, center, f, R, Geo::FootprintShape::Disc, r, 0.0f,
                                          0.0f, 0.0f, 48u,
                                          [](float, float, float) { return 0.0f; });
    ASSERT_EQ(ring.size(), 48u);
    // A disc footprint of world radius r subtends angular radius asin(r/R) from N.
    const float expected = std::asin(r / R);
    for (const auto& p : ring)
    {
        const Vector3 dir = p / R;
        const float ang = std::acos(std::clamp(Vector3::Dot(dir, f.N), -1.0f, 1.0f));
        EXPECT_NEAR(ang, expected, 1e-3f);
    }
}

TEST(TerrainModifierGizmoGeometry, SphereOverWideFootprintStaysOnSphere)
{
    // Footprint wider than the sphere hemisphere: the radicand clamps and the ring
    // saturates at the horizon, still exactly on the sphere (unit directions).
    const Vector3 center = {0, 0, 0};
    const Vector3 pos = {0.0f, 100.0f, 0.0f};
    const float R = 100.0f;
    const Geo::SphereFrame f = Geo::MakeSphereFrame(center, pos);

    std::vector<Vector3> ring;
    Geo::ComputeSphereProjectedRingPoints(ring, center, f, R, Geo::FootprintShape::Disc, 500.0f,
                                          0.0f, 0.0f, 0.0f, 32u,
                                          [](float, float, float) { return 0.0f; });
    ASSERT_EQ(ring.size(), 32u);
    for (const auto& p : ring)
        EXPECT_NEAR(Dist(center, p), R, 1e-2f);
}

TEST(TerrainModifierGizmoGeometry, InvalidSphereFrameEmitsNothing)
{
    const Vector3 center = {0, 0, 0};
    Geo::SphereFrame invalid{}; // Valid == false by default
    std::vector<Vector3> ring;
    Geo::ComputeSphereProjectedRingPoints(ring, center, invalid, 1000.0f, Geo::FootprintShape::Disc,
                                          50.0f, 0.0f, 0.0f, 0.0f, 48u,
                                          [](float, float, float) { return 0.0f; });
    EXPECT_TRUE(ring.empty());
}
