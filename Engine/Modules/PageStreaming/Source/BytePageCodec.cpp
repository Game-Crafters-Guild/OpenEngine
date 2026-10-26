#include "BytePageCodec.h"

#include "Types/Fnv1a.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::PageStreaming
{

void BytePageCodec::Encode(uint32 /*level*/, std::span<const uint8> samples, const PageFootprint& /*footprint*/,
                           EncodedPage& out) const
{
    out.Bytes.assign(samples.begin(), samples.end());
    out.Hash = Hashing::Fnv1a64(out.Bytes.data(), out.Bytes.size());
    out.MinWord = 0;
    out.MaxWord = 0;
    out.Flags = 0;
}

void BytePageCodec::Decode(uint32 /*level*/, const PageIndexEntry& /*entry*/, std::span<const uint8> bytes,
                           std::span<uint8> outSamples) const
{
    assert(bytes.size() == outSamples.size());
    std::copy(bytes.begin(), bytes.end(), outSamples.begin());
}

} // namespace GameEngine::PageStreaming
