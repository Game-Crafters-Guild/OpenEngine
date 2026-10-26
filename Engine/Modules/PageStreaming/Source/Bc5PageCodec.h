#pragma once

#include "PageCodec.h"

namespace GameEngine::PageStreaming
{

/// The codec of Bc5 pages: two channels a sample, as BC5 blocks (a BC4 block a channel) of 4 x 4
/// samples, row-major over the page's 136 x 136 samples. The encoder takes no search and no
/// dithering: each block's endpoints are the highest and lowest of its 16 values and each value
/// takes the nearest of the eight palette entries (the lowest index on a tie), so the same 16 values
/// always encode to the same block, which is what makes a page's apron block equal to its
/// neighbor's edge block.
class Bc5PageCodec final : public PageCodec
{
public:
    void Encode(uint32 level, std::span<const uint8> samples, const PageFootprint& footprint,
                EncodedPage& out) const override;
    void Decode(uint32 level, const PageIndexEntry& entry, std::span<const uint8> bytes,
                std::span<uint8> outSamples) const override;
};

} // namespace GameEngine::PageStreaming
