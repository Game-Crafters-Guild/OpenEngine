#pragma once

#include "Image/PixelImage.h"

#include <cstdint>
#include <span>

namespace GameEngine::Image
{

/// How ResampleRgba8 filters the four channels.
enum class ResampleColorSpace : uint8_t
{
    /// Color: R, G and B are sRGB-encoded and filter in linear light; alpha is
    /// coverage and weights the color of each tap (stb_image_resize2's STBIR_RGBA).
    Srgb,
    /// Data (normals, masks, linear color): the four channels filter independently
    /// as plain numbers, alpha included (STBIR_4CHANNEL).
    Linear,
    /// Linear color with coverage alpha: R, G and B filter as plain numbers, and
    /// alpha weights the color of each tap, as in Srgb (STBIR_RGBA without the
    /// transfer curve).
    LinearAlphaWeighted,
};

/// Resamples an RGBA8 image (an Image8's layout with 4 channels) to `destinationWidth` x
/// `destinationHeight` with stb_image_resize2's default filters (Mitchell when
/// shrinking, Catmull-Rom when enlarging; edges clamp). This is the texture cook's mip
/// filter: its output is pinned by golden hashes.
/// @return False, with `destination` unchanged, when a span does not hold its
/// extent's Width * Height * 4 bytes, an extent is zero or the resize fails.
bool ResampleRgba8(std::span<const uint8_t> source, uint32_t sourceWidth, uint32_t sourceHeight,
                   std::span<uint8_t> destination, uint32_t destinationWidth, uint32_t destinationHeight,
                   ResampleColorSpace colorSpace);

/// Resamples a float RGBA image (four floats per pixel, linear values, otherwise
/// an Image8's layout) to `destinationWidth` x `destinationHeight`; the four
/// channels filter independently, with the filters of ResampleRgba8.
/// @return False, with `destination` unchanged, when a span does not hold its
/// extent's Width * Height * 4 floats, an extent is zero or the resize fails.
bool ResampleRgba32F(std::span<const float> source, uint32_t sourceWidth, uint32_t sourceHeight,
                     std::span<float> destination, uint32_t destinationWidth, uint32_t destinationHeight);

/// Resamples an sRGB-encoded RGBA8 color image so that its longer edge is
/// `longEdge` pixels and its aspect is kept (the shorter edge rounds to the nearest
/// pixel, at least 1). Filters as ResampleColorSpace::Srgb.
/// @return An invalid image when `source` does not hold Width * Height * 4 bytes,
/// an extent or `longEdge` is zero, or the resize fails.
Image8 ResampleToLongEdge(std::span<const uint8_t> source, uint32_t sourceWidth, uint32_t sourceHeight,
                          uint32_t longEdge);

} // namespace GameEngine::Image
