#include "PageFieldFilter.h"

#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector3.h"
#include "Types/Color.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::PageStreaming
{
namespace
{

// The [1 2 1] x [1 2 1] tent's weights on the taps; they sum to 16.
constexpr std::array<uint32, kFilterTaps> kTapWeights{1u, 2u, 1u, 2u, 4u, 2u, 1u, 2u, 1u};
constexpr uint32 kTapWeightSum = 16u;
constexpr float32 kByteMax = 255.0f;
// A splat texel's four weights sum to this.
constexpr uint32 kWeightChannels = 4u;
constexpr uint32 kWeightSum = 255u;

using Taps = FilterTaps;


uint8 ToByte(float32 unit)
{
    return static_cast<uint8>(std::lround(Math::Clamp01(unit) * kByteMax));
}

// The tent per channel, rounded to nearest.
void TentTexel(const Taps& taps, uint32 channels, uint8* out)
{
    for (uint32 channel = 0; channel < channels; ++channel)
    {
        uint32 sum = 0;
        for (uint32 t = 0; t < kFilterTaps; ++t)
            sum += kTapWeights[t] * taps[t][channel];
        out[channel] = static_cast<uint8>((sum + kTapWeightSum / 2u) / kTapWeightSum);
    }
}

// The tent per weight, renormalized to sum to 255: each weight takes the floor of its share and the
// remainder goes to the largest (the lowest channel on a tie). A footprint with no weight stays zero.
void WeightsTentTexel(const Taps& taps, uint8* out)
{
    std::array<uint32, kWeightChannels> sums{};
    uint32 total = 0;
    for (uint32 channel = 0; channel < kWeightChannels; ++channel)
    {
        for (uint32 t = 0; t < kFilterTaps; ++t)
            sums[channel] += kTapWeights[t] * taps[t][channel];
        total += sums[channel];
    }
    if (total == 0)
    {
        std::fill(out, out + kWeightChannels, uint8{0});
        return;
    }
    uint32 assigned = 0;
    uint32 largest = 0;
    for (uint32 channel = 0; channel < kWeightChannels; ++channel)
    {
        out[channel] = static_cast<uint8>(static_cast<uint64>(sums[channel]) * kWeightSum / total);
        assigned += out[channel];
        if (sums[channel] > sums[largest])
            largest = channel;
    }
    out[largest] = static_cast<uint8>(out[largest] + (kWeightSum - assigned));
}

// A unit normal stored as (x, z) in unsigned normalized bytes; y is the non-negative rest.
Mathematics::Vector3 DecodeNormal(const uint8* texel)
{
    const float32 x = static_cast<float32>(texel[0]) / kByteMax * 2.0f - 1.0f;
    const float32 z = static_cast<float32>(texel[1]) / kByteMax * 2.0f - 1.0f;
    return {x, std::sqrt(std::max(0.0f, 1.0f - x * x - z * z)), z};
}

// The tent on the unit normals, renormalized; a footprint whose normals cancel faces up.
void NormalTentTexel(const Taps& taps, uint8* out)
{
    Mathematics::Vector3 sum;
    for (uint32 t = 0; t < kFilterTaps; ++t)
        sum = sum + DecodeNormal(taps[t]) * static_cast<float32>(kTapWeights[t]);
    Mathematics::Vector3 normal = sum.NormalizeOrZero();
    if (normal.LengthSquared() == 0.0f)
        normal = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
    EncodeNormalSample(normal, out);
}

// The tent in linear light on premultiplied color: rgb decoded from sRGB, a linear, rgb encoded back.
void PremultipliedTentTexel(const Taps& taps, uint8* out)
{
    ColorLinear sum(0.0f, 0.0f, 0.0f, 0.0f);
    for (uint32 t = 0; t < kFilterTaps; ++t)
    {
        const ColorLinear tap = ColorSRGB::FromSRGB255(taps[t][0], taps[t][1], taps[t][2], taps[t][3]).ToLinear();
        const float32 weight = static_cast<float32>(kTapWeights[t]) / static_cast<float32>(kTapWeightSum);
        sum.r += weight * tap.r;
        sum.g += weight * tap.g;
        sum.b += weight * tap.b;
        sum.a += weight * tap.a;
    }
    const ColorSRGB encoded = sum.ToSRGB();
    out[0] = ToByte(encoded.r);
    out[1] = ToByte(encoded.g);
    out[2] = ToByte(encoded.b);
    out[3] = ToByte(encoded.a);
}

} // namespace

void FilterCoarserTexel(PageFieldFilter filter, const FilterTaps& taps, uint32 texelBytes, uint8* out)
{
    switch (filter)
    {
    case PageFieldFilter::WeightsTent:
        WeightsTentTexel(taps, out);
        break;
    case PageFieldFilter::NormalTent:
        NormalTentTexel(taps, out);
        break;
    case PageFieldFilter::Tent:
        TentTexel(taps, texelBytes, out);
        break;
    case PageFieldFilter::PremultipliedTent:
        PremultipliedTentTexel(taps, out);
        break;
    case PageFieldFilter::HeightTent:
        break;
    }
}

} // namespace GameEngine::PageStreaming
