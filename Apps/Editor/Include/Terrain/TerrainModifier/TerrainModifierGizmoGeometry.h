#pragma once

// Pure geometry for terrain-modifier volume gizmos (display-only, read-only).
//
// Produces world-space line-list outlines for a modifier's footprint on planar
// AND spherical (planet) terrains. There are no ECS / render / service
// dependencies here — the gizmo class feeds these outlines to
// GizmoRenderContext. Kept header-only and dependency-light so it is unit-
// testable in isolation, which is also what enforces the #530 lesson: gizmo
// geometry is a pure function of the modifier's parameters and can never reach
// the modifier system's bake / dirty path.
//
// The spherical projection mirrors TerrainModifierSystem's sphere bake exactly
// (MakeSpherePlacement / EvaluateSpherePlacement in TerrainModifierSystem.cpp):
// centre direction N = normalize(worldPos - centre), tangent frame
// E1 = AnyTangent(N), E2 = cross(N, E1), and local tangent-plane metres
// l0 = R*dot(dir, E1), l1 = R*dot(dir, E2). Reusing AnyTangent (the same helper
// the bake uses) means the ring the gizmo draws is the true set of surface
// directions the bake weights — "what it affects", not an approximation.

#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

#include "CBTTerrain/CBTPlanetShading.h" // AnyTangent — the tangent frame the sphere bake uses
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"

namespace GameEngine::Editor::SceneTools::ModifierGizmoGeometry
{

// The footprint outline the modifier stack weights against. Disc = circle
// (Radius); Rect = axis-aligned rectangle (RectHalfX/Z) rotated about +Y by yaw.
// Spline modifiers are drawn from their resolved path by the gizmo, not here.
enum class FootprintShape : std::uint8_t
{
    Disc,
    Rect,
};

// Ordered footprint-local offsets (l0, l1) in metres, in the modifier's
// footprint plane. For a planar terrain these are world XZ offsets from the
// modifier position; for a sphere they are tangent-plane metres about the
// modifier direction. Shared by both projections so the shape convention has a
// single source of truth and matches the bake's ComputeWeight / SphereShapeWeight
// rotation exactly (rect corners are R(yaw) * (±halfX, ±halfZ)).
inline void ComputeFootprintLocalRing(std::vector<Mathematics::Vector2>& out,
                                      FootprintShape shape, float radius, float rectHalfX,
                                      float rectHalfZ, float yawRadians, std::uint32_t segments)
{
    out.clear();
    if (shape == FootprintShape::Rect)
    {
        const float c = std::cos(yawRadians);
        const float s = std::sin(yawRadians);
        const Mathematics::Vector2 corners[] = {
            {rectHalfX, rectHalfZ},
            {-rectHalfX, rectHalfZ},
            {-rectHalfX, -rectHalfZ},
            {rectHalfX, -rectHalfZ},
        };
        out.reserve(std::size(corners));
        for (const Mathematics::Vector2& corner : corners)
            out.emplace_back(c * corner.x - s * corner.y, s * corner.x + c * corner.y);
        return;
    }

    const std::uint32_t count = segments < 3u ? 3u : segments;
    out.reserve(count);
    constexpr float kTwoPi = 6.28318530717958647692f;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(count);
        out.emplace_back(radius * std::cos(a), radius * std::sin(a));
    }
}

// Append a closed loop of line segments (consecutive from, to endpoint pairs, the
// layout GizmoRenderContext::DrawColoredLines consumes) connecting `points` in
// order, with the last point joined back to the first.
inline void AppendClosedLoopSegments(std::vector<Mathematics::Vector3>& out,
                                     const std::vector<Mathematics::Vector3>& points)
{
    const std::size_t n = points.size();
    if (n < 2u)
        return;
    out.reserve(out.size() + n * 2u);
    for (std::size_t i = 0; i < n; ++i)
    {
        out.push_back(points[i]);
        out.push_back(points[(i + 1u) % n]);
    }
}

// ---- planar footprint outline (XZ plane) -----------------------------------

// Ordered world-space ring points of a planar footprint at height `y`. The
// footprint-local offset (l0, l1) maps directly to a world XZ offset from the
// modifier position, matching ComputeModifierBounds' rect rotation.
inline void ComputePlanarRingPoints(std::vector<Mathematics::Vector3>& out, FootprintShape shape,
                                    float centerX, float y, float centerZ, float radius,
                                    float rectHalfX, float rectHalfZ, float yawRadians,
                                    std::uint32_t segments)
{
    std::vector<Mathematics::Vector2> local;
    ComputeFootprintLocalRing(local, shape, radius, rectHalfX, rectHalfZ, yawRadians, segments);
    out.clear();
    out.reserve(local.size());
    for (const Mathematics::Vector2& p : local)
        out.emplace_back(centerX + p.x, y, centerZ + p.y);
}

// Closed planar footprint outline as a DrawColoredLines-ready line list.
inline void AppendPlanarFootprint(std::vector<Mathematics::Vector3>& out, FootprintShape shape, float centerX,
                                  float y, float centerZ, float radius, float rectHalfX,
                                  float rectHalfZ, float yawRadians, std::uint32_t segments)
{
    std::vector<Mathematics::Vector3> pts;
    ComputePlanarRingPoints(pts, shape, centerX, y, centerZ, radius, rectHalfX, rectHalfZ,
                            yawRadians, segments);
    AppendClosedLoopSegments(out, pts);
}

