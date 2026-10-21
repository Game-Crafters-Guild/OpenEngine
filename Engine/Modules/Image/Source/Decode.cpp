#include "Image/Decode.h"

#include "Logger/Logger.h"
#include "PngDeclaration.h"

#include <stb_image.h>

#include <climits>
#include <optional>

namespace GameEngine::Image
{
namespace
{
// stb_image allocates a decoded image as one int-sized block, so no result can be
// larger than this.
constexpr uint64_t kMaxResultBytes = INT_MAX;

int RequestedChannelCount(DecodeChannels channels)
{
    switch (channels)
    {
    case DecodeChannels::FromFile:
        return 0;
    case DecodeChannels::Gray:
        return 1;
    case DecodeChannels::Rgb:
        return 3;
    case DecodeChannels::Rgba:
        return 4;
    }
    return 0;
}

// Warns, by name, that a PNG declares more than stb_image's PNG decoder reads.
void WarnPastPngLimits(const PngDeclaration& png, std::string_view sourceName)
{
    Logger::Log::Warning("Image: '{}' is a PNG of {}x{} pixels of {} channels at {} bits, past what a PNG decode "
                         "reads (at most {} pixels a side, {} samples and {} bytes of image rows); downscale the "
                         "image",
                         sourceName, png.Width, png.Height, png.LimitChannels, png.BitDepth, kMaxPngSide,
                         kMaxPngSamples, kMaxPngRowBytes);
}

// The decoded result's extent, read from the header before any pixel is allocated.
struct DecodeHeader
{
    int Width = 0;
    int Height = 0;
    int Channels = 0;
    bool Valid = false;
};

// Reads the header and refuses, by name, a decode stb_image cannot produce: input past
// what it can address, a PNG past its decoder's limits (PngDeclaration.h), or a result
// of more than kMaxResultBytes for `bytesPerSample`-byte samples.
DecodeHeader ReadDecodableHeader(std::span<const uint8_t> fileBytes, std::string_view sourceName,
                                 DecodeChannels channels, uint64_t bytesPerSample)
{
    DecodeHeader header{};
    if (fileBytes.empty())
        return header;
    if (fileBytes.size() > static_cast<size_t>(INT_MAX))
    {
        Logger::Log::Warning("Image: '{}' is {} bytes, past the {} bytes an image decode reads; recompress "
                             "or split the image",
                             sourceName, fileBytes.size(), INT_MAX);
        return header;
    }
    if (const std::optional<PngDeclaration> png = ReadPngDeclaration(fileBytes); png && !png->WithinDecoderLimits)
    {
        WarnPastPngLimits(*png, sourceName);
        return header;
    }
    int fileChannels = 0;
    if (!stbi_info_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()), &header.Width, &header.Height,
                               &fileChannels) ||
        header.Width <= 0 || header.Height <= 0)
        return header;
    const int requested = RequestedChannelCount(channels);
    header.Channels = requested == 0 ? fileChannels : requested;
    const uint64_t resultBytes = static_cast<uint64_t>(header.Width) * static_cast<uint64_t>(header.Height) *
                                 static_cast<uint64_t>(header.Channels) * bytesPerSample;
    if (resultBytes > kMaxResultBytes)
    {
        Logger::Log::Warning("Image: '{}' declares {}x{} pixels of {} channels, {} bytes decoded, past the {} bytes "
                             "an image decode can produce; downscale the image",
                             sourceName, header.Width, header.Height, header.Channels, resultBytes, kMaxResultBytes);
        return header;
    }
    header.Valid = true;
    return header;
}

// Copies stb_image's result into `result` when its extent matches the header it was
// checked against; a decode whose extent disagrees with its own header is refused
// rather than trusted. A decode that failed warns with stb_image's reason. Frees
// `pixels` either way.
template <typename Sample>
void TakeDecodedPixels(Sample* pixels, int width, int height, int fileChannels, const DecodeHeader& header,
                       DecodeChannels channels, std::string_view sourceName, PixelImage<Sample>& result)
{
    if (!pixels)
    {
        // stb_image's reason is per thread and set by the call that just failed.
        const char* reason = stbi_failure_reason();
        Logger::Log::Warning("Image: '{}' could not be decoded: {}", sourceName, reason ? reason : "unknown");
        return;
    }
    const int decodedChannels = channels == DecodeChannels::FromFile ? fileChannels : header.Channels;
    if (width == header.Width && height == header.Height && decodedChannels >= 1 && decodedChannels <= 4)
    {
        const size_t sampleCount = static_cast<size_t>(width) * static_cast<size_t>(height) *
                                   static_cast<size_t>(decodedChannels);
        result.Pixels.assign(pixels, pixels + sampleCount);
        result.Width = static_cast<uint32_t>(width);
        result.Height = static_cast<uint32_t>(height);
        result.Channels = static_cast<uint32_t>(decodedChannels);
        result.Valid = true;
    }
    stbi_image_free(pixels);
}
} // namespace

Image8 Decode8(std::span<const uint8_t> fileBytes, std::string_view sourceName, DecodeChannels channels)
{
    Image8 result{};
    const DecodeHeader header = ReadDecodableHeader(fileBytes, sourceName, channels, sizeof(uint8_t));
    if (!header.Valid)
        return result;
    int width = 0;
    int height = 0;
    int fileChannels = 0;
    stbi_uc* pixels = stbi_load_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()), &width, &height,
                                            &fileChannels, RequestedChannelCount(channels));
    TakeDecodedPixels(pixels, width, height, fileChannels, header, channels, sourceName, result);
    return result;
}

Image16 Decode16(std::span<const uint8_t> fileBytes, std::string_view sourceName, DecodeChannels channels)
{
    Image16 result{};
    const DecodeHeader header = ReadDecodableHeader(fileBytes, sourceName, channels, sizeof(uint16_t));
    if (!header.Valid)
        return result;
    int width = 0;
    int height = 0;
    int fileChannels = 0;
    stbi_us* pixels = stbi_load_16_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()), &width,
                                               &height, &fileChannels, RequestedChannelCount(channels));
    TakeDecodedPixels(pixels, width, height, fileChannels, header, channels, sourceName, result);
    return result;
}

ImageFloat DecodeFloat(std::span<const uint8_t> fileBytes, std::string_view sourceName, DecodeChannels channels)
{
    ImageFloat result{};
    const DecodeHeader header = ReadDecodableHeader(fileBytes, sourceName, channels, sizeof(float));
    if (!header.Valid)
        return result;
    int width = 0;
    int height = 0;
    int fileChannels = 0;
    float* pixels = stbi_loadf_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()), &width, &height,
                                           &fileChannels, RequestedChannelCount(channels));
    TakeDecodedPixels(pixels, width, height, fileChannels, header, channels, sourceName, result);
    return result;
}

} // namespace GameEngine::Image
