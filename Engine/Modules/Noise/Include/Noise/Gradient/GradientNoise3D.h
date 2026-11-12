#pragma once

#include "Mathematics/Interpolation.h"
#include "Mathematics/MulAdd.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/WorldPosition.h"
#include "Types/Fnv1a.h"
#include "Types/Types.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

// ---- 3D gradient noise on an exact lattice ----------------------------------
//
// Twelve edge-midpoint gradients, a quintic fade, and an integer hash of the full
// 64-bit cell, so the lattice has no period. The input is a 64-bit cell and an fp32
// fraction, which keeps the phase exact at planet distances.

namespace GameEngine::Noise
{

/// @brief A 3D noise sample with its analytic gradient, per unit of the input the call took.
struct NoiseSample3D
{
    float32 Value = 0.0f;
    Mathematics::Vector3 Gradient;
};

/// @brief A point on the noise lattice: the integer cell and the fraction in it, in [0, 1] per axis.
struct NoiseLatticePoint3D
{
    int64 CellX = 0;
    int64 CellY = 0;
    int64 CellZ = 0;
    Mathematics::Vector3 Fraction;

    /// The point `position * cellsPerMeter`, split in double. The fraction rounds once to fp32.
    /// A power-of-two `cellsPerMeter` makes the product exact, so every exact path to one point
    /// gives the same cell and fraction. Each axis of the product must be finite and is meant to be
    /// under 2^62 cells; an axis past that saturates to the cell +/-2^62 with fraction 0.
    static NoiseLatticePoint3D FromPosition(const Mathematics::WorldPosition& position, float64 cellsPerMeter);
};

namespace GradientNoise3DDetail
{
inline void SplitAxis(float64 cells, int64& outCell, float32& outFraction)
{
    assert(std::isfinite(cells) && "NoiseLatticePoint3D: the point is not finite");
    // The clamp keeps the cast and the neighbor cell (+1) inside int64.
    const float64 onLattice = std::clamp(cells, -0x1p62, 0x1p62);
    const float64 whole = std::floor(onLattice);
    outCell = static_cast<int64>(whole);
    outFraction = static_cast<float32>(onLattice - whole);
}

/// One link of the lattice hash: folds a 64-bit cell coordinate into the running hash.
inline uint64 HashStep(uint64 hash, int64 cell)
{
    return Hashing::SplitMix64(hash ^ static_cast<uint64>(cell));
}

/// Value and gradient, per lattice cell, of the noise in one cell. Corner c is the upper corner
/// along axis a when bit a of c is set; its hash selects the gradient. Every 3D gradient noise in
/// this module evaluates its cell here.
inline NoiseSample3D EvaluateCell(const std::array<uint32, 8>& cornerHashes, const Mathematics::Vector3& fraction)
{
    Mathematics::Vector3 fade, fadeSlope;
    for (uint32 axis = 0; axis < 3; ++axis)
    {
        const float32 t = fraction[axis];
        fade[axis] = Math::SmootherStep(0.0f, 1.0f, t);
        fadeSlope[axis] = 30.0f * t * t * (t - 1.0f) * (t - 1.0f);
    }
    NoiseSample3D result;
    for (uint32 corner = 0; corner < 8; ++corner)
    {
        Mathematics::Vector3 delta, weights, slopes;
        for (uint32 axis = 0; axis < 3; ++axis)
        {
            const uint32 upper = (corner >> axis) & 1u;
            delta[axis] = fraction[axis] - static_cast<float32>(upper);
            weights[axis] = upper ? fade[axis] : 1.0f - fade[axis];
            slopes[axis] = upper ? fadeSlope[axis] : -fadeSlope[axis];
        }
        // One of the twelve edge-midpoint gradients: a zero axis picked by the hash, signs by its bits.
        const uint32 hash = cornerHashes[corner];
        const uint32 absent = hash % 3u;
        uint32 sign = hash >> 2;
        Mathematics::Vector3 gradient;
        for (uint32 axis = 0; axis < 3; ++axis)
            if (axis != absent)
            {
                gradient[axis] = (sign & 1u) ? 1.0f : -1.0f;
                sign >>= 1;
            }
        const float32 dot = Mathematics::Vector3::Dot(gradient, delta);
        const float32 weight = weights[0] * weights[1] * weights[2];
        result.Value = Math::MulAdd(weight, dot, result.Value);
        for (uint32 axis = 0; axis < 3; ++axis)
        {
            float32 weightSlope = slopes[axis];
            for (uint32 other = 0; other < 3; ++other)
                if (other != axis)
                    weightSlope *= weights[other];
            result.Gradient[axis] += Math::MulAdd(gradient[axis], weight, dot * weightSlope);
        }
    }
    return result;
}
} // namespace GradientNoise3DDetail

inline NoiseLatticePoint3D NoiseLatticePoint3D::FromPosition(const Mathematics::WorldPosition& position,
                                                             float64 cellsPerMeter)
{
    NoiseLatticePoint3D point;
    GradientNoise3DDetail::SplitAxis(position.X * cellsPerMeter, point.CellX, point.Fraction.x);
    GradientNoise3DDetail::SplitAxis(position.Y * cellsPerMeter, point.CellY, point.Fraction.y);
    GradientNoise3DDetail::SplitAxis(position.Z * cellsPerMeter, point.CellZ, point.Fraction.z);
    return point;
}

/// @brief One octave of 3D gradient noise at an exact lattice point, with its gradient per cell.
///
/// The value is not normalized; its magnitude is below 1. The gradient is per lattice cell. The hash
/// reads all 64 bits of each cell, so the field does not repeat. A sample is a pure function of the
/// point and the seed, the same bits on every platform.
inline NoiseSample3D GradientNoise3D(const NoiseLatticePoint3D& point, uint32 seed)
{
    using GradientNoise3DDetail::HashStep;
    const uint64 seedHash = Hashing::SplitMix64(seed);
    const std::array<uint64, 2> hashX = {HashStep(seedHash, point.CellX), HashStep(seedHash, point.CellX + 1)};
    std::array<uint64, 4> hashXY;
    for (uint32 corner = 0; corner < 4; ++corner)
        hashXY[corner] = HashStep(hashX[corner & 1u], point.CellY + (corner >> 1));
    std::array<uint32, 8> cornerHashes;
    for (uint32 corner = 0; corner < 8; ++corner)
        cornerHashes[corner] = static_cast<uint32>(HashStep(hashXY[corner & 3u], point.CellZ + (corner >> 2)) >> 32);
    return GradientNoise3DDetail::EvaluateCell(cornerHashes, point.Fraction);
}

} // namespace GameEngine::Noise
