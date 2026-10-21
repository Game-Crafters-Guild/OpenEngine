#include "Image/ImageHeader.h"

#include "PngDeclaration.h"

#include <stb_image.h>

#include <fstream>
#include <optional>
#include <span>

namespace GameEngine::Image
{
namespace
{
// stb_image's reader callbacks over a std::ifstream, which opens any filesystem path.
int ReadFromStream(void* user, char* data, int size)
{
    auto* stream = static_cast<std::ifstream*>(user);
    stream->read(data, size);
    return static_cast<int>(stream->gcount());
}

void SkipInStream(void* user, int count)
{
    auto* stream = static_cast<std::ifstream*>(user);
    stream->clear();
    stream->seekg(count, std::ios::cur);
}

int IsStreamAtEnd(void* user)
{
    auto* stream = static_cast<std::ifstream*>(user);
    return stream->peek() == std::ifstream::traits_type::eof() ? 1 : 0;
}
} // namespace

ImageHeader ReadImageHeader(const std::filesystem::path& file)
{
    ImageHeader header{};
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        return header;
    uint8_t leading[29] = {};
    stream.read(reinterpret_cast<char*>(leading), sizeof(leading));
    const std::optional<PngDeclaration> png =
        ReadPngDeclaration(std::span<const uint8_t>(leading, static_cast<size_t>(stream.gcount())));
    stream.clear();
    stream.seekg(0);

    const stbi_io_callbacks callbacks{ReadFromStream, SkipInStream, IsStreamAtEnd};
    int width = 0;
    int height = 0;
    int channels = 0;
    const bool read =
        stbi_info_from_callbacks(&callbacks, &stream, &width, &height, &channels) && width > 0 && height > 0;
    if (png)
    {
        // The IHDR is the declaration, including an extent stb_image refuses to read.
        header.Width = png->Width;
        header.Height = png->Height;
        header.Channels = read ? static_cast<uint32_t>(channels) : png->Channels;
        header.Valid = true;
        header.WithinDecoderLimits = png->WithinDecoderLimits;
        return header;
    }
    if (!read)
        return header;
    header.Width = static_cast<uint32_t>(width);
    header.Height = static_cast<uint32_t>(height);
    header.Channels = static_cast<uint32_t>(channels);
    header.Valid = true;
    header.WithinDecoderLimits = true;
    return header;
}

} // namespace GameEngine::Image
