#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace GameEngine::Image
{

// The image a PNG's IHDR declares, judged against stb_image's PNG decoder.
struct PngDeclaration
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    // Channels stb_image counts for its sample limit: gray 1, gray and alpha 2, RGB 3,
    // RGBA 4, a palette 4.
    uint32_t LimitChannels = 0;
    // Channels the file decodes to, as LimitChannels except a palette, which is 3
    // (4 with transparency, which the IHDR cannot tell).
    uint32_t Channels = 0;
    uint32_t BitDepth = 0;
    // False when stb_image's PNG decoder refuses the extent: see kMaxPngSide,
    // kMaxPngSamples and kMaxPngRowBytes.
    bool WithinDecoderLimits = false;
};

// stb_image's PNG limits: pixels a side (STBI_MAX_DIMENSIONS), samples (width * height
// * LimitChannels), and the bytes of the filtered image rows, held in one int-sized
// block (each row's bytes plus one row more; for an interlaced image, the whole image).
inline constexpr uint64_t kMaxPngSide = 1ull << 24;
inline constexpr uint64_t kMaxPngSamples = 1ull << 30;
inline constexpr uint64_t kMaxPngRowBytes = 2147483647ull;

// Reads the IHDR from a file's leading bytes (the first 29 are enough). Nullopt when
// they are not the PNG signature followed by an IHDR stb_image accepts.
std::optional<PngDeclaration> ReadPngDeclaration(std::span<const uint8_t> leadingBytes);

} // namespace GameEngine::Image
