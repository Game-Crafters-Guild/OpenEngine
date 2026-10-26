#pragma once

#include "PageCodec.h"

namespace GameEngine::PageStreaming
{

/// The codec of the byte formats (Rgba8, Rgba8Srgb, Rg8): a page's stored bytes are its samples.
class BytePageCodec final : public PageCodec
{
public:
    void Encode(uint32 level, std::span<const uint8> samples, const PageFootprint& footprint,
                EncodedPage& out) const override;
    void Decode(uint32 level, const PageIndexEntry& entry, std::span<const uint8> bytes,
                std::span<uint8> outSamples) const override;
};

} // namespace GameEngine::PageStreaming
