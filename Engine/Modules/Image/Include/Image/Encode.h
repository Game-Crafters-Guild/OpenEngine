#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace GameEngine::Image
{

/// Encodes an RGBA8 image (an Image8's layout with 4 channels; the bytes are written
/// as they are, so sRGB-encoded color stays sRGB-encoded) as the bytes of a PNG file.
/// @return Empty when `rgba` does not hold width * height * 4 bytes, an extent is
/// zero or the encoder fails.
std::vector<uint8_t> EncodePng(std::span<const uint8_t> rgba, uint32_t width, uint32_t height);

/// Encodes as EncodePng and writes the PNG to `path`, replacing any file there. The
/// write is not atomic: a caller whose readers may open the file mid-write writes a
/// temporary sibling and publishes it.
/// @return False when the encode fails or the file cannot be written.
bool WritePngFile(const std::filesystem::path& path, std::span<const uint8_t> rgba, uint32_t width,
                  uint32_t height);

} // namespace GameEngine::Image