// ---- spherical (planet) projection -----------------------------------------

// Radial tangent frame at a modifier direction, matching MakeSpherePlacement.
struct SphereFrame
{
    Mathematics::Vector3 N{0.0f, 1.0f, 0.0f};  // centre direction = normalize(worldPos - centre)
    Mathematics::Vector3 E1{1.0f, 0.0f, 0.0f}; // AnyTangent(N) — local footprint X axis
    Mathematics::Vector3 E2{0.0f, 0.0f, 1.0f}; // cross(N, E1) — local footprint Z axis
    bool Valid = false;                        // false when the modifier sits on the planet centre
};

inline SphereFrame MakeSphereFrame(const Mathematics::Vector3& planetCenter,
                                   const Mathematics::Vector3& modifierWorldPos)
{
    SphereFrame f{};
    const Mathematics::Vector3 radial = modifierWorldPos - planetCenter;
    const float len = std::sqrt(radial.x * radial.x + radial.y * radial.y + radial.z * radial.z);
    if (len <= 0.0f)
        return f; // no radial direction at the centre
    f.N = radial / len;
    f.E1 = CBTTerrain::AnyTangent(f.N);
    f.E2 = {f.N.y * f.E1.z - f.N.z * f.E1.y, f.N.z * f.E1.x - f.N.x * f.E1.z,
            f.N.x * f.E1.y - f.N.y * f.E1.x};
    f.Valid = true;
    return f;
}

// Unit surface direction for a tangent-plane local offset (l0, l1) metres:
// dir = N*sqrt(1 - (l0^2+l1^2)/R^2) + E1*(l0/R) + E2*(l1/R). This is the exact
// inverse of the bake's l0 = R*dot(dir, E1). A footprint wider than the
// hemisphere saturates at the horizon (radicand clamped at 0); the result is
// renormalized so an over-wide footprint stays a unit direction.
inline Mathematics::Vector3 TangentOffsetToDirection(const SphereFrame& f, float radius, float l0, float l1)
{
    const float invR = radius > 0.0f ? 1.0f / radius : 0.0f;
    const float a = l0 * invR;
    const float b = l1 * invR;
    const float rad = 1.0f - a * a - b * b;
    const float n = rad > 0.0f ? std::sqrt(rad) : 0.0f;
    const Mathematics::Vector3 dir = {f.N.x * n + f.E1.x * a + f.E2.x * b, f.N.y * n + f.E1.y * a + f.E2.y * b,
                                      f.N.z * n + f.E1.z * a + f.E2.z * b};
    const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    const float dinv = dl > 0.0f ? 1.0f / dl : 0.0f;
    return {dir.x * dinv, dir.y * dinv, dir.z * dinv};
}

// Ordered world-space points of a footprint projected onto the sphere of
// `radius` centred at `planetCenter`. `heightAbove(dx, dy, dz)` lifts each point
// off the base sphere onto the visible relief / sculpt surface; pass a functor
// returning 0 for the base sphere. Each point is
// planetCenter + dir * (radius + heightAbove(dir)).
template <typename HeightFn>
inline void ComputeSphereProjectedRingPoints(std::vector<Mathematics::Vector3>& out,
                                             const Mathematics::Vector3& planetCenter,
                                             const SphereFrame& frame, float radius,
                                             FootprintShape shape, float footprintRadius,
                                             float rectHalfX, float rectHalfZ, float yawRadians,
                                             std::uint32_t segments, HeightFn&& heightAbove)
{
    out.clear();
    if (!frame.Valid || radius <= 0.0f)
        return;
    std::vector<Mathematics::Vector2> local;
    ComputeFootprintLocalRing(local, shape, footprintRadius, rectHalfX, rectHalfZ, yawRadians,
                              segments);
    out.reserve(local.size());
    for (const Mathematics::Vector2& p : local)
    {
        const Mathematics::Vector3 dir = TangentOffsetToDirection(frame, radius, p.x, p.y);
        const float h = static_cast<float>(heightAbove(dir.x, dir.y, dir.z));
        out.push_back(planetCenter + dir * (radius + h));
    }
}

// Closed spherical footprint outline as a DrawColoredLines-ready line list.
template <typename HeightFn>
inline void AppendSphereFootprint(std::vector<Mathematics::Vector3>& out,
                                  const Mathematics::Vector3& planetCenter, const SphereFrame& frame,
                                  float radius, FootprintShape shape, float footprintRadius,
                                  float rectHalfX, float rectHalfZ, float yawRadians,
                                  std::uint32_t segments, HeightFn&& heightAbove)
{
    std::vector<Mathematics::Vector3> pts;
    ComputeSphereProjectedRingPoints(pts, planetCenter, frame, radius, shape, footprintRadius,
                                     rectHalfX, rectHalfZ, yawRadians, segments,
                                     std::forward<HeightFn>(heightAbove));
    AppendClosedLoopSegments(out, pts);
}

} // namespace GameEngine::Editor::SceneTools::ModifierGizmoGeometry
