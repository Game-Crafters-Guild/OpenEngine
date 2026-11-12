#include "Noise/FractalNoise.h"
#include "Noise/Gradient/GradientNoise3D.h"

#include "NoiseGoldenSweep.h"

#include "Mathematics/Vector3Int.h"
#include "Mathematics/WorldPosition.h"
#include "Types/Fnv1a.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

using GameEngine::Mathematics::Vector3;
using GameEngine::Mathematics::Vector3Int;
using GameEngine::Mathematics::WorldPosition;
using namespace GameEngine::Noise;

namespace
{

// The radius of the Earth, so the samples sit where a planet needs the lattice.
constexpr double kPlanetRadius = 6371000.0;

struct ReferenceSample
{
    double Value = 0.0;
    double Gradient[3] = {};
};

// The lattice hash written out from its definition: SplitMix64 of the seed, then of each 64-bit cell in turn.
uint32_t ReferenceCornerHash(const int64_t cell[3], uint32_t seed)
{
    uint64_t hash = GameEngine::Hashing::SplitMix64(seed);
    for (std::size_t axis = 0; axis < 3; ++axis)
        hash = GameEngine::Hashing::SplitMix64(hash ^ static_cast<uint64_t>(cell[axis]));
    return static_cast<uint32_t>(hash >> 32);
}

// One octave in double, per lattice cell: the same corners and gradients, an independent fade, dot and sum.
ReferenceSample ReferenceOctave(const int64_t cell[3], const double fraction[3], uint32_t seed)
{
    ReferenceSample out;
    for (int corner = 0; corner < 8; ++corner)
    {
        int64_t cornerCell[3];
        double delta[3], weight[3], slope[3];
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const int upper = (corner >> axis) & 1;
            const double t = fraction[axis];
            const double fade = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
            const double fadeSlope = 30.0 * t * t * (t - 1.0) * (t - 1.0);
            cornerCell[axis] = cell[axis] + upper;
            delta[axis] = t - upper;
            weight[axis] = upper ? fade : 1.0 - fade;
            slope[axis] = upper ? fadeSlope : -fadeSlope;
        }
        const uint32_t hash = ReferenceCornerHash(cornerCell, seed);
        double gradient[3] = {};
        uint32_t sign = hash >> 2;
        for (uint32_t axis = 0; axis < 3; ++axis)
            if (axis != hash % 3u)
            {
                gradient[axis] = (sign & 1u) ? 1.0 : -1.0;
                sign >>= 1;
            }
        const double dot = gradient[0] * delta[0] + gradient[1] * delta[1] + gradient[2] * delta[2];
        const double product = weight[0] * weight[1] * weight[2];
        out.Value += product * dot;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const double others = weight[(axis + 1) % 3] * weight[(axis + 2) % 3];
            out.Gradient[axis] += gradient[axis] * product + dot * slope[axis] * others;
        }
    }
    return out;
}

ReferenceSample ReferenceFractal(const WorldPosition& position, double topFrequency, uint32_t octaves, double gain,
                                 uint32_t seed)
{
    ReferenceSample out;
    const double meters[3] = {position.X, position.Y, position.Z};
    double frequency = topFrequency;
    double amplitude = 1.0;
    for (uint32_t octave = 0; octave < octaves; ++octave)
    {
        int64_t cell[3];
        double fraction[3];
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const double cells = meters[axis] * frequency;
            cell[axis] = static_cast<int64_t>(std::floor(cells));
            fraction[axis] = cells - std::floor(cells);
        }
        const ReferenceSample sample = ReferenceOctave(cell, fraction, seed + octave * kOctaveSeedStride);
        out.Value += amplitude * sample.Value;
        for (std::size_t axis = 0; axis < 3; ++axis)
            out.Gradient[axis] += amplitude * frequency * sample.Gradient[axis];
        frequency *= 2.0;
        amplitude *= gain;
    }
    return out;
}

