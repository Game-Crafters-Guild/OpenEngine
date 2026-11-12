#pragma once

#include "Mathematics/MulAdd.h"
#include "Mathematics/Vector3.h"

#include <cmath>
#include <type_traits>

namespace GameEngine {
namespace Mathematics {

/// A direction or an offset in three doubles, for math at planet radius where `Vector3` keeps too few bits.
/// A position is a `WorldPosition`. The conversions to and from `Vector3` are explicit.
struct Vector3F64
{
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;

    constexpr Vector3F64() = default;
    constexpr Vector3F64(double x, double y, double z) : X(x), Y(y), Z(z) {}

    /// The same vector in doubles. Exact: a float widens to a double.
    constexpr explicit Vector3F64(const Vector3& vec) : X(vec.x), Y(vec.y), Z(vec.z) {}

    /// Each component rounded to a float: at 6.4e6 m a float keeps only 0.5 m steps.
    explicit operator Vector3() const
    {
        return Vector3(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
    }

    Vector3F64 operator+(const Vector3F64& other) const { return Vector3F64(X + other.X, Y + other.Y, Z + other.Z); }
    Vector3F64 operator-(const Vector3F64& other) const { return Vector3F64(X - other.X, Y - other.Y, Z - other.Z); }
    Vector3F64 operator*(double scalar) const { return Vector3F64(X * scalar, Y * scalar, Z * scalar); }
    Vector3F64 operator/(double scalar) const { return Vector3F64(X / scalar, Y / scalar, Z / scalar); }

    double Length() const
    {
        return std::sqrt(LengthSquared());
    }

    double LengthSquared() const
    {
        return Dot(*this, *this);
    }

    /// Unit-length copy. A zero vector gives NaN.
    Vector3F64 Normalize() const
    {
        return *this / Length();
    }

    /// The fused form of `Vector3::Dot`: the same bits on every platform.
    static double Dot(const Vector3F64& a, const Vector3F64& b)
    {
        return Math::MulAdd(a.Z, b.Z, Math::MulAdd(a.X, b.X, a.Y * b.Y));
    }

    /// The fused form of `Vector3::Cross`. Left-handed, +Y up: Cross(+X, +Y) is +Z.
    static Vector3F64 Cross(const Vector3F64& a, const Vector3F64& b)
    {
        return Vector3F64(
            Math::MulAdd(a.Y, b.Z, -(a.Z * b.Y)),
            Math::MulAdd(a.Z, b.X, -(a.X * b.Z)),
            Math::MulAdd(a.X, b.Y, -(a.Y * b.X))
        );
    }
};

static_assert(std::is_standard_layout_v<Vector3F64>);
static_assert(std::is_trivially_copyable_v<Vector3F64>);
static_assert(sizeof(Vector3F64) == 3 * sizeof(double));

inline Vector3F64 operator*(double scalar, const Vector3F64& vec)
{
    return vec * scalar;
}

} // namespace Mathematics
} // namespace GameEngine
