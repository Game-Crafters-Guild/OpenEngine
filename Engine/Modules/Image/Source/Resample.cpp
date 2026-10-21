#include "Image/Resample.h"

#include <stb_image_resize2.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <utility>

namespace GameEngine::Image
{
namespace
{
// stb_image_resize2 takes int extents; four elements per pixel.
bool IsResampleExtent(size_t elementCount, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || width > static_cast<uint32_t>(INT_MAX) ||
        height > static_cast<uint32_t>(INT_MAX))
        return false;
    return static_cast<uint64_t>(width) * height * 4 == elementCount;
}
} // namespace

bool ResampleRgba8(std::span<const uint8_t> source, uint32_t sourceWidth, uint32_t sourceHeight,
                   std::span<uint8_t> destination, uint32_t destinationWidth, uint32_t destinationHeight,
                   ResampleColorSpace colorSpace)
{
    if (!IsResampleExtent(source.size(), sourceWidth, sourceHeight) ||
        !IsResampleExtent(destination.size(), destinationWidth, destinationHeight))
        return false;
    const int srcW = static_cast<int>(sourceWidth);
    const int srcH = static_cast<int>(sourceHeight);
    const int dstW = static_cast<int>(destinationWidth);
    const int dstH = static_cast<int>(destinationHeight);
    switch (colorSpace)
    {
    case ResampleColorSpace::Srgb:
        return stbir_resize_uint8_srgb(source.data(), srcW, srcH, 0, destination.data(), dstW, dstH, 0,
                                       STBIR_RGBA) != nullptr;
    case ResampleColorSpace::Linear:
        return stbir_resize_uint8_linear(source.data(), srcW, srcH, 0, destination.data(), dstW, dstH, 0,
                                         STBIR_4CHANNEL) != nullptr;
    case ResampleColorSpace::LinearAlphaWeighted:
        return stbir_resize_uint8_linear(source.data(), srcW, srcH, 0, destination.data(), dstW, dstH, 0,
                                         STBIR_RGBA) != nullptr;
    }
    return false;
}

bool ResampleRgba32F(std::span<const float> source, uint32_t sourceWidth, uint32_t sourceHeight,
                     std::span<float> destination, uint32_t destinationWidth, uint32_t destinationHeight)
{
    if (!IsResampleExtent(source.size(), sourceWidth, sourceHeight) ||
        !IsResampleExtent(destination.size(), destinationWidth, destinationHeight))
        return false;
    return stbir_resize_float_linear(source.data(), static_cast<int>(sourceWidth), static_cast<int>(sourceHeight),
                                     0, destination.data(), static_cast<int>(destinationWidth),
                                     static_cast<int>(destinationHeight), 0, STBIR_4CHANNEL) != nullptr;
}

Image8 ResampleToLongEdge(std::span<const uint8_t> source, uint32_t sourceWidth, uint32_t sourceHeight,
                          uint32_t longEdge)
{
    Image8 result{};
    if (longEdge == 0 || !IsResampleExtent(source.size(), sourceWidth, sourceHeight))
        return result;
    const double scale = static_cast<double>(longEdge) / static_cast<double>(std::max(sourceWidth, sourceHeight));
    const uint32_t width = std::max(1u, static_cast<uint32_t>(std::lround(sourceWidth * scale)));
    const uint32_t height = std::max(1u, static_cast<uint32_t>(std::lround(sourceHeight * scale)));
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    if (!ResampleRgba8(source, sourceWidth, sourceHeight, pixels, width, height, ResampleColorSpace::Srgb))
        return result;
    result.Pixels = std::move(pixels);
    result.Width = width;
    result.Height = height;
    result.Channels = 4;
    result.Valid = true;
    return result;
}

} // namespace GameEngine::Image
