#pragma once

#include "Noise/Gradient/GradientNoise2D.h"
#include "Noise/Gradient/GradientNoise3D.h"

#include "Mathematics/MulAdd.h"
#include "Mathematics/WorldPosition.h"
#include "Types/Types.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Noise
{

/// @brief Upper bound on the octave count any fractal sum here will run.
///
/// Callers are expected to clamp authored counts themselves; this is the
/// backstop. The loops run per texel, so an unclamped count from a hand-edited
/// or corrupt source would otherwise turn a bake into a hang.
inline constexpr uint32 kMaxOctaves = 8u;

/// @brief Seed step between octaves, so each octave hashes a decorrelated
/// lattice instead of repeating the previous one at a finer scale.
inline constexpr uint32 kOctaveSeedStride = 31u;

/// @brief Fractional Brownian motion over the gradient-noise lattice.
///
/// Each octave multiplies frequency by @p lacunarity and amplitude by
/// @p persistence. The octave count is clamped to kMaxOctaves.
inline float32 FBMNoise2D(float32 x, float32 z, float32 frequency, float32 amplitude,
                          uint32 octaves, uint32 seed, float32 lacunarity, float32 persistence)
{
    float32 value = 0.0f;
    float32 freq = frequency;
    float32 amp = amplitude;
    const uint32 count = std::min(octaves, kMaxOctaves);
    for (uint32 i = 0; i < count; ++i)
    {
        value = Math::MulAdd(GradientNoise2D(x * freq, z * freq, seed + i * kOctaveSeedStride), amp, value);
        freq *= lacunarity;
        amp *= persistence;
    }
    return value;
}

/// @brief FBMNoise2D carrying the analytic gradient of the sum.
///
/// Value reproduces FBMNoise2D expression for expression, so the two agree bit
/// for bit; the derivatives accumulate through the same octave loop.
inline NoiseSample FBMNoise2DWithDerivatives(float32 x, float32 z, float32 frequency,
                                             float32 amplitude, uint32 octaves, uint32 seed,
                                             float32 lacunarity, float32 persistence)
{
    NoiseSample out{};
    float32 freq = frequency;
    float32 amp = amplitude;
    const uint32 count = std::min(octaves, kMaxOctaves);
    for (uint32 i = 0; i < count; ++i)
    {
        const NoiseSample n =
            GradientNoise2DWithDerivatives(x * freq, z * freq, seed + i * kOctaveSeedStride);
        out.Value = Math::MulAdd(n.Value, amp, out.Value);
        // Chain rule through the octave's frequency scaling of x and z.
        out.DValueDX = Math::MulAdd(n.DValueDX * freq, amp, out.DValueDX);
        out.DValueDZ = Math::MulAdd(n.DValueDZ * freq, amp, out.DValueDZ);
        freq *= lacunarity;
        amp *= persistence;
    }
    return out;
}

/// @brief Upper bound on the octave count of FractalNoise3D: the finest cells are 2^25 times smaller than
/// the top cells (8,389 km down to 0.25 m at a top frequency of 2^-23 cells per meter).
inline constexpr uint32 kMaxFractalNoise3DOctaves = 26u;

/// @brief Fractional Brownian motion of 3D gradient noise at an exact position, with its gradient per meter.
/// Octave k has `topFrequency * 2^k` cells per meter, amplitude `gain^k` and seed `seed + k * kOctaveSeedStride`; a power-of-two `topFrequency` keeps each split exact, so fewer octaves leave the coarser ones bit-identical.
/// A preset with Hurst exponent H passes `gain = 2^-H`. The count is clamped to kMaxFractalNoise3DOctaves; a non-finite position gives zero.
/// The value is not normalized: one octave's |value| is below 1, so |Value| is at most the sum of gain^k over the octaves.
/// Precondition: each axis of `position * topFrequency * 2^(octaves - 1)` is under 2^62 cells; past that, the cell saturates at the lattice edge and the value is finite but is not noise.
inline NoiseSample3D FractalNoise3D(const Mathematics::WorldPosition& position, float64 topFrequency,
                                    uint32 octaves, float32 gain, uint32 seed)
{
    if (!std::isfinite(position.X) || !std::isfinite(position.Y) || !std::isfinite(position.Z))
        return {};
    NoiseSample3D out;
    float64 frequency = topFrequency;
    float32 amplitude = 1.0f;
    const uint32 count = std::min(octaves, kMaxFractalNoise3DOctaves);
    for (uint32 octave = 0; octave < count; ++octave)
    {
        const NoiseSample3D sample = GradientNoise3D(NoiseLatticePoint3D::FromPosition(position, frequency),
                                                     seed + octave * kOctaveSeedStride);
        out.Value = Math::MulAdd(sample.Value, amplitude, out.Value);
        // Chain rule: one cell is 1 / frequency meters.
        const float32 slopeScale = amplitude * static_cast<float32>(frequency);
        for (uint32 axis = 0; axis < 3; ++axis)
            out.Gradient[axis] = Math::MulAdd(sample.Gradient[axis], slopeScale, out.Gradient[axis]);
        frequency *= 2.0;
        amplitude *= gain;
    }
    return out;
}

} // namespace GameEngine::Noise
