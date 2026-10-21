#include "PngDeclaration.h"

#include <cstring>

namespace GameEngine::Image
{
namespace
{
uint32_t ReadBigEndian32(const uint8_t* bytes)
{
    return (uint32_t(bytes[0]) << 24u) | (uint32_t(bytes[1]) << 16u) | (uint32_t(bytes[2]) << 8u) | uint32_t(bytes[3]);
}
} // namespace

std::optional<PngDeclaration> ReadPngDeclaration(std::span<const uint8_t> leadingBytes)
{
    static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    // The IHDR's data: width at 16, height at 20, bit depth at 24, color type at 25,
    // compression, filter and interlace at 26 to 28.
    if (leadingBytes.size() < 29 || std::memcmp(leadingBytes.data(), kSignature, sizeof(kSignature)) != 0 ||
        ReadBigEndian32(leadingBytes.data() + 8) != 13 || std::memcmp(leadingBytes.data() + 12, "IHDR", 4) != 0)
        return std::nullopt;
    const uint8_t* ihdr = leadingBytes.data() + 16;
    PngDeclaration png{};
    png.Width = ReadBigEndian32(ihdr);
    png.Height = ReadBigEndian32(ihdr + 4);
    png.BitDepth = ihdr[8];
    const uint8_t colorType = ihdr[9];
    const bool depthValid = png.BitDepth == 1 || png.BitDepth == 2 || png.BitDepth == 4 || png.BitDepth == 8 ||
                            png.BitDepth == 16;
    const bool palette = colorType == 3;
    if (png.Width == 0 || png.Height == 0 || !depthValid || colorType > 6 || (colorType & 1 && !palette) ||
        (palette && png.BitDepth == 16) || ihdr[10] != 0 || ihdr[11] != 0 || ihdr[12] > 1)
        return std::nullopt;
    const uint32_t fileChannels = (colorType & 2 ? 3u : 1u) + (colorType & 4 ? 1u : 0u);
    png.LimitChannels = palette ? 4u : fileChannels;
    png.Channels = palette ? 3u : fileChannels;

    const uint64_t width = png.Width;
    const uint64_t height = png.Height;
    const uint64_t rawChannels = palette ? 1u : fileChannels;
    // A non-interlaced image filters its full rows; an interlaced one filters each pass's
    // smaller rows into a full image of whole bytes per sample.
    const uint64_t rowBytes = (rawChannels * width * png.BitDepth + 7u) >> 3u;
    const uint64_t filteredBytes = ihdr[12] == 0 ? rowBytes * height + rowBytes
                                                 : width * height * rawChannels * (png.BitDepth == 16 ? 2u : 1u);
    png.WithinDecoderLimits = width <= kMaxPngSide && height <= kMaxPngSide &&
                              width * height * png.LimitChannels <= kMaxPngSamples &&
                              filteredBytes <= kMaxPngRowBytes;
    return png;
}

} // namespace GameEngine::Image
