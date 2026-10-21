#include "Image/Encode.h"

#include <stb_image_write.h>

#include <climits>
#include <fstream>

namespace GameEngine::Image
{
namespace
{
void AppendEncodedBytes(void* context, void* data, int size)
{
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

// stb_image_write takes int extents and an int row stride of width * 4 bytes.
bool IsEncodeExtent(size_t byteCount, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || width > static_cast<uint32_t>(INT_MAX / 4) ||
        height > static_cast<uint32_t>(INT_MAX))
        return false;
    return static_cast<uint64_t>(width) * height * 4 == byteCount;
}
} // namespace

std::vector<uint8_t> EncodePng(std::span<const uint8_t> rgba, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> png;
    if (!IsEncodeExtent(rgba.size(), width, height))
        return png;
    if (!stbi_write_png_to_func(AppendEncodedBytes, &png, static_cast<int>(width), static_cast<int>(height), 4,
                                rgba.data(), static_cast<int>(width * 4)))
        png.clear();
    return png;
}

bool WritePngFile(const std::filesystem::path& path, std::span<const uint8_t> rgba, uint32_t width,
                  uint32_t height)
{
    const std::vector<uint8_t> png = EncodePng(rgba, width, height);
    if (png.empty())
        return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    file.close();
    return !file.fail();
}

} // namespace GameEngine::Image
