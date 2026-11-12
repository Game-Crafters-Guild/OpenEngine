#pragma once

#include "Types/Types.h"

namespace GameEngine::Terrain
{

/// The octave stack of a terrain's base value noise (SampleWorldSpaceNoise): octave k has a cell of 1 / (TopFrequency x 2^k) m and the seed TopSeed + k.
/// The kDetailOctaves finest octaves (250 m to 15.625 m) have the amplitude Gain^j; each larger octave has kLargeOctaveGrowth times the next smaller one.
/// The sum, over the detail band's total and times Amplitude, spans [0, BandTop()]; the detail band spans [0, Amplitude].
struct TerrainNoiseStack
{
    /// The presets' gains, 2^-H for the exponent H of the detail band's height over wavelength.
    /// The planets note (section 3.15) has the same table.
    static constexpr float32 kSmoothGain = 0.5f;     ///< gain 0.5 (H 1.0): soft ground
    static constexpr float32 kRollingGain = 0.5743f; ///< gain 0.574 (H 0.8): hills at every scale
    static constexpr float32 kRuggedGain = 0.6598f;  ///< gain 0.660 (H 0.6): broken ground
    /// The octaves of 250 m and finer.
    static constexpr uint32 kDetailOctaves = 5;
    /// 2^0.5: an octave above 250 m has height proportional to the square root of its cell.
    static constexpr float32 kLargeOctaveGrowth = 1.41421356f;
    float32 TopFrequency = 0.004f; ///< per meter: the largest octave's cell is 250 m
    uint32 Octaves = 5;            ///< the finest cell is 15.625 m
    float32 Gain = 0.5f;           ///< amplitude of an octave over the one before it
    uint32 TopSeed = 42;
    float32 Amplitude = 1.0f;      ///< scales the whole sum: a Generated noise volume's Weight

    /// The stack of a terrain whose longer side is `terrainSize` meters. The largest cell is 250 m x 2^J,
    /// the largest such cell that fits four times in the terrain (J >= 0); the finest cell stays 15.625 m.
    /// The seed follows the cell, so the 250 m to 15.625 m octaves are the same on every terrain size.
    static TerrainNoiseStack ForTerrainSize(float32 terrainSize, float32 gain);

    /// The octaves above 250 m: 0 when the stack has kDetailOctaves octaves or fewer.
    uint32 LargeOctaves() const;

    /// The highest value of the sum: Amplitude for a terrain under 2 km.
    float32 BandTop() const;

    /// The highest point of the terrain: a measured share of BandTop() by LargeOctaves()
    /// (kRealizedTopShares in the source); BandTop() with no large octaves.
    float32 RealizedTop() const;

    bool operator==(const TerrainNoiseStack&) const = default;
};

} // namespace GameEngine::Terrain
