#pragma once

#include <cstdint>
#include <vector>

namespace GameEngine::Image
{

/// A decoded or resampled image: Channels samples per pixel, row-major from the top
/// row, no padding between rows (Pixels holds Width * Height * Channels samples).
/// The channels are, by count: 1 gray; 2 gray, alpha; 3 R, G, B; 4 R, G, B, A.
/// 8-bit color samples are sRGB-encoded unless the producer states that they are
/// linear; float samples are linear; alpha is linear coverage.
template <typename Sample>
struct PixelImage
{
    std::vector<Sample> Pixels;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Channels = 0;
    /// False when the decode or the resample that produced this image failed; the
    /// other fields are then empty.
    bool Valid = false;
};

/// 8 bits per sample: PNG, JPEG, TGA, BMP and the other 8-bit formats.
using Image8 = PixelImage<uint8_t>;
/// 16 bits per sample: 16-bit PNG and PNM, such as heightmaps.
using Image16 = PixelImage<uint16_t>;
/// 32-bit float samples: Radiance HDR, such as sky and lighting images.
using ImageFloat = PixelImage<float>;

} // namespace GameEngine::Image
