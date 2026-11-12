#pragma once

#include <cmath>
#include <numbers>

namespace GameEngine::Mathematics
{

/// An angle in radians. A script API declaration spells an angle with this alias, so every
/// binding knows the value is an angle: C++ and C# keep radians, and the public TypeScript API
/// takes degrees.
using Radians = float;

/// Degrees to radians.
constexpr Radians ToRadians(float degrees) noexcept { return degrees * (std::numbers::pi_v<float> / 180.0f); }

/// Radians to degrees.
constexpr float ToDegrees(Radians radians) noexcept { return radians * static_cast<float>(180.0 / std::numbers::pi); }

/// The angle wrapped into one turn, in radians, in the range [-pi, pi). The remainder is exact, so
/// an angle that is already in the range comes back unchanged.
inline Radians WrapAngle(Radians radians) noexcept
{
    constexpr Radians kTurn = 2.0f * std::numbers::pi_v<float>;
    const Radians wrapped = std::remainder(radians, kTurn);
    return wrapped >= std::numbers::pi_v<float> ? wrapped - kTurn : wrapped;
}

/// The shortest signed turn from `from` to `to`, in radians, in the range [-pi, pi). A positive
/// value turns in the direction of increasing angle.
inline Radians SignedAngleDifference(Radians from, Radians to) noexcept
{
    return WrapAngle(to - from);
}

} // namespace GameEngine::Mathematics