// A deterministic spread of positions on and near a planet's surface, both signs on every axis.
WorldPosition SweepPosition(int index)
{
    // Irrational steps in turns, so no two indices share a phase.
    const double turnsX = 0.7548776662 * index;
    const double turnsY = 0.5698402910 * index;
    const double x = std::sin(turnsX * 2.0 * std::numbers::pi) * kPlanetRadius;
    const double y = std::cos(turnsY * 2.0 * std::numbers::pi) * kPlanetRadius * 0.6;
    const double z = (turnsX - std::floor(turnsX) - 0.5) * 2.0e5 + 0.0371 * index;
    return WorldPosition{x, y, z};
}

} // namespace

TEST(FractalNoise3D, NoRepeatWithin100KilometersAtA2MeterOctave)
{
    // A 2 m octave: 0.5 cells a meter. A shift of n cells repeats the field when every probe matches.
    constexpr double kCellsPerMeter = 0.5;
    constexpr int64_t kCellsIn100Kilometers = 50000;
    const WorldPosition probes[2] = {{0.75, 1.25, 0.5}, {14.6, -6.2, 22.9}};
    int repeatingShifts = 0;
    for (std::size_t axis = 0; axis < 3; ++axis)
        for (int64_t shift = 1; shift <= kCellsIn100Kilometers; ++shift)
        {
            bool repeats = true;
            for (const WorldPosition& probe : probes)
            {
                WorldPosition shifted = probe;
                (axis == 0 ? shifted.X : axis == 1 ? shifted.Y : shifted.Z) += shift / kCellsPerMeter;
                const auto here = GradientNoise3D(NoiseLatticePoint3D::FromPosition(probe, kCellsPerMeter), 7);
                const auto there = GradientNoise3D(NoiseLatticePoint3D::FromPosition(shifted, kCellsPerMeter), 7);
                repeats = repeats && here.Value == there.Value;
            }
            repeatingShifts += repeats ? 1 : 0;
        }
    EXPECT_EQ(repeatingShifts, 0);
}

TEST(FractalNoise3D, NoRepeatAtPowerOfTwoShiftsUpTo2To61Cells)
{
    // A hash that narrows the cell (a 2^32 period, for example) repeats at a power-of-two shift.
    const NoiseLatticePoint3D probes[2] = {{0, 0, 0, Vector3(0.75f, 0.25f, 0.5f)},
                                           {-7, 3, 11, Vector3(0.3f, 0.9f, 0.45f)}};
    for (std::size_t axis = 0; axis < 3; ++axis)
        for (int power = 8; power <= 61; ++power)
        {
            bool repeats = true;
            for (const NoiseLatticePoint3D& probe : probes)
            {
                NoiseLatticePoint3D shifted = probe;
                (axis == 0 ? shifted.CellX : axis == 1 ? shifted.CellY : shifted.CellZ) += int64_t{1} << power;
                repeats = repeats && GradientNoise3D(probe, 7).Value == GradientNoise3D(shifted, 7).Value;
            }
            EXPECT_FALSE(repeats) << "axis " << axis << " shift 2^" << power;
        }
}

TEST(FractalNoise3D, ValueStaysWithinTheSumOfOctaveAmplitudes)
{
    // One octave's |value| is below 1, so the sum is at most the sum of gain^k.
    float worstOctave = 0.0f;
    for (int index = 0; index < 20000; ++index)
    {
        const auto sample = GradientNoise3D(NoiseLatticePoint3D::FromPosition(SweepPosition(index), 0.37), 9);
        worstOctave = std::max(worstOctave, std::abs(sample.Value));
    }
    EXPECT_LT(worstOctave, 1.0f);
    constexpr uint32_t kOctaves = kMaxFractalNoise3DOctaves;
    for (const float gain : {0.5f, 0.99f})
    {
        double amplitudeSum = 0.0, amplitude = 1.0;
        for (uint32_t octave = 0; octave < kOctaves; ++octave, amplitude *= gain)
            amplitudeSum += amplitude;
        for (int index = 0; index < 2000; ++index)
        {
            const auto sample = FractalNoise3D(SweepPosition(index), 0x1p-23, kOctaves, gain, 4);
            EXPECT_LE(std::abs(sample.Value), amplitudeSum) << "gain " << gain << " sample " << index;
        }
    }
}

