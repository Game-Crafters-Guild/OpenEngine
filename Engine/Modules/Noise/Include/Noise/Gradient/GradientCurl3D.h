#pragma once

#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector3.h"
#include "Noise/Gradient/GradientNoise3D.h"

#include <array>
#include <cmath>
#include <cstdint>

// ---- Curl of 3D gradient noise ----------------------------------------------
//
// A divergence-free vector field for turbulence (particles, wind): the curl of
// three seeded gradient-noise potentials, one per axis. The potentials use
// quintic fade and an integer hash per lattice corner; their analytic
// derivatives give the curl without finite differences. A sample is a pure
// function of the point and the seed.

namespace GameEngine::Noise
{

namespace GradientCurl3DDetail
{
/// Lattice coordinates wrap every this many cells, which keeps the float-to-integer
/// conversion defined at any distance from the origin.
inline constexpr float kPeriod = 256.0f;

inline std::uint32_t Hash(std::uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

/// The gradient of one seeded potential at `point`, in lattice units.
inline Mathematics::Vector3 PotentialGradient(const Mathematics::Vector3& point, std::uint32_t seed)
{
    constexpr std::uint32_t kAxisPrimes[3] = {0x9e3779b9u, 0x85ebca6bu, 0xc2b2ae35u};
    std::uint32_t cell[3]{};
    Mathematics::Vector3 fraction;
    for (std::uint32_t axis = 0; axis < 3; ++axis)
    {
        const float wrapped = Math::Wrap(point[axis], kPeriod);
        const float whole = std::floor(wrapped);
        cell[axis] = static_cast<std::uint32_t>(whole);
        fraction[axis] = wrapped - whole;
    }
    std::array<std::uint32_t, 8> cornerHashes;
    for (std::uint32_t corner = 0; corner < 8; ++corner)
    {
        std::uint32_t hash = seed;
        for (std::uint32_t axis = 0; axis < 3; ++axis)
            hash ^= ((cell[axis] + ((corner >> axis) & 1u)) & 255u) * kAxisPrimes[axis];
        cornerHashes[corner] = Hash(hash);
    }
    return GradientNoise3DDetail::EvaluateCell(cornerHashes, fraction).Gradient;
}
} // namespace GradientCurl3DDetail

/// The curl of three seeded gradient-noise potentials at `point`, in lattice units: one lattice
/// cell per unit, so a caller sampling a world position scales it by the field's frequency first.
/// The field is divergence-free, continuous across the 256-cell wrap, and its magnitude does not
/// depend on the frequency the caller chose. A non-finite point gives zero.
inline Mathematics::Vector3 GradientCurl3D(const Mathematics::Vector3& point, std::uint32_t seed)
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
        return {};
    Mathematics::Vector3 potential[3];
    for (std::uint32_t component = 0; component < 3; ++component)
    {
        // Each potential reads the point with its axes rotated, so the three are independent.
        Mathematics::Vector3 rotated;
        for (std::uint32_t axis = 0; axis < 3; ++axis)
            rotated[axis] = point[(component + axis + 1) % 3];
        const auto gradient = GradientCurl3DDetail::PotentialGradient(
            rotated, GradientCurl3DDetail::Hash(seed + component * 0x9e3779b9u));
        for (std::uint32_t axis = 0; axis < 3; ++axis)
            potential[component][(component + axis + 1) % 3] = gradient[axis];
    }
    return {potential[2][1] - potential[1][2], potential[0][2] - potential[2][0], potential[1][0] - potential[0][1]};
}

} // namespace GameEngine::Noise
