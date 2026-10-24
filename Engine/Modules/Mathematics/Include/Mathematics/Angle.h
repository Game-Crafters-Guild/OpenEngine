#pragma once

#include <numbers>

namespace GameEngine::Mathematics
{

/// An angle in radians. A script API declaration spells an angle with this alias, so every
/// binding knows the value is an angle: C++ and C# keep radians, and the public TypeScript API
/// takes degrees.
using Radians = float;

/// Degrees to radians.
constexpr Radians ToRadians(float degrees) noexcept { return degrees * (std::numbers::pi_v<float> / 180.0f); }

} // namespace GameEngine::Mathematics