TEST(FractalNoise3D, PositionPastTheLatticeGivesAFiniteValue)
{
    // 1e30 m at 1 cell per meter is past 2^62 cells on every axis: the cells saturate at the lattice edge.
    const auto sample = FractalNoise3D(WorldPosition{1e30, -1e30, 1e30}, 1.0, 4, 0.5f, 2);
    EXPECT_TRUE(std::isfinite(sample.Value));
    for (std::size_t axis = 0; axis < 3; ++axis)
        EXPECT_TRUE(std::isfinite(sample.Gradient[axis]));
}

TEST(FractalNoise3D, ValueAndGradientAreContinuousAcrossACellFace)
{
    const int64_t cells[][3] = {{0, 0, 0}, {-1, -17, 5}, {25484000, -25484000, 1203}};
    for (const auto& cell : cells)
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            NoiseLatticePoint3D below{cell[0], cell[1], cell[2], Vector3(0.3f, 0.6f, 0.85f)};
            below.Fraction[axis] = 1.0f;
            NoiseLatticePoint3D above = below;
            (axis == 0 ? above.CellX : axis == 1 ? above.CellY : above.CellZ) += 1;
            above.Fraction[axis] = 0.0f;
            const auto belowSample = GradientNoise3D(below, 11);
            const auto aboveSample = GradientNoise3D(above, 11);
            EXPECT_NEAR(belowSample.Value, aboveSample.Value, 1e-6f);
            for (std::size_t component = 0; component < 3; ++component)
                EXPECT_NEAR(belowSample.Gradient[component], aboveSample.Gradient[component], 1e-5f);
        }
}

TEST(FractalNoise3D, GradientMatchesCentralDifferences)
{
    // 2 m down to 0.5 m cells, so the finest octave's chain-rule factor is 2. The fp32 value rounds
    // near 1e-7, so the step is 1 mm and the octaves are few: a gradient near 1 per meter keeps the
    // rounding under 0.1 percent.
    constexpr double kTopFrequency = 0.5;
    constexpr uint32_t kOctaves = 3;
    constexpr float kGain = 0.5f;
    constexpr double kStep = 0.001;
    // The gradient's scale: the sum of amplitude times frequency over the octaves.
    const double gradientScale = kOctaves * kTopFrequency;
    for (int index = 0; index < 64; ++index)
    {
        const WorldPosition position = SweepPosition(index);
        const auto analytic = FractalNoise3D(position, kTopFrequency, kOctaves, kGain, 3);
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            WorldPosition plus = position, minus = position;
            (axis == 0 ? plus.X : axis == 1 ? plus.Y : plus.Z) += kStep;
            (axis == 0 ? minus.X : axis == 1 ? minus.Y : minus.Z) -= kStep;
            const double difference = (static_cast<double>(FractalNoise3D(plus, kTopFrequency, kOctaves, kGain, 3).Value) -
                                       FractalNoise3D(minus, kTopFrequency, kOctaves, kGain, 3).Value) /
                                      (2.0 * kStep);
            EXPECT_NEAR(analytic.Gradient[static_cast<std::size_t>(axis)], difference, 2e-3 * gradientScale)
                << "sample " << index << " axis " << axis;
        }
    }
}

