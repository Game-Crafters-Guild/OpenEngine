#pragma once

#include "Mathematics/Vector3.h"
#include "Mathematics/Vector3Int.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace GameEngine {
namespace Mathematics {

/// Meters along each axis of one world sector, the cell of the split to a 32-bit position.
inline constexpr double kWorldSectorSize = 1024.0;

namespace WorldPositionDetail {

// The split of one axis: sector = floor(meters / kWorldSectorSize + 0.5), local in [-512, 512).
// A local that rounds up to +512 in 32 bits moves to the next sector as -512. The sector is a double.
inline double SplitAxisSector(double meters, float& outLocal)
{
    constexpr float kSectorSize = static_cast<float>(kWorldSectorSize);
    constexpr float kHalfSector = 0.5f * kSectorSize;
    double sector = std::floor(meters / kWorldSectorSize + 0.5);
    float local = static_cast<float>(meters - sector * kWorldSectorSize);
    if (local >= kHalfSector)
    {
        sector += 1.0;
        local -= kSectorSize;
    }
    outLocal = local;
    return sector;
}

inline int32_t SplitAxis(double meters, float& outLocal)
{
    const double sector = SplitAxisSector(meters, outLocal);
    assert(sector > static_cast<double>(INT32_MIN) && sector < static_cast<double>(INT32_MAX) &&
           "WorldPosition::ToSector: the position is outside the int32 sector range");
    return static_cast<int32_t>(sector);
}

// The sector of SplitAxis, clamped to [minAxis, maxAxis].
inline int32_t SplitAxisClamped(double meters, int32_t minAxis, int32_t maxAxis)
{
    float local = 0.0f;
    const double sector = SplitAxisSector(meters, local);
    return static_cast<int32_t>(std::clamp(sector, static_cast<double>(minAxis), static_cast<double>(maxAxis)));
}

} // namespace WorldPositionDetail

/// An exact world position in meters, three doubles: the spacing is 0.12 mm at 1e12 m.
/// It has no implicit conversion to or from `Vector3`, and two positions do not add.
struct WorldPosition
{
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;

    /// The same point as a position. Exact: a float widens to a double.
    static WorldPosition FromVector3(const Vector3& meters)
    {
        return WorldPosition{meters.x, meters.y, meters.z};
    }

    /// The point `sector * kWorldSectorSize + local`. Exact for a local in [-512, 512) whose
    /// smallest bit is not below the spacing of a double at the position.
    static WorldPosition FromSector(const Vector3Int& sector, const Vector3& local)
    {
        return WorldPosition{static_cast<double>(sector.x) * kWorldSectorSize + local.x,
                             static_cast<double>(sector.y) * kWorldSectorSize + local.y,
                             static_cast<double>(sector.z) * kWorldSectorSize + local.z};
    }

    /// The one split: the sector is floor(x / kWorldSectorSize + 0.5) and the local is in [-512, 512),
    /// within 2^-16 m of the position. Compare composed positions, never the sectors of two splits.
    /// Each axis must be inside the int32 sector range (2.2e12 m); an assert stops a position outside it.
    void ToSector(Vector3Int& outSector, Vector3& outLocal) const
    {
        outSector = Vector3Int(WorldPositionDetail::SplitAxis(X, outLocal.x), WorldPositionDetail::SplitAxis(Y, outLocal.y),
                               WorldPositionDetail::SplitAxis(Z, outLocal.z));
    }

    /// The sector of ToSector with each axis clamped to [minAxis, maxAxis], for a packed sector range.
    Vector3Int ToSectorClamped(int32_t minAxis, int32_t maxAxis) const
    {
        return Vector3Int(WorldPositionDetail::SplitAxisClamped(X, minAxis, maxAxis),
                          WorldPositionDetail::SplitAxisClamped(Y, minAxis, maxAxis),
                          WorldPositionDetail::SplitAxisClamped(Z, minAxis, maxAxis));
    }

    /// The offset from `b` to `a`: subtracts in double and rounds once, within 1 cm while |a - b| < 131 km.
    /// `p - WorldPosition{}` is the marked narrowing to the origin: past 131 km it is outside this bound.
    friend Vector3 operator-(const WorldPosition& a, const WorldPosition& b)
    {
        return Vector3(static_cast<float>(a.X - b.X), static_cast<float>(a.Y - b.Y), static_cast<float>(a.Z - b.Z));
    }

    /// The position moved by `offset`, added in double.
    friend WorldPosition operator+(const WorldPosition& p, const Vector3& offset)
    {
        return WorldPosition{p.X + offset.x, p.Y + offset.y, p.Z + offset.z};
    }

    friend bool operator==(const WorldPosition&, const WorldPosition&) = default;
};
static_assert(std::is_standard_layout_v<WorldPosition>);
static_assert(std::is_trivially_copyable_v<WorldPosition>);
static_assert(sizeof(WorldPosition) == 3 * sizeof(double));

} // namespace Mathematics
} // namespace GameEngine
