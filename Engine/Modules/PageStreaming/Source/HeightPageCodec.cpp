#include "HeightPageCodec.h"

#include "PageStoreSerialization.h"

#include "Types/Fnv1a.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

namespace GameEngine::PageStreaming
{
namespace
{

// Integers a page's 16-bit words span above its base.
constexpr int64 kWordMax = 65535;

float32 StepOf(const PageStoreHeader& header, uint32 level)
{
    assert(level < kPageStoreMaxLevels && header.StepPerLevel[level] > 0.0f);
    return header.StepPerLevel[level];
}

// The integer nearest value / step. The step rule keeps |value / step| at most kHeightIntegerMax,
// so the integer fits int32; the clamp only guards a value outside the store's range.
int32 NearestInteger(float32 value, float32 step)
{
    constexpr float32 kLimit = static_cast<float32>(kHeightIntegerMax) * 2.0f - 128.0f;
    return static_cast<int32>(std::clamp(std::round(value / step), -kLimit, kLimit));
}

float32 IntegerValue(int64 integer, float32 step)
{
    return static_cast<float32>(integer) * step;
}

// The largest integer whose value is at most `value`.
int32 FloorInteger(float32 value, float32 step)
{
    const int32 nearest = NearestInteger(value, step);
    return IntegerValue(nearest, step) > value ? nearest - 1 : nearest;
}

// The smallest integer whose value is at least `value`.
int32 CeilInteger(float32 value, float32 step)
{
    const int32 nearest = NearestInteger(value, step);
    return IntegerValue(nearest, step) < value ? nearest + 1 : nearest;
}

} // namespace

float32 QuantizeHeight(float32 value, float32 step)
{
    return IntegerValue(NearestInteger(value, step), step);
}

void EncodeHeightPage(const PageStoreHeader& header, uint32 level, std::span<const float32> samples,
                      float32 footprintMin, float32 footprintMax, EncodedPage& out)
{
    assert(samples.size() == kPageSampleCount);
    const float32 step = StepOf(header, level);
    int32 lowest = std::numeric_limits<int32>::max();
    int32 highest = std::numeric_limits<int32>::lowest();
    std::vector<int32> integers(kPageSampleCount);
    for (std::size_t i = 0; i < kPageSampleCount; ++i)
    {
        integers[i] = NearestInteger(samples[i], step);
        lowest = std::min(lowest, integers[i]);
        highest = std::max(highest, integers[i]);
    }
    out.MinWord = std::min(lowest, FloorInteger(footprintMin, step));
    out.MaxWord = std::max(highest, CeilInteger(footprintMax, step));
    const bool wide = static_cast<int64>(highest) - out.MinWord > kWordMax;
    out.Flags = wide ? kPageWideWords : uint8{0};
    out.Bytes.resize(PageStoredBytes(PageFieldFormat::HeightSteps, wide));
    if (wide)
    {
        std::memcpy(out.Bytes.data(), integers.data(), out.Bytes.size());
    }
    else
    {
        auto* words = reinterpret_cast<uint16*>(out.Bytes.data());
        for (std::size_t i = 0; i < kPageSampleCount; ++i)
            words[i] = static_cast<uint16>(integers[i] - out.MinWord);
    }
    out.Hash = Hashing::Fnv1a64(out.Bytes.data(), out.Bytes.size());
}

void DecodeHeightPage(const PageStoreHeader& header, uint32 level, const PageIndexEntry& entry,
                      std::span<const uint8> bytes, std::span<float32> out)
{
    assert(out.size() == kPageSampleCount &&
           bytes.size() == PageStoredBytes(PageFieldFormat::HeightSteps, entry.HasWideWords()));
    const float32 step = StepOf(header, level);
    if (entry.HasWideWords())
    {
        int32 integer = 0;
        for (std::size_t i = 0; i < kPageSampleCount; ++i)
        {
            std::memcpy(&integer, bytes.data() + i * sizeof(int32), sizeof(int32));
            out[i] = IntegerValue(integer, step);
        }
        return;
    }
    uint16 word = 0;
    for (std::size_t i = 0; i < kPageSampleCount; ++i)
    {
        std::memcpy(&word, bytes.data() + i * sizeof(uint16), sizeof(uint16));
        out[i] = IntegerValue(static_cast<int64>(entry.MinWord) + word, step);
    }
}

float32 DecodeHeightWord(const PageStoreHeader& header, uint32 level, int32 word)
{
    return IntegerValue(word, StepOf(header, level));
}

HeightPageCodec::HeightPageCodec(const PageStoreHeader& header)
    : m_Header(header)
{
}

void HeightPageCodec::Encode(uint32 level, std::span<const uint8> samples, const PageFootprint& footprint,
                             EncodedPage& out) const
{
    assert(samples.size() == kPageSampleCount * sizeof(float32));
    const std::span<const float32> heights(reinterpret_cast<const float32*>(samples.data()), kPageSampleCount);
    EncodeHeightPage(m_Header, level, heights, footprint.Min, footprint.Max, out);
}

void HeightPageCodec::Decode(uint32 level, const PageIndexEntry& entry, std::span<const uint8> bytes,
                             std::span<uint8> outSamples) const
{
    assert(outSamples.size() == kPageSampleCount * sizeof(float32));
    const std::span<float32> heights(reinterpret_cast<float32*>(outSamples.data()), kPageSampleCount);
    DecodeHeightPage(m_Header, level, entry, bytes, heights);
}

} // namespace GameEngine::PageStreaming
