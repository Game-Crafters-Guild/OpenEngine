#include "Terrain/TerrainNoiseStack.h"

#include <algorithm>
#include <array>
#include <cassert>

namespace GameEngine::Terrain
{

namespace
{
// The largest cell is at most this many times the 250 m cell: 16 M km, past any float32 terrain.
constexpr uint32 kMaxCellDoublings = 16;
// A terrain holds at least this many of its largest cells along its longer side.
constexpr float32 kMinCellsAcrossTerrain = 4.0f;
// The share of BandTop() that the highest point reaches, by LargeOctaves(), measured on the smallest terrain of each
// count (PageStoreTests). The share changes with the size: up to 8.7 percent under the highest point near 32 km and
// 32,768 km (counts 4 and 14). Streamed tiles widen the range as they load; #6625 measures the exact top.
constexpr std::array<float32, kMaxCellDoublings + 1> kRealizedTopShares = {
    1.0f, 0.87f, 0.80f, 0.815f, 0.82f, 0.81f, 0.85f, 0.865f, 0.805f, 0.785f, 0.825f, 0.81f, 0.805f, 0.825f, 0.76f, 0.79f, 0.795f};
} // namespace

TerrainNoiseStack TerrainNoiseStack::ForTerrainSize(float32 terrainSize, float32 gain)
{
    assert(gain > 0.0f && gain < 1.0f);
    TerrainNoiseStack stack;
    const float32 detailTopCell = 1.0f / stack.TopFrequency;
    uint32 doublings = 0;
    while (doublings < kMaxCellDoublings &&
           detailTopCell * static_cast<float32>(2u << doublings) * kMinCellsAcrossTerrain <= terrainSize)
        ++doublings;
    stack.TopFrequency /= static_cast<float32>(1u << doublings);
    stack.Octaves += doublings;
    stack.Gain = gain;
    stack.TopSeed -= doublings;
    return stack;
}

uint32 TerrainNoiseStack::LargeOctaves() const
{
    return Octaves > kDetailOctaves ? Octaves - kDetailOctaves : 0u;
}

float32 TerrainNoiseStack::BandTop() const
{
    const uint32 largeOctaves = LargeOctaves();
    float32 amplitude = 1.0f;
    float32 detailTotal = 0.0f;
    for (uint32 j = largeOctaves; j < Octaves; ++j, amplitude *= Gain)
        detailTotal += amplitude;
    float32 largeTotal = 0.0f;
    amplitude = 1.0f;
    for (uint32 k = 0; k < largeOctaves; ++k)
    {
        amplitude *= kLargeOctaveGrowth;
        largeTotal += amplitude;
    }
    return (detailTotal + largeTotal) / detailTotal * Amplitude;
}

float32 TerrainNoiseStack::RealizedTop() const
{
    const std::size_t index = std::min<std::size_t>(LargeOctaves(), kRealizedTopShares.size() - 1);
    return kRealizedTopShares[index] * BandTop();
}

} // namespace GameEngine::Terrain
