#pragma once

#include "Mathematics/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace GameEngine {
namespace Mathematics {

/// A 3D vector of 32-bit signed integers, for exact lattice points such as cube corners.
/// The components are plain members, not a union, so constant expressions can read them by index.
struct Vector3Int
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    constexpr Vector3Int() = default;
    constexpr Vector3Int(int32_t x_, int32_t y_, int32_t z_) : x(x_), y(y_), z(z_) {}

    /// Component by index: 0 is `x`, 1 is `y`, 2 is `z`. The caller keeps the index below 3.
    constexpr int32_t operator[](std::size_t index) const { return index == 0 ? x : (index == 1 ? y : z); }

    constexpr bool operator==(const Vector3Int& other) const = default;

    constexpr Vector3Int operator+(const Vector3Int& other) const { return Vector3Int(x + other.x, y + other.y, z + other.z); }

    /// Each component divided by `divisor`, rounded toward zero.
    constexpr Vector3Int operator/(int32_t divisor) const { return Vector3Int(x / divisor, y / divisor, z / divisor); }

    /// The same point as a float vector.
    constexpr explicit operator Vector3() const
    {
        return Vector3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    }
};

static_assert(std::is_standard_layout_v<Vector3Int>);
static_assert(sizeof(Vector3Int) == 3 * sizeof(int32_t));

} // namespace Mathematics
} // namespace GameEngine
