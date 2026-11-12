#pragma once

#include "Mathematics/Vector3.h"
#include "Mathematics/WorldPosition.h"
#include "Types/Types.h"

#include <algorithm>
#include <cmath>

/// The exact shape tests of the grid's queries. A mover is a sphere at `p` with the radius `r`. Each test
/// uses add, subtract, multiply, divide, square root and compare on doubles only. IEEE 754 rounds each of
/// these exactly, so every compiler gives the same answer.
namespace GameEngine::SpatialGrid
{

/// Square meters between two positions.
inline float64 DistanceSquared(const Mathematics::WorldPosition& a, const Mathematics::WorldPosition& b)
{
    const float64 dx = b.X - a.X;
    const float64 dy = b.Y - a.Y;
    const float64 dz = b.Z - a.Z;
    return dx * dx + dy * dy + dz * dz;
}

/// True when the sphere at `center` with `radius` overlaps the mover.
inline bool OverlapsSphere(const Mathematics::WorldPosition& center, float64 radius,
                           const Mathematics::WorldPosition& p, float64 r)
{
    const float64 reach = radius + r;
    return DistanceSquared(center, p) <= reach * reach;
}

/// True when the upright cylinder overlaps the mover. The axis is +Y through `center`; `halfHeight` is
/// measured from the center along that axis.
inline bool OverlapsCylinder(const Mathematics::WorldPosition& center, float64 radius, float64 halfHeight,
                             const Mathematics::WorldPosition& p, float64 r)
{
    const float64 dx = p.X - center.X;
    const float64 dz = p.Z - center.Z;
    const float64 across = dx * dx + dz * dz;
    const float64 above = std::max(0.0, std::abs(p.Y - center.Y) - halfHeight);
    if (above > r)
        return false;
    if (above == 0.0)
        return across <= (radius + r) * (radius + r);
    if (across <= radius * radius)
        return true;
    // The rim: (sqrt(across) - radius)^2 <= r^2 - above^2, squared once more to remove the root.
    const float64 slack = r * r - above * above;
    const float64 side = across + radius * radius - slack;
    if (side <= 0.0)
        return true;
    return 4.0 * radius * radius * across >= side * side;
}

/// True when the axis-aligned box overlaps the mover.
inline bool OverlapsBox(const Mathematics::WorldPosition& center, const Mathematics::Vector3& halfExtents,
                        const Mathematics::WorldPosition& p, float64 r)
{
    const float64 ex = std::max(0.0, std::abs(p.X - center.X) - static_cast<float64>(halfExtents.x));
    const float64 ey = std::max(0.0, std::abs(p.Y - center.Y) - static_cast<float64>(halfExtents.y));
    const float64 ez = std::max(0.0, std::abs(p.Z - center.Z) - static_cast<float64>(halfExtents.z));
    return ex * ex + ey * ey + ez * ez <= r * r;
}

/// True when the mover overlaps the solid cone: the points in `range` of the apex and in the half angle
/// of `direction`. `direction` is a unit vector; `cosHalfAngle` and `sinHalfAngle` are of a half angle
/// in (0, pi/2].
inline bool OverlapsCone(const Mathematics::WorldPosition& apex, const Mathematics::Vector3& direction,
                         float64 range, float64 cosHalfAngle, float64 sinHalfAngle,
                         const Mathematics::WorldPosition& p, float64 r)
{
    const float64 reach = range + r;
    const float64 distanceSquared = DistanceSquared(apex, p);
    if (distanceSquared > reach * reach)
        return false;
    // s is the distance along the axis, d the distance from the axis.
    const float64 s = direction.x * (p.X - apex.X) + direction.y * (p.Y - apex.Y) + direction.z * (p.Z - apex.Z);
    if (s >= 0.0 && s * s >= cosHalfAngle * cosHalfAngle * distanceSquared)
        return true;
    const float64 d = std::sqrt(std::max(0.0, distanceSquared - s * s));
    // The nearest point of the cone is on the side line from the apex, at `side` meters from the apex.
    const float64 side = s * cosHalfAngle + d * sinHalfAngle;
    if (side <= 0.0)
        return distanceSquared <= r * r;
    if (side >= range)
    {
        const float64 axial = s - range * cosHalfAngle;
        const float64 radial = d - range * sinHalfAngle;
        return axial * axial + radial * radial <= r * r;
    }
    return d * cosHalfAngle - s * sinHalfAngle <= r;
}

/// True when the sphere of `radius` swept from `start` to `end` overlaps the mover. `alongSquared`
/// receives the squared distance from `start` to the point of the segment nearest the mover's center.
inline bool OverlapsSegment(const Mathematics::WorldPosition& start, const Mathematics::WorldPosition& end,
                            float64 radius, const Mathematics::WorldPosition& p, float64 r, float64& alongSquared)
{
    const float64 dx = end.X - start.X;
    const float64 dy = end.Y - start.Y;
    const float64 dz = end.Z - start.Z;
    const float64 lengthSquared = dx * dx + dy * dy + dz * dz;
    const float64 px = p.X - start.X;
    const float64 py = p.Y - start.Y;
    const float64 pz = p.Z - start.Z;
    float64 t = 0.0;
    if (lengthSquared > 0.0)
        t = std::clamp((px * dx + py * dy + pz * dz) / lengthSquared, 0.0, 1.0);
    const float64 ox = px - t * dx;
    const float64 oy = py - t * dy;
    const float64 oz = pz - t * dz;
    alongSquared = t * t * lengthSquared;
    const float64 reach = radius + r;
    return ox * ox + oy * oy + oz * oz <= reach * reach;
}

} // namespace GameEngine::SpatialGrid