TEST(FractalNoise3D, OneLatticePointThroughThreeExactPathsGivesTheSameBits)
{
    // 0.25 m cells at the planet's radius; the fraction is a sum of powers of two, so every path is exact.
    constexpr double kCellsPerMeter = 4.0;
    const int64_t cell[3] = {25484000, -1203, 77};
    const Vector3 fraction(0.375f, 0.5f, 0.8125f);
    const NoiseLatticePoint3D direct{cell[0], cell[1], cell[2], fraction};
    const WorldPosition meters{(cell[0] + 0.375) / kCellsPerMeter, (cell[1] + 0.5) / kCellsPerMeter,
                               (cell[2] + 0.8125) / kCellsPerMeter};
    Vector3Int sector;
    Vector3 local;
    meters.ToSector(sector, local);
    const WorldPosition fromSector = WorldPosition::FromSector(sector, local);

    const NoiseLatticePoint3D paths[2] = {NoiseLatticePoint3D::FromPosition(meters, kCellsPerMeter),
                                          NoiseLatticePoint3D::FromPosition(fromSector, kCellsPerMeter)};
    const auto expected = GradientNoise3D(direct, 5);
    for (const NoiseLatticePoint3D& path : paths)
    {
        EXPECT_EQ(path.CellX, direct.CellX);
        EXPECT_EQ(path.CellY, direct.CellY);
        EXPECT_EQ(path.CellZ, direct.CellZ);
        const auto actual = GradientNoise3D(path, 5);
        EXPECT_EQ(actual.Value, expected.Value);
        for (std::size_t axis = 0; axis < 3; ++axis)
            EXPECT_EQ(actual.Gradient[axis], expected.Gradient[axis]);
    }
    const auto fractalDirect = FractalNoise3D(meters, 1.0 / 1024.0, 13, 0.6f, 5);
    const auto fractalSector = FractalNoise3D(fromSector, 1.0 / 1024.0, 13, 0.6f, 5);
    EXPECT_EQ(fractalDirect.Value, fractalSector.Value);
}

TEST(FractalNoise3D, OctaveSumMatchesTheDoubleReference)
{
    // 4,096 m down to 0.25 m cells: the detail layer's span on a planet.
    constexpr double kTopFrequency = 1.0 / 4096.0;
    constexpr uint32_t kOctaves = 15;
    double worstValue = 0.0, worstGradient = 0.0;
    for (const float gain : {0.5f, 0.574349177f, 0.75f})
    {
        double amplitudeSum = 0.0, slopeSum = 0.0, amplitude = 1.0, frequency = kTopFrequency;
        for (uint32_t octave = 0; octave < kOctaves; ++octave, amplitude *= gain, frequency *= 2.0)
        {
            amplitudeSum += amplitude;
            slopeSum += amplitude * frequency;
        }
        for (int index = 0; index < 512; ++index)
        {
            const WorldPosition position = SweepPosition(index);
            const uint32_t seed = 1000u + static_cast<uint32_t>(index);
            const auto actual = FractalNoise3D(position, kTopFrequency, kOctaves, gain, seed);
            const ReferenceSample reference = ReferenceFractal(position, kTopFrequency, kOctaves, gain, seed);
            worstValue = std::max(worstValue, std::abs(actual.Value - reference.Value) / amplitudeSum);
            for (std::size_t axis = 0; axis < 3; ++axis)
                worstGradient = std::max(worstGradient, std::abs(actual.Gradient[static_cast<std::size_t>(axis)] -
                                                                 reference.Gradient[axis]) / slopeSum);
        }
    }
    EXPECT_LE(worstValue, 2e-6);
    EXPECT_LE(worstGradient, 5e-6);
}

TEST(FractalNoise3D, ReproducesItsPinnedValues)
{
    // The bits of value and gradient over positions at the origin and at the planet's radius, several
    // seeds, octave counts 0, 1, 7, 26 and 99 (above the clamp), and a non-finite position.
    Testing::BitHasher hasher;
    for (const uint32_t seed : {0u, 1u, 4294967295u})
        for (const uint32_t octaves : {0u, 1u, 7u, 26u, 99u})
            for (int index = 0; index < 24; ++index)
            {
                const WorldPosition atOrigin{-3.1 + 0.37 * index, 2.3 - 0.41 * index, 0.13 * index};
                for (const WorldPosition& position : {atOrigin, SweepPosition(index)})
                {
                    const auto sample = FractalNoise3D(position, 1.0 / 256.0, octaves, 0.574349177f, seed);
                    hasher.Feed(sample.Value);
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        hasher.Feed(sample.Gradient[axis]);
                }
            }
    const auto nonFinite = FractalNoise3D(WorldPosition{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, 1.0, 4, 0.5f, 0);
    hasher.Feed(nonFinite.Value);
    EXPECT_EQ(hasher.Value(), 0xB0062EB03A21E16Dull);
}
