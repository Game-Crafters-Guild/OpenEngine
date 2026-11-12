#include "CBTTerrain/CBTPlanetShading.h"

#include "Mathematics/Float8.h"
#include "Mathematics/Transcendental.h"

#include <array>
#include <cassert>
#include <cstddef>

namespace GameEngine::CBTTerrain
{

namespace
{

using Mathematics::Float8;

// PlanetBaseNoise in eight lanes: the same operations in the same order, so each lane has its bits.
Float8 GE_VECTORCALL PlanetBaseNoise(Float8 dx, Float8 dy, Float8 dz, float frequency)
{
    const Float8 f = Float8::Broadcast(frequency);
    const Float8 xf = dx * f;
    const Float8 yf = dy * f;
    const Float8 zf = dz * f;
    const Float8 s1 = Math::FastSin(xf);
    const Float8 s2 = Math::FastSin(yf * Float8::Broadcast(1.3f) + Float8::Broadcast(0.7f));
    const Float8 s3 = Math::FastSin(zf * Float8::Broadcast(0.7f) + Float8::Broadcast(1.9f));
    const Float8 s4 = Math::FastSin(xf * Float8::Broadcast(2.1f) + Float8::Broadcast(1.3f));
    const Float8 s5 = Math::FastSin(yf * Float8::Broadcast(1.7f) + Float8::Broadcast(2.4f));
    return s1 * s2 * s3 + Float8::Broadcast(0.5f) * s4 * s5;
}

} // namespace

void PlanetReliefRow(std::span<const Mathematics::Vector3> directions, float amplitude, float frequency,
                     uint32_t octaves, float minResolvedArc, std::span<float> outHeights)
{
    assert(outHeights.size() == directions.size());
    constexpr std::size_t kLanes = Float8::kLaneCount;
    const uint32_t octaveCount = octaves < 1u ? 1u : octaves;
    std::size_t first = 0;
    for (; amplitude > 0.0f && first + kLanes <= directions.size(); first += kLanes)
    {
        // The SoA staging of eight Vector3 directions, one array per axis. The 32-byte alignment keeps
        // each Float8::Load inside one cache line.
        alignas(32) std::array<float, kLanes> xs, ys, zs;
        for (std::size_t lane = 0; lane < kLanes; ++lane)
        {
            xs[lane] = directions[first + lane].x;
            ys[lane] = directions[first + lane].y;
            zs[lane] = directions[first + lane].z;
        }
        const Float8 dx = Float8::Load(xs);
        const Float8 dy = Float8::Load(ys);
        const Float8 dz = Float8::Load(zs);
        Float8 total;
        float octaveAmplitude = 1.0f, octaveFrequency = frequency, amplitudeSum = 0.0f;
        for (uint32_t i = 0; i < octaveCount; ++i)
        {
            if (kReliefHighestFrequencyFactor * octaveFrequency * minResolvedArc <= kReliefTwoPi)
                total = total + Float8::Broadcast(octaveAmplitude) * PlanetBaseNoise(dx, dy, dz, octaveFrequency);
            amplitudeSum += octaveAmplitude;
            octaveAmplitude *= kReliefPersistence;
            octaveFrequency *= kReliefLacunarity;
        }
        const Float8 heights = Float8::Broadcast(amplitude) * total / Float8::Broadcast(amplitudeSum);
        heights.Store(outHeights.subspan(first).first<kLanes>());
    }
    for (std::size_t i = first; i < directions.size(); ++i)
        outHeights[i] = PlanetRelief(directions[i].x, directions[i].y, directions[i].z, amplitude, frequency, octaves,
                                     minResolvedArc);
}

} // namespace GameEngine::CBTTerrain
