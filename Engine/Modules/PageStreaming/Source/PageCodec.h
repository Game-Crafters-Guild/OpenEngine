#pragma once

#include "PageStreaming/PageStoreFormat.h"

#include <memory>
#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

/// One page in a store's encoding, ready to write.
struct EncodedPage
{
    std::vector<uint8> Bytes; ///< PageStoredBytes(format, wide) bytes
    uint64 Hash = 0;          ///< FNV-1a 64 of Bytes
    int32 MinWord = 0;        ///< zero on every format but HeightSteps
    int32 MaxWord = 0;        ///< zero on every format but HeightSteps
    uint8 Flags = 0;          ///< kPageWideWords
};

/// The lowest and highest level-0 value under a height page's footprint. Read by the HeightSteps
/// codec only.
struct PageFootprint
{
    float32 Min = 0.0f;
    float32 Max = 0.0f;
};

/// Turns a page's samples into its stored bytes and back, for one store's format. A page's samples
/// are PageStrideTexels(format)^2 samples of PageTexelBytes(format) bytes each, row-major, apron
/// included: a height's float32 values as bytes, or the format's channels. Encode and Decode may
/// run on any number of threads at once.
class PageCodec
{
public:
    virtual ~PageCodec() = default;

    /// Encodes the samples of a level-`level` page. The same samples encode to the same bytes.
    virtual void Encode(uint32 level, std::span<const uint8> samples, const PageFootprint& footprint,
                        EncodedPage& out) const = 0;

    /// Decodes the stored bytes of the page of `entry`, of level `level`, into its samples.
    virtual void Decode(uint32 level, const PageIndexEntry& entry, std::span<const uint8> bytes,
                        std::span<uint8> outSamples) const = 0;
};

/// The codec of the store whose header is `header`: its steps for HeightSteps, a byte copy for
/// Rgba8, Rgba8Srgb and Rg8, the BC5 encoder and decoder for Bc5.
std::unique_ptr<PageCodec> MakePageCodec(const PageStoreHeader& header);

} // namespace GameEngine::PageStreaming
