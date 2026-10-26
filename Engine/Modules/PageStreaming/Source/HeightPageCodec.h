#pragma once

#include "PageCodec.h"
#include "PageStreaming/PageStoreFormat.h"

#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

/// The step of every level of a normalized store (.r16 and 16-bit PNG sources): 1 / 65535, the
/// source's own quantum. Level 0 is then lossless, and the whole [0, 1] range fits 16 bits, so no
/// page needs 32-bit integers.
inline constexpr float32 kNormalizedHeightStep = 1.0f / 65535.0f;
/// `value` on the lattice of `step`: step x k for the nearest integer k. A value already on the
/// lattice is returned unchanged.
float32 QuantizeHeight(float32 value, float32 step);

/// Encodes the kPageSampleCount samples of a level-`level` page (row-major, apron included) in that
/// level's step, with the footprint's lowest and highest level-0 values: 16-bit words above the
/// page's MinWord when its largest integer is at most 65535 above it, else 32-bit integers. Every
/// sample is on the level's lattice (the cooker quantizes before it filters or bounds), so each
/// encodes to its own integer.
void EncodeHeightPage(const PageStoreHeader& header, uint32 level, std::span<const float32> samples,
                      float32 footprintMin, float32 footprintMax, EncodedPage& out);

/// Decodes a page's stored bytes into kPageSampleCount samples, h = step x k: the one decode every
/// reader of a store uses.
void DecodeHeightPage(const PageStoreHeader& header, uint32 level, const PageIndexEntry& entry,
                      std::span<const uint8> bytes, std::span<float32> out);

/// The value of a level-`level` page's MinWord or MaxWord.
float32 DecodeHeightWord(const PageStoreHeader& header, uint32 level, int32 word);

/// The HeightSteps codec: EncodeHeightPage and DecodeHeightPage in the steps of one store's header,
/// on samples that are float32 heights as bytes.
class HeightPageCodec final : public PageCodec
{
public:
    explicit HeightPageCodec(const PageStoreHeader& header);

    void Encode(uint32 level, std::span<const uint8> samples, const PageFootprint& footprint,
                EncodedPage& out) const override;
    void Decode(uint32 level, const PageIndexEntry& entry, std::span<const uint8> bytes,
                std::span<uint8> outSamples) const override;

private:
    PageStoreHeader m_Header;
};

} // namespace GameEngine::PageStreaming
